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

#include <cares_asio/error.hpp>
#include <cares_asio/export.hpp>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <ares.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cares_asio
{

namespace detail
{
class tls_transport;
}

// --------------------------------------------------------------------------------------------------

//
// One HTTPS resource record (RFC 9460). A 'priority' of 0 is the alias form, where 'target' names
// the host to continue at; anything else is the service form, where 'params' describes how to
// reach the endpoint.
//
struct https_record
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

//
// DNS over TLS (RFC 7858), which is off unless 'enabled' says otherwise.
//
struct tls_options
{
   bool enabled = false;
   std::string hostname; // for SNI and certificate verification, empty if none was given
   bool verify = true;
};

//
// What the resolver has to say about its work goes to a function like this one, if set_logger()
// was given any: the queries it starts and how they end, the sockets c-ares opens, and what the
// TLS handshake negotiated or why it failed. The library itself writes nowhere.
//
enum class log_level
{
   debug,
   info,
   warning,
   error
};

using log_function = std::function<void(log_level, std::string_view)>;

// --------------------------------------------------------------------------------------------------

class CARES_ASIO_EXPORT resolver
{
public:
   using executor_type = boost::asio::any_io_executor;

   using results_type = std::vector<boost::asio::ip::tcp::endpoint>;
   using resolve_signature = void(boost::system::error_code, results_type);
   using resolve_handler = boost::asio::any_completion_handler<resolve_signature>;

   using https_results_type = std::vector<https_record>;
   using https_signature = void(boost::system::error_code, https_results_type);
   using https_handler = boost::asio::any_completion_handler<https_signature>;

   //
   // With 'tls' enabled every query goes out over DNS over TLS instead of plain UDP/TCP, and the
   // servers move to port 853 unless set_servers() names a port explicitly.
   //
   explicit resolver(executor_type executor, const tls_options& tls = {});
   ~resolver();

   resolver(const resolver&) = delete;
   resolver& operator=(const resolver&) = delete;

   const executor_type& get_executor() const noexcept { return m_executor; }

   //
   // Replaces the servers from /etc/resolv.conf, as a comma separated list of addresses with an
   // optional port ("192.0.2.1", "192.0.2.1:5353", "[::1]:5353"). Does nothing if 'servers' is
   // empty, and leaves the channel untouched if the list cannot be parsed.
   //
   boost::system::error_code set_servers(std::string_view servers);

   //
   // Has 'logger' called with every message from here on, on whichever thread runs the executor.
   // An empty function turns it off again, which is what a resolver starts out with.
   //
   void set_logger(log_function logger);

   //
   // Resolves 'host' for 'service' (a port number or service name), like ares_getaddrinfo() with
   // AF_UNSPEC/SOCK_STREAM hints. The completion handler is invoked on this resolver's executor.
   //
   template <boost::asio::completion_token_for<resolve_signature> CompletionToken>
   auto async_resolve(std::string_view host, std::string_view service, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, resolve_signature>(
         [this](resolve_handler handler, std::string host, std::string service) {
            start_resolve(std::move(host), std::move(service), std::move(handler));
         },
         token, std::string(host), std::string(service));
   }

   //
   // Looks up the HTTPS record (RFC 9460) for 'host', search domains and all, the way
   // async_resolve() looks up its addresses. A host that has none completes with ARES_ENODATA,
   // which is the common case rather than a problem.
   //
   template <boost::asio::completion_token_for<https_signature> CompletionToken>
   auto async_resolve_https(std::string_view host, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, https_signature>(
         [this](https_handler handler, std::string host) {
            start_resolve_https(std::move(host), std::move(handler));
         },
         token, std::string(host));
   }

private:
   struct socket_watch;

   void start_resolve(std::string host, std::string service, resolve_handler handler);
   void start_resolve_https(std::string host, https_handler handler);

   static void socket_state_callback(void* data, ares_socket_t fd, int readable, int writable);
   void on_socket_state(ares_socket_t fd, bool readable, bool writable);

   void arm(const std::shared_ptr<socket_watch>& socket);
   void on_socket_event(const std::shared_ptr<socket_watch>& socket, unsigned int event,
                        const boost::system::error_code& ec);

   void update_timeout();

   executor_type m_executor;
   std::shared_ptr<boost::asio::steady_timer> m_timer; // shared for update_timeout() to tell
   ares_channel_t* m_channel = nullptr;
   log_function m_logger; // declared ahead of the transport, which keeps a reference to it
   std::unique_ptr<detail::tls_transport> m_tls; // null unless DNS over TLS was asked for
   std::map<ares_socket_t, std::shared_ptr<socket_watch>> m_sockets;
};

} // namespace cares_asio
