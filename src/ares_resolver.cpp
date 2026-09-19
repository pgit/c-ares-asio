#include "ares_resolver.hpp"

#include <boost/asio/append.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/system_error.hpp>

#include <spdlog/spdlog.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstring>
#include <optional>

using namespace boost::asio;
using boost::system::error_code;

// --------------------------------------------------------------------------------------------------

const boost::system::error_category& ares_category()
{
   static const struct Category : boost::system::error_category
   {
      const char* name() const noexcept override { return "c-ares"; }
      std::string message(int value) const override { return ares_strerror(value); }
   } category;

   return category;
}

boost::system::error_code make_ares_error(int status)
{
   return {status, ares_category()}; // ARES_SUCCESS is 0, so this stays falsy on success
}

// --------------------------------------------------------------------------------------------------

//
// One socket c-ares has opened. ASIO is only used to wait for readiness on it -- the socket itself
// belongs to c-ares, which is why the descriptor is released again instead of being closed.
//
struct AresResolver::Socket
{
   Socket(const any_io_executor& executor, ares_socket_t fd) : fd(fd), descriptor(executor, fd) {}
   ~Socket() { detach(); }

   void detach()
   {
      if (descriptor.is_open())
         descriptor.release(); // cancels pending waits, leaves the file descriptor alone
   }

   ares_socket_t fd;
   posix::stream_descriptor descriptor;

   bool wantRead = false; // what c-ares is interested in
   bool wantWrite = false; //
   bool waitingRead = false; // what we have handed to ASIO
   bool waitingWrite = false; //
};

// --------------------------------------------------------------------------------------------------

namespace
{

//
// A single ares_getaddrinfo() request, kept alive until its callback fires.
//
struct Request
{
   any_io_executor executor;
   AresResolver::ResolveHandler handler;
};

std::optional<ip::tcp::endpoint> toEndpoint(const ares_addrinfo_node& node)
{
   if (node.ai_family == AF_INET && node.ai_addrlen >= sizeof(sockaddr_in))
   {
      sockaddr_in sa{};
      std::memcpy(&sa, node.ai_addr, sizeof(sa));

      ip::address_v4::bytes_type bytes;
      std::memcpy(bytes.data(), &sa.sin_addr, bytes.size());
      return ip::tcp::endpoint(ip::address_v4(bytes), ntohs(sa.sin_port));
   }

   if (node.ai_family == AF_INET6 && node.ai_addrlen >= sizeof(sockaddr_in6))
   {
      sockaddr_in6 sa{};
      std::memcpy(&sa, node.ai_addr, sizeof(sa));

      ip::address_v6::bytes_type bytes;
      std::memcpy(bytes.data(), &sa.sin6_addr, bytes.size());
      return ip::tcp::endpoint(ip::address_v6(bytes, sa.sin6_scope_id), ntohs(sa.sin6_port));
   }

   return std::nullopt;
}

void addrinfoCallback(void* arg, int status, int timeouts, ares_addrinfo* result)
{
   std::unique_ptr<Request> request(static_cast<Request*>(arg));
   spdlog::debug("ares_getaddrinfo: {} ({} timeout(s))", ares_strerror(status), timeouts);

   //
   // Report the c-ares way of being cancelled as the ASIO one, so that the operation behaves like
   // any other cancelled ASIO operation.
   //
   const auto ec = (status == ARES_ECANCELLED || status == ARES_EDESTRUCTION)
                      ? error_code(error::operation_aborted)
                      : make_ares_error(status);

   if (auto slot = get_associated_cancellation_slot(request->handler); slot.is_connected())
      slot.clear();

   AresResolver::Results endpoints;
   if (result)
   {
      for (const auto* node = result->nodes; node; node = node->ai_next)
         if (auto endpoint = toEndpoint(*node))
            endpoints.push_back(*endpoint);

      ares_freeaddrinfo(result);
   }

   //
   // c-ares calls us from inside ares_getaddrinfo() or ares_process_fds(), which may well be a
   // synchronous completion -- post to stay clear of the "never invoke the handler inline" rule.
   //
   post(request->executor, append(std::move(request->handler), ec, std::move(endpoints)));
}

} // namespace

// --------------------------------------------------------------------------------------------------

AresResolver::AresResolver(any_io_executor executor)
   : m_executor(std::move(executor)), m_timer(m_executor)
{
   if (int status = ares_library_init(ARES_LIB_INIT_ALL); status != ARES_SUCCESS)
      throw boost::system::system_error(make_ares_error(status), "ares_library_init");

   //
   // The socket state callback is what makes this work without c-ares' own event thread: it tells
   // us which sockets to watch and for what.
   //
   ares_options options{};
   options.sock_state_cb = &AresResolver::socketStateCallback;
   options.sock_state_cb_data = this;

   if (int status = ares_init_options(&m_channel, &options, ARES_OPT_SOCK_STATE_CB);
       status != ARES_SUCCESS)
      throw boost::system::system_error(make_ares_error(status), "ares_init_options");
}

