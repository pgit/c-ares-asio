// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// Minimal integration of c-ares into an event loop.
//
// c-ares is told about nothing but its own sockets: it hands them to us through its socket state
// callback, we wait for readiness on each of them the way the runtime waits for anything, and
// c-ares is told when one of them is ready, using ares_process_fds().
//
// https://c-ares.org/docs/ares_process_fds.html
//
#pragma once

#include <cares_asio/error.hpp>
#include <cares_asio/export.hpp>
#include <cares_asio/runtime.hpp>

#include <ares.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#if CARES_ASIO_ASIO
#include <boost/asio/async_result.hpp>
#endif

namespace cares_asio
{

namespace detail
{
class Timer;
class tls_transport;
} // namespace detail

// --------------------------------------------------------------------------------------------------

/** One HTTPS resource record (RFC 9460).

    The record that tells a client which protocols an endpoint speaks before
    it connects. It comes in two forms: with a @ref priority of 0 it is an
    alias, where @ref target names the host to go on with, and with any other
    it describes a service, where @ref params says how to reach it.

    @see resolver::resolve_https
 */
struct https_record
{
   /** 0 for an alias. For a service, its place among the records of the
       name: a client tries the lowest first.
    */
   unsigned short priority = 0;

   /** The name to go on with. Empty, or ".", where that is the name the
       record was found under.
    */
   std::string target;

   /** The service parameters, in presentation form.

       One string for each parameter, the way a zone file has it:
       "alpn=h3,h2", "port=8443", "ipv4hint=192.0.2.1,192.0.2.2" -- or the
       key alone for one that has no value, like "no-default-alpn". Every key
       has a value syntax of its own and c-ares hands them over as opaque
       bytes, so there is not much to be gained from carrying them around
       any further apart.

       A key that c-ares has no name for reads "key" and its number, and a
       value it has no format for is given in hexadecimal, like that of
       "ech".
    */
   std::vector<std::string> params;
};

/** Whether a @ref resolver uses DNS over TLS (RFC 7858), and how.

    c-ares has no encrypted transport of its own. What the library does is
    to put a TLS session under the TCP connections that c-ares opens, which
    is all it takes to turn DNS over TCP into DNS over TLS.
 */
struct tls_options
{
   /** Sends every query over TLS: no UDP, and no TCP in the clear either. */
   bool enabled = false;

   /** The name of the server, for SNI and for its certificate to be checked
       against.

       c-ares knows its servers by address and nothing else, so the name has
       to come from here. Without one there is no SNI, and @ref verify still
       checks that the certificate is one to trust but not whose it is --
       which is no more than knowing that the server is somebody.
    */
   std::string hostname;

   /** Whether the certificate of the server is verified, against the trust
       store that the TLS library uses by default.

       Without this any certificate is taken, and with it anybody who is in
       a position to answer for the server. That is for testing.
    */
   bool verify = true;
};

/** How much a message from a @ref resolver matters.

    @see log_function
 */
enum class log_level
{
   /** What is going on: queries, the sockets of c-ares, timeouts. */
   debug,

   /** What may be worth knowing: a TLS session and whom it is with. */
   info,

   /** What did not go as it should, but did go on. */
   warning,

