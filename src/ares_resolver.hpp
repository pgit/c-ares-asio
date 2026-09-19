//
// Minimal integration of c-ares into an ASIO event loop.
//
// c-ares is told about nothing but its own sockets: it hands them to us through its socket state
// callback, we wrap each of them in a (non-owning) ASIO descriptor and feed readiness back into
// the library using ares_process_fds().
//
// https://c-ares.org/docs/ares_process_fds.html
//
#pragma once

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <ares.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

//
// Error category wrapping the ARES_* status codes, so that they can travel as an error_code.
//
const boost::system::error_category& ares_category();
boost::system::error_code make_ares_error(int status);

// --------------------------------------------------------------------------------------------------

class AresResolver
{
public:
   using Results = std::vector<boost::asio::ip::tcp::endpoint>;
   using Signature = void(boost::system::error_code, Results);
   using ResolveHandler = boost::asio::any_completion_handler<Signature>;

   explicit AresResolver(boost::asio::any_io_executor executor);
   ~AresResolver();

   //
   // Replaces the servers from /etc/resolv.conf, as a comma separated list of addresses with an
   // optional port ("192.0.2.1", "192.0.2.1:5353", "[::1]:5353"). Does nothing if 'servers' is
   // empty, and leaves the channel untouched if the list cannot be parsed.
   //
   boost::system::error_code setServers(std::string_view servers);

   AresResolver(const AresResolver&) = delete;
   AresResolver& operator=(const AresResolver&) = delete;

   //
   // Resolves 'host' for 'service' (a port number or service name), like ares_getaddrinfo() with
   // AF_UNSPEC/SOCK_STREAM hints. The completion handler is invoked on this resolver's executor.
   //
   template <typename CompletionToken>
   auto async_resolve(std::string_view host, std::string_view service, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Signature>(
         [this](ResolveHandler handler, std::string host, std::string service)
      { startResolve(std::move(host), std::move(service), std::move(handler)); }, //
         token, std::string(host), std::string(service));
   }

private:
   struct Socket;

   void startResolve(std::string host, std::string service, ResolveHandler handler);

   static void socketStateCallback(void* data, ares_socket_t fd, int readable, int writable);
   void onSocketState(ares_socket_t fd, bool readable, bool writable);

   void arm(const std::shared_ptr<Socket>& socket);
   void onSocketEvent(const std::weak_ptr<Socket>& weak, unsigned int event,
                      const boost::system::error_code& ec);

   void updateTimeout();

   boost::asio::any_io_executor m_executor;
   boost::asio::steady_timer m_timer;
   ares_channel_t* m_channel = nullptr;
   std::map<ares_socket_t, std::shared_ptr<Socket>> m_sockets;
};
