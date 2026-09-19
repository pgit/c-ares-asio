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

#include "tls_transport.hpp"

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

//
// One HTTPS resource record (RFC 9460). A 'priority' of 0 is the alias form, where 'target' names
// the host to continue at; anything else is the service form, where 'params' describes how to
// reach the endpoint.
//
struct HttpsRecord
{
   unsigned short priority = 0;
   std::string target; // empty, or ".", for the record's own name

   //
   // The SvcParams, already in presentation form ("alpn=h3,h2", "ipv4hint=104.20.23.154"). Every
   // key has its own value syntax and c-ares hands them over as opaque bytes, so there is not
   // much to be gained from carrying them around any further apart.
   //
   std::vector<std::string> params;
};

// --------------------------------------------------------------------------------------------------

class AresResolver
{
public:
   using Results = std::vector<boost::asio::ip::tcp::endpoint>;
   using Signature = void(boost::system::error_code, Results);
   using ResolveHandler = boost::asio::any_completion_handler<Signature>;

   using HttpsResults = std::vector<HttpsRecord>;
   using HttpsSignature = void(boost::system::error_code, HttpsResults);
   using HttpsHandler = boost::asio::any_completion_handler<HttpsSignature>;

   //
   // With 'tls' enabled every query goes out over DNS over TLS instead of plain UDP/TCP, and the
   // servers move to port 853 unless --server named a port explicitly.
   //
   explicit AresResolver(boost::asio::any_io_executor executor, const TlsOptions& tls = {});
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
      { //
         startResolve(std::move(host), std::move(service), std::move(handler));
      },
      token, std::string(host), std::string(service));
   }

   //
   // Looks up the HTTPS record (RFC 9460) for 'host', search domains and all, the way
   // async_resolve() looks up its addresses. A host that has none completes with ARES_ENODATA,
   // which is the common case rather than a problem.
   //
   template <typename CompletionToken>
   auto async_lookupHttps(std::string_view host, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, HttpsSignature>(
         [this](HttpsHandler handler, std::string host)
      { //
         startHttpsLookup(std::move(host), std::move(handler));
      },
      token, std::string(host));
   }

private:
   struct Socket;

   void startResolve(std::string host, std::string service, ResolveHandler handler);
   void startHttpsLookup(std::string host, HttpsHandler handler);

   static void socketStateCallback(void* data, ares_socket_t fd, int readable, int writable);
   void onSocketState(ares_socket_t fd, bool readable, bool writable);

   void arm(const std::shared_ptr<Socket>& socket);
   void onSocketEvent(const std::weak_ptr<Socket>& weak, unsigned int event,
                      const boost::system::error_code& ec);

   void updateTimeout();

   boost::asio::any_io_executor m_executor;
   boost::asio::steady_timer m_timer;
   ares_channel_t* m_channel = nullptr;
   std::unique_ptr<TlsTransport> m_tls; // null unless DNS over TLS was asked for
   std::map<ares_socket_t, std::shared_ptr<Socket>> m_sockets;
};