   /** What went wrong, and why: the reason a TLS handshake failed is one
       that only ever shows here.
    */
   error
};

/** Receives the messages of a @ref resolver.

    The library does not write anywhere by itself. What it has to say goes
    to a function like this one, with the level of the message and its text,
    and is not even put together where there is none.

    @see resolver::set_logger
 */
using log_function = std::function<void(log_level, std::string_view)>;

// --------------------------------------------------------------------------------------------------

/** Resolves host names with c-ares, as asynchronous operations of the
    runtime the library was built on.

    A resolver has one c-ares channel and drives it from the executor it was
    given. There is no thread behind it and nothing blocks: c-ares says which
    sockets it wants watched, the resolver waits for them the way the runtime
    waits for anything, and c-ares is told when one of them is ready.

    Any number of operations may be pending at a time. c-ares keeps the
    queries apart, over UDP as well as on the one connection that TCP or TLS
    take.

    @par The two runtimes
    Both lookups come as a coroutine, @ref resolve and @ref resolve_https,
    which is the spelling that an `CARES_ASIO_ASIO` and an
    `CARES_ASIO_COROSIO` build have in common: it yields the error and the
    result as a tuple. On Boost.Asio there is also @ref async_resolve and
    @ref async_resolve_https, which take any completion token. See
    `doc/corosio.md`.

    @par Thread safety
    Distinct objects: safe. Shared objects: unsafe. Where the context runs on
    more than one thread, make the executor of the resolver a strand and call
    the resolver from inside that strand.

    @par Cancellation
    Both operations can be cancelled -- by any type of cancellation on
    Boost.Asio, by the `std::stop_token` of the awaiting coroutine on
    corosio -- and then complete with @ref errors::canceled. But c-ares has
    no handle for a single request, so cancelling one operation cancels
    every operation that is pending on the resolver.

    @par Lifetime
    Destroying the resolver completes what is pending with
    @ref errors::canceled as well. The completions are not called from the
    destructor but handed to their executors, so the context has to run once
    more for them to be.

    @par Example
    @code
    boost::asio::awaitable<void> lookup(std::string host)
    {
       cares_asio::resolver resolver(co_await boost::asio::this_coro::executor);

       for (const auto& endpoint :
            co_await resolver.async_resolve(host, "443", boost::asio::use_awaitable))
          std::cout << endpoint << '\n';
    }
    @endcode

    The same on either runtime, where the error is wanted rather than an
    exception:

    @code
    cares_asio::Task<void> lookup(cares_asio::Executor executor, std::string host)
    {
       cares_asio::resolver resolver(executor);

       auto [ec, endpoints] = co_await resolver.resolve(host, "443");
       for (const auto& endpoint : endpoints)
          std::cout << endpoint << '\n';
    }
    @endcode
 */
class CARES_ASIO_EXPORT resolver
{
public:
   /** The type of the executor that a resolver runs on. */
   using executor_type = Executor;

   /** What @ref resolve comes back with: the endpoints of the host, best
       first.
    */
   using results_type = std::vector<Endpoint>;

   /** The completion signature of @ref resolve. */
   using resolve_signature = void(error_code, results_type);

   /** What @ref resolve_https comes back with: the records of the host, in
       no particular order.
    */
   using https_results_type = std::vector<https_record>;

   /** The completion signature of @ref resolve_https. */
   using https_signature = void(error_code, https_results_type);

   /** Makes a resolver that runs on `executor`.

       Which servers to ask, which domains to search and the rest of the
       configuration are those of the system, found the way c-ares finds
       them: in `/etc/resolv.conf` and what goes with it.

       With `tls.enabled`, every query goes out over DNS over TLS instead of
       UDP or TCP in the clear, and the servers are asked on port 853.

       @param executor Runs everything the resolver does, and every
       completion handler that has no executor of its own.

       @param tls Whether to use DNS over TLS, and how.

       @par Exceptions
       @ref system_error where c-ares cannot be set up, with its status code
       as @ref make_ares_error makes it, and `std::runtime_error` where the
       TLS library cannot.
    */
   explicit resolver(executor_type executor, const tls_options& tls = {});

   /** Completes every pending operation with @ref errors::canceled and
       closes the sockets.
    */
   ~resolver();

   //
   // A resolver stays where it was made: a moved-from one would still be the 'this' that the
   // socket state callback of its channel points at. Spelled out rather than left out, so that
   // whoever tries it reads it here.
   //
   resolver(const resolver&) = delete;
   resolver& operator=(const resolver&) = delete;
   resolver(resolver&&) = delete;
   resolver& operator=(resolver&&) = delete;

   /** Returns the executor that the resolver was made with.

       @return The executor.
    */
   const executor_type& get_executor() const noexcept { return m_executor; }

   /** Replaces the servers that the resolver asks.

       The list is one that `ares_set_servers_csv()` takes: addresses
       separated by commas, each with or without a port, as in "192.0.2.1",
       "192.0.2.1:5353" or "[2001:db8::1]:5353". An address without a port is
       asked on port 53, or on port 853 over TLS.

       A server has to be given by its address. A resolver that is to ask a
       server it only has the name of needs that name resolved first, by
       another resolver.

       @param servers The list. An empty one changes nothing.

       @return An error if the list cannot be made sense of, in which case
       the resolver keeps the servers it had.
    */
   error_code set_servers(std::string_view servers);

   /** Has `logger` called with every message from here on.

       The messages come from inside the operations of the resolver, so
       `logger` is called on a thread that runs the executor. It should come
       back quickly, and must not destroy the resolver.

       @param logger Receives the messages. An empty function turns them off
       again, which is how a resolver starts out.
    */
   void set_logger(log_function logger);

