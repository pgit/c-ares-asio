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

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
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

   // what the TLS handshake is interested in, which overrides c-ares until there is a session
   TlsTransport::Interest tlsInterest = TlsTransport::Interest::None;
};

// --------------------------------------------------------------------------------------------------

namespace
{

std::string_view trim(std::string_view text)
{
   const auto first = text.find_first_not_of(" \t");
   if (first == std::string_view::npos)
      return {};

   return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

//
// Whether a c-ares server entry carries an explicit port. "[::1]:853" has it behind the bracket,
// and "192.0.2.1:853" is the only unbracketed form that can have one -- a bare IPv6 address has
// several colons and no port at all.
//
bool hasPort(std::string_view server)
{
   const auto colon = server.rfind(':');
   if (colon == std::string_view::npos || colon + 1 == server.size())
      return false;

   const auto bracket = server.rfind(']');
   if (bracket == std::string_view::npos ? server.find(':') != colon : colon < bracket)
      return false;

   return std::ranges::all_of(server.substr(colon + 1),
                              [](char c) { return c >= '0' && c <= '9'; });
}

//
// Puts 'port' on every entry of a server list that has none, or on all of them when 'force' is
// set. Entries in the dns:// URI form are left alone, c-ares knows what to do with those.
//
std::string withPort(std::string_view servers, unsigned port, bool force)
{
   std::string result;

   for (size_t begin = 0; begin <= servers.size();)
   {
      const auto comma = servers.find(',', begin);
      auto server = trim(servers.substr(begin, comma - begin));
      begin = comma == std::string_view::npos ? servers.size() + 1 : comma + 1;

      if (server.empty())
         continue;

      if (!result.empty())
         result += ',';

      const bool ported = hasPort(server);
      if (server.contains("://") || (ported && !force))
      {
         result += server;
         continue;
      }

      if (ported)
         server = server.substr(0, server.rfind(':'));

      //
      // A bare IPv6 address only needs its brackets once there is a port behind it.
      //
      if (!server.starts_with('[') && server.contains(':'))
         result += std::format("[{}]:{}", server, port);
      else
         result += std::format("{}:{}", server, port);
   }

   return result;
}

//
// A single ares_getaddrinfo() request, kept alive until its callback fires.
//
struct Request
{
   any_io_executor executor;
   AresResolver::ResolveHandler handler;
};

//
// The same, for a single ares_search_dnsrec() request.
//
struct HttpsRequest
{
   any_io_executor executor;
   AresResolver::HttpsHandler handler;
};

//
// Renders one SvcParam value. c-ares hands these over as opaque bytes and tells us, through
// ares_dns_opt_get_datatype(), which of the RFC 9460 wire formats to read them as.
//
std::string toText(unsigned short param, ares_dns_opt_datatype_t datatype,
                   const unsigned char* value, size_t length)
{
   switch (datatype)
   {
   case ARES_OPT_DATATYPE_NONE:
      return {}; // a key that is its own value, like "no-default-alpn"

   case ARES_OPT_DATATYPE_STR_LIST:
   {
      //
      // "alpn" and friends: each string prefixed with a single octet holding its length.
      //
      std::string result;
      for (size_t i = 0; i < length;)
      {
         const size_t size = value[i++];
         if (i + size > length)
            break;

         if (!result.empty())
            result += ',';
         result.append(reinterpret_cast<const char*>(value + i), size);
         i += size;
      }

      return result;
   }

   case ARES_OPT_DATATYPE_U16:
      return length < 2 ? std::string() : std::format("{}", (value[0] << 8) | value[1]);

   case ARES_OPT_DATATYPE_U16_LIST:
   {
      //
      // "mandatory" is the only SvcParam built this way, and what it lists is other SvcParam
      // keys -- by number on the wire, but by name anywhere a person is going to read them.
      //
      std::string result;
      for (size_t i = 0; i + 2 <= length; i += 2)
      {
         const auto key = static_cast<unsigned short>((value[i] << 8) | value[i + 1]);
         const auto* name = param == ARES_SVCB_PARAM_MANDATORY
                               ? ares_dns_opt_get_name(ARES_RR_HTTPS_PARAMS, key)
                               : nullptr;

         if (!result.empty())
            result += ',';
         result += name ? name : std::format("{}", key);
      }

      return result;
   }

   case ARES_OPT_DATATYPE_INADDR4_LIST:
   {
      std::string result;
      for (size_t i = 0; i + 4 <= length; i += 4)
      {
         ip::address_v4::bytes_type bytes;
         std::memcpy(bytes.data(), value + i, bytes.size());
         if (!result.empty())
            result += ',';
         result += ip::address_v4(bytes).to_string();
      }

      return result;
   }

   case ARES_OPT_DATATYPE_INADDR6_LIST:
   {
      std::string result;
      for (size_t i = 0; i + 16 <= length; i += 16)
      {
         ip::address_v6::bytes_type bytes;
         std::memcpy(bytes.data(), value + i, bytes.size());
         if (!result.empty())
            result += ',';
         result += ip::address_v6(bytes).to_string();
      }

      return result;
   }

   default:
   {
      //
      // "ech" and anything defined after this was written: show it rather than drop it.
      //
      std::string result;
      for (size_t i = 0; i < length; ++i)
         result += std::format("{:02x}", value[i]);

      return result;
   }
   }
}

//
// Renders SvcParam 'index' of 'rr' as "key" or "key=value".
//
std::string toText(const ares_dns_rr_t* rr, size_t index)
{
   const unsigned char* value = nullptr;
   size_t length = 0;

   const auto param = ares_dns_rr_get_opt(rr, ARES_RR_HTTPS_PARAMS, index, &value, &length);
   if (param == 65535) // c-ares' way of saying it did not like the question
      return {};

   const auto* name = ares_dns_opt_get_name(ARES_RR_HTTPS_PARAMS, param);
   const auto key = name ? std::string(name) : std::format("key{}", param);

   const auto text =
      value ? toText(param, ares_dns_opt_get_datatype(ARES_RR_HTTPS_PARAMS, param), value, length)
            : std::string();

   return text.empty() ? key : std::format("{}={}", key, text);
}

void httpsCallback(void* arg, ares_status_t status, size_t timeouts,
                   const ares_dns_record_t* dnsrec)
{
   std::unique_ptr<HttpsRequest> request(static_cast<HttpsRequest*>(arg));
   spdlog::debug("ares_search_dnsrec: {} ({} timeout(s))", ares_strerror(static_cast<int>(status)),
                 timeouts);

   const auto ec = (status == ARES_ECANCELLED || status == ARES_EDESTRUCTION)
                      ? error_code(error::operation_aborted)
                      : make_ares_error(static_cast<int>(status));

   if (auto slot = get_associated_cancellation_slot(request->handler); slot.is_connected())
      slot.clear();

   AresResolver::HttpsResults records;
   if (dnsrec && status == ARES_SUCCESS)
   {
      const auto count = ares_dns_record_rr_cnt(dnsrec, ARES_SECTION_ANSWER);
      for (size_t i = 0; i < count; ++i)
      {
         const auto* rr = ares_dns_record_rr_get_const(dnsrec, ARES_SECTION_ANSWER, i);

         //
         // The answer section also carries whatever CNAMEs led here, which are not ours.
         //
         if (!rr || ares_dns_rr_get_type(rr) != ARES_REC_TYPE_HTTPS)
            continue;

         HttpsRecord record;
         record.priority = ares_dns_rr_get_u16(rr, ARES_RR_HTTPS_PRIORITY);
         if (const auto* target = ares_dns_rr_get_str(rr, ARES_RR_HTTPS_TARGET))
            record.target = target;

         const auto params = ares_dns_rr_get_opt_cnt(rr, ARES_RR_HTTPS_PARAMS);
         for (size_t param = 0; param < params; ++param)
            if (auto text = toText(rr, param); !text.empty())
               record.params.push_back(std::move(text));

         records.push_back(std::move(record));
      }
   }

   post(request->executor, append(std::move(request->handler), ec, std::move(records)));
}

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

AresResolver::AresResolver(any_io_executor executor, const TlsOptions& tls)
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

   int optmask = ARES_OPT_SOCK_STATE_CB;

   //
   // DNS over TLS is DNS over TCP with a TLS session under it, so UDP is off the table
   // (ARES_FLAG_USEVC). Keeping the connection up once the queries drain is worth a lot more here
   // than it is for plain TCP: it is a handshake per name otherwise.
   //
   if (tls.enabled)
   {
      options.flags = ARES_FLAG_USEVC | ARES_FLAG_STAYOPEN;
      optmask |= ARES_OPT_FLAGS;
   }

   if (int status = ares_init_options(&m_channel, &options, optmask); status != ARES_SUCCESS)
      throw boost::system::system_error(make_ares_error(status), "ares_init_options");

   if (tls.enabled)
   {
      m_tls = std::make_unique<TlsTransport>(tls);
      m_tls->install(m_channel); // before the channel has had a chance to open anything
   }
}

boost::system::error_code AresResolver::setServers(std::string_view servers)
{
   std::string list(servers);

   if (m_tls)
   {
      //
      // Nothing listens for DoT on port 53. The servers c-ares read from /etc/resolv.conf come
      // with :53 already on them and are moved wholesale, while a port spelled out on --server is
      // taken at face value.
      //
      const bool fromResolvConf = list.empty();
      if (fromResolvConf)
      {
         const std::unique_ptr<char, decltype(&ares_free_string)> current(
            ares_get_servers_csv(m_channel), &ares_free_string);
         if (!current)
            return make_ares_error(ARES_ENOMEM);

         list = current.get();
      }

      list = withPort(list, 853, fromResolvConf);
   }

   if (list.empty())
      return {};

   spdlog::debug("using DNS server(s) {}", list);
   return make_ares_error(ares_set_servers_csv(m_channel, list.c_str()));
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

void AresResolver::startHttpsLookup(std::string host, HttpsHandler handler)
{
   if (auto slot = get_associated_cancellation_slot(handler); slot.is_connected())
      slot.assign([this](cancellation_type) { //
         ares_cancel(m_channel);
      });

   auto request = std::make_unique<HttpsRequest>(m_executor, std::move(handler));

   //
   // ares_search_dnsrec() wants the question as a record rather than as a name, so that it can
   // swap the name out as it works down the search list.
   //
   ares_dns_record_t* query = nullptr;
   auto status =
      ares_dns_record_create(&query, 0, ARES_FLAG_RD, ARES_OPCODE_QUERY, ARES_RCODE_NOERROR);
   if (status == ARES_SUCCESS)
      status = ares_dns_record_query_add(query, host.c_str(), ARES_REC_TYPE_HTTPS, ARES_CLASS_IN);

   if (status == ARES_SUCCESS)
   {
      spdlog::debug("ares_search_dnsrec({}, HTTPS)...", host);

      //
      // The callback owns the request from here on, and may well have run and freed it before
      // this call returns -- ARES_EFORMERR is the one status that does not reach it.
      //
      status = ares_search_dnsrec(m_channel, query, &httpsCallback, request.get());
      if (status != ARES_EFORMERR)
      {
         request.release();
         ares_dns_record_destroy(query);
         updateTimeout();
         return;
      }
   }

   ares_dns_record_destroy(query);

   //
   // Nothing was enqueued and nothing will call the callback, so the request is still ours to
   // finish. Both ways of getting here mean a bug or an allocation failure right in this
   // function, not anything the name could have done.
   //
   spdlog::error("HTTPS query for {}: {}", host, ares_strerror(status));
   post(m_executor, append(std::move(request->handler), make_ares_error(status), HttpsResults()));
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

   //
   // While the handshake is in flight the socket is ours, not c-ares'. Waiting on what c-ares
   // asked for would be wrong in both directions: a handshake that wants to read gets starved,
   // and one that wants to read while c-ares waits for writability spins, because a connected
   // socket is writable almost all of the time.
   //
   const bool handshaking = socket->tlsInterest != TlsTransport::Interest::None;
   const bool wantRead =
      handshaking ? socket->tlsInterest == TlsTransport::Interest::Read : socket->wantRead;
   const bool wantWrite =
      handshaking ? socket->tlsInterest == TlsTransport::Interest::Write : socket->wantWrite;

   if (wantRead && !socket->waitingRead)
   {
      socket->waitingRead = true;
      socket->descriptor.async_wait(posix::stream_descriptor::wait_read,
                                    [this, weak = std::weak_ptr(socket)](const error_code& ec) { //
         onSocketEvent(weak, ARES_FD_EVENT_READ, ec);
      });
   }

   if (wantWrite && !socket->waitingWrite)
   {
      socket->waitingWrite = true;
      socket->descriptor.async_wait(posix::stream_descriptor::wait_write,
                                    [this, weak = std::weak_ptr(socket)](const error_code& ec) { //
         onSocketEvent(weak, ARES_FD_EVENT_WRITE, ec);
      });
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
   // Get the TCP connect and the TLS handshake out of the way first -- c-ares is only ever shown
   // a socket it can talk DNS on, and until then this readiness is none of its business.
   //
   if (m_tls)
   {
      socket->tlsInterest = m_tls->advance(socket->fd, event == ARES_FD_EVENT_WRITE);
      if (socket->tlsInterest != TlsTransport::Interest::None)
      {
         arm(socket);
         return;
      }
   }

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