boost::system::error_code AresResolver::setServers(std::string_view servers)
{
   if (servers.empty())
      return {};

   spdlog::debug("using DNS server(s) {}", servers);
   return make_ares_error(ares_set_servers_csv(m_channel, std::string(servers).c_str()));
}

AresResolver::~AresResolver()
{
   ares_destroy(m_channel); // fails pending queries with ARES_EDESTRUCTION and closes all sockets
   m_sockets.clear();
   m_timer.cancel();
   ares_library_cleanup();
}

// --------------------------------------------------------------------------------------------------

void AresResolver::startResolve(std::string host, std::string service, ResolveHandler handler)
{
   //
   // There is no handle for an individual ares_getaddrinfo() request, so cancelling one takes the
   // whole channel with it. Good enough while queries are issued one at a time.
   //
   if (auto slot = get_associated_cancellation_slot(handler); slot.is_connected())
      slot.assign([this](cancellation_type) { //
         ares_cancel(m_channel);
      });

   auto request = std::make_unique<Request>(m_executor, std::move(handler));

   ares_addrinfo_hints hints{};
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;

   spdlog::debug("ares_getaddrinfo({}, {})...", host, service);
   ares_getaddrinfo(m_channel, host.c_str(), service.empty() ? nullptr : service.c_str(), &hints,
                    &addrinfoCallback, request.release());

   updateTimeout();
}

// --------------------------------------------------------------------------------------------------

void AresResolver::socketStateCallback(void* data, ares_socket_t fd, int readable, int writable)
{
   static_cast<AresResolver*>(data)->onSocketState(fd, readable != 0, writable != 0);
}

void AresResolver::onSocketState(ares_socket_t fd, bool readable, bool writable)
{
   spdlog::debug("socket state: fd={} readable={} writable={}", fd, readable, writable);

   //
   // Neither readable nor writable means c-ares is about to close the socket. The file descriptor
   // is still valid at this point, so this is our chance to get it out of ASIO's reactor.
   //
   if (!readable && !writable)
   {
      if (auto it = m_sockets.find(fd); it != m_sockets.end())
      {
         it->second->detach();
         m_sockets.erase(it);
      }
      return;
   }

   auto& socket = m_sockets[fd];
   if (!socket)
      socket = std::make_shared<Socket>(m_executor, fd);

   socket->wantRead = readable;
   socket->wantWrite = writable;
   arm(socket);
}

void AresResolver::arm(const std::shared_ptr<Socket>& socket)
{
   if (!socket->descriptor.is_open())
      return;

   if (socket->wantRead && !socket->waitingRead)
   {
      socket->waitingRead = true;
      socket->descriptor.async_wait(posix::stream_descriptor::wait_read,
                                    [this, weak = std::weak_ptr(socket)](const error_code& ec)
      { onSocketEvent(weak, ARES_FD_EVENT_READ, ec); });
   }

   if (socket->wantWrite && !socket->waitingWrite)
   {
      socket->waitingWrite = true;
      socket->descriptor.async_wait(posix::stream_descriptor::wait_write,
                                    [this, weak = std::weak_ptr(socket)](const error_code& ec)
      { onSocketEvent(weak, ARES_FD_EVENT_WRITE, ec); });
   }
}

void AresResolver::onSocketEvent(const std::weak_ptr<Socket>& weak, unsigned int event,
                                 const error_code& ec)
{
   auto socket = weak.lock();
   if (!socket)
      return; // socket is gone, and so is c-ares' interest in it

   (event == ARES_FD_EVENT_READ ? socket->waitingRead : socket->waitingWrite) = false;

   if (ec)
      return; // operation_aborted, i.e. the descriptor was detached underneath us

   //
   // ares_process_fds() takes an array, and processing several sockets in one call is cheaper than
   // one call per socket -- with one ASIO completion handler per descriptor, we get one at a time.
   //
   const ares_fd_events_t events{socket->fd, event};
   ares_process_fds(m_channel, &events, 1, ARES_PROCESS_FLAG_NONE);

   //
   // The socket state callback only fires on *changes*, so anything c-ares is still interested in
   // has to be re-armed here. If the socket was closed during processing, arm() does nothing.
   //
   arm(socket);
   updateTimeout();
}

// --------------------------------------------------------------------------------------------------

void AresResolver::updateTimeout()
{
   timeval tv{};
   if (ares_timeout(m_channel, nullptr, &tv) == nullptr)
   {
      m_timer.cancel(); // nothing in flight
      return;
   }

   using namespace std::chrono;
   m_timer.expires_after(seconds(tv.tv_sec) + microseconds(tv.tv_usec));
   m_timer.async_wait([this](const error_code& ec)
   {
      if (ec)
         return; // superseded by a later timeout, or cancelled

      spdlog::debug("query timeout");
      ares_process_fds(m_channel, nullptr, 0, ARES_PROCESS_FLAG_NONE);
      updateTimeout();
   });
}