   /** Resolves a host name to the endpoints of a service.

       Looks up the addresses of `host`, of both families, the way
       `ares_getaddrinfo()` does for a stream socket: `/etc/hosts` and the
       search domains are taken into account, and the endpoints come sorted,
       the one to try first in front. An address in place of the name is
       taken as it is, without a query.

       This is the coroutine spelling, which both runtimes have: it yields
       the error and the endpoints as a tuple, the way the completion token
       `as_tuple(use_awaitable)` does on Boost.Asio.

       @par Errors
       Any of the c-ares status codes, as @ref make_ares_error makes them --
       `ARES_ENOTFOUND` for a name that does not exist and `ARES_ETIMEOUT`
       for servers that do not answer among them -- or @ref errors::canceled.

       @param host The name to resolve, or an address.

       @param service The port of every endpoint, by its number or by the
       name it has in `/etc/services`. Empty for port 0, which is also what a
       name that is not in there comes to -- that is no error.

       @return The error and the endpoints.
    */
   Task<std::tuple<error_code, results_type>> resolve(std::string host, std::string service);

   /** Resolves the HTTPS records (RFC 9460) of a host name.

       Asks for the records of type HTTPS that `host` has, search domains and
       all, the way @ref resolve asks for addresses. It is a question of its
       own, so the two can be asked at the same time.

       @par Errors
       As for @ref resolve. A name that has no such record completes with
       `ARES_ENODATA`, which is the common case rather than a problem.

       @param host The name to resolve.

       @return The error and the records.
    */
   Task<std::tuple<error_code, https_results_type>> resolve_https(std::string host);

#if CARES_ASIO_ASIO

   /** A completion handler of @ref async_resolve, whatever its type. */
   using resolve_handler = detail::Completion<resolve_signature>;

   /** A completion handler of @ref async_resolve_https, whatever its type. */
   using https_handler = detail::Completion<https_signature>;

   /** Resolves a host name to the endpoints of a service, with a completion
       token.

       The same lookup as @ref resolve, as an ASIO asynchronous operation.
       Boost.Asio only.

       @par Completion signature
       @code
       void(boost::system::error_code, results_type)
       @endcode

       @par Errors
       As for @ref resolve.

       @param host The name to resolve, or an address.

       @param service The port of every endpoint, as @ref resolve takes it.

       @param token The completion token: a callback,
       `boost::asio::use_awaitable`, `boost::asio::deferred` or any other.
       The handler is never called from inside this function.

       @return Whatever the completion token makes of the operation.
    */
   template <boost::asio::completion_token_for<resolve_signature> CompletionToken>
   auto async_resolve(std::string_view host, std::string_view service, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, resolve_signature>(
         [this](resolve_handler handler, std::string host, std::string service) {
            start_resolve(std::move(host), std::move(service), std::move(handler));
         },
         token, std::string(host), std::string(service));
   }

   /** Resolves the HTTPS records (RFC 9460) of a host name, with a
       completion token.

       The same lookup as @ref resolve_https, as an ASIO asynchronous
       operation. Boost.Asio only.

       @par Completion signature
       @code
       void(boost::system::error_code, https_results_type)
       @endcode

       @par Errors
       As for @ref resolve_https.

       @param host The name to resolve.

       @param token The completion token: a callback,
       `boost::asio::use_awaitable`, `boost::asio::deferred` or any other.
       The handler is never called from inside this function.

       @return Whatever the completion token makes of the operation.
    */
   template <boost::asio::completion_token_for<https_signature> CompletionToken>
   auto async_resolve_https(std::string_view host, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, https_signature>(
         [this](https_handler handler, std::string host) {
            start_resolve_https(std::move(host), std::move(handler));
         },
         token, std::string(host));
   }

#endif // CARES_ASIO_ASIO

private:
   struct socket_watch;

   void start_resolve(std::string host, std::string service,
                      detail::Completion<resolve_signature> handler);
   void start_resolve_https(std::string host, detail::Completion<https_signature> handler);

   static void socket_state_callback(void* data, ares_socket_t fd, int readable, int writable);
   void on_socket_state(ares_socket_t fd, bool readable, bool writable);

   void arm(const std::shared_ptr<socket_watch>& socket);
   void on_socket_event(const std::shared_ptr<socket_watch>& socket, unsigned int event,
                        const error_code& ec);

   void update_timeout();

   executor_type m_executor;
   std::unique_ptr<detail::Timer> m_timer;
   ares_channel_t* m_channel = nullptr;
   log_function m_logger; // declared ahead of the transport, which keeps a reference to it
   std::unique_ptr<detail::tls_transport> m_tls; // null unless DNS over TLS was asked for
   std::map<ares_socket_t, std::shared_ptr<socket_watch>> m_sockets;
};

} // namespace cares_asio
