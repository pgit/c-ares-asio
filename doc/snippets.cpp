// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The code of the programmer's guide. The guide takes it from here, from one '[name]' comment to
// the next, and it is compiled along with everything else, so what the guide shows is what the
// library takes. See programmers-guide.rst.
//
#include <c_ares_async.hpp>

#if C_ARES_ASYNC_ASIO
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#else
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run.hpp>
#include <boost/corosio/timeout.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace snippets
{

// --------------------------------------------------------------------------------------------------
//
// What both runtimes have.
//

// [coroutine]
c_ares_async::Task<void> print_addresses(c_ares_async::resolver& resolver, std::string host)
{
   const auto [ec, endpoints] = co_await resolver.resolve(host, "443");
   if (ec)
   {
      std::cerr << host << ": " << ec.message() << '\n';
      co_return;
   }

   for (const auto& endpoint : endpoints)
      std::cout << endpoint.address().to_string() << " port " << endpoint.port() << '\n';
}
// [coroutine-end]

// [errors]
c_ares_async::Task<void> say_what_happened(c_ares_async::resolver& resolver, std::string host)
{
   const auto [ec, endpoints] = co_await resolver.resolve(host, "443");

   if (!ec)
      std::cout << host << ": " << endpoints.size() << " endpoint(s)\n";
   else if (ec == c_ares_async::errors::canceled)
      std::cout << host << ": cancelled\n";
   else if (ec == c_ares_async::make_ares_error(ARES_ENOTFOUND))
      std::cout << host << ": no such name\n";
   else if (ec == c_ares_async::make_ares_error(ARES_ETIMEOUT))
      std::cout << host << ": the servers do not answer\n";
   else
      std::cout << host << ": " << ec.message() << '\n';
}
// [errors-end]

// [https-records]
c_ares_async::Task<void> print_https_records(c_ares_async::resolver& resolver, std::string host)
{
   auto [ec, records] = co_await resolver.resolve_https(host);

   //
   // Most names have no HTTPS record, which is no error worth a mention.
   //
   if (ec && ec != c_ares_async::make_ares_error(ARES_ENODATA))
      std::cerr << host << ": " << ec.message() << '\n';

   //
   // They come in no particular order. This one is the order a client goes through them in, with
   // the alias form, whose priority is 0, in front.
   //
   std::ranges::sort(records, {}, &c_ares_async::https_record::priority);

   for (const auto& record : records)
   {
      std::cout << record.priority << ' ' << (record.target.empty() ? "." : record.target);
      for (const auto& param : record.params)
         std::cout << ' ' << param;
      std::cout << '\n';
   }
}
// [https-records-end]

// [set-servers]
void use_these_servers(c_ares_async::resolver& resolver)
{
   //
   // Addresses, each with or without a port. The list is taken as a whole or not at all.
   //
   if (const auto ec = resolver.set_servers("192.0.2.1,[2001:db8::1]:5353"))
      std::cerr << "bad list of servers: " << ec.message() << '\n';
}
// [set-servers-end]

// [dns-over-tls]
c_ares_async::Task<void> resolve_privately(c_ares_async::Executor executor, std::string host)
{
   c_ares_async::tls_options tls;
   tls.enabled = true;
   tls.hostname = "one.one.one.one"; // what the certificate of the server has to say

   c_ares_async::resolver resolver(executor, tls);

   //
   // c-ares knows its servers by address, so the name above does not tell it where to go. No port
   // means 853 here.
   //
   if (const auto ec = resolver.set_servers("1.1.1.1"))
      co_return;

   const auto [ec, endpoints] = co_await resolver.resolve(host, "443");
   std::cout << host << ": " << (ec ? ec.message() : "found") << '\n';
}
// [dns-over-tls-end]

// [logging]
void log_to_stderr(c_ares_async::resolver& resolver)
{
   resolver.set_logger([](c_ares_async::log_level level, std::string_view message) {
      if (level >= c_ares_async::log_level::warning)
         std::cerr << message << '\n';
   });
}
// [logging-end]

// --------------------------------------------------------------------------------------------------
//
// What only Boost.Asio has.
//
#if C_ARES_ASYNC_ASIO

namespace asio = boost::asio;

// [token-exception]
asio::awaitable<void> throws_what_goes_wrong(c_ares_async::resolver& resolver, std::string host)
{
   for (const auto& endpoint : co_await resolver.async_resolve(host, "443", asio::use_awaitable))
      std::cout << endpoint << '\n';
}
// [token-exception-end]

// [token-tuple]
asio::awaitable<void> yields_what_goes_wrong(c_ares_async::resolver& resolver, std::string host)
{
   const auto [ec, endpoints] =
      co_await resolver.async_resolve(host, "443", asio::as_tuple(asio::use_awaitable));

   std::cout << host << ": " << (ec ? ec.message() : "found") << '\n';
}
// [token-tuple-end]

// [token-callback]
void calls_back(c_ares_async::resolver& resolver, const std::string& host)
{
   resolver.async_resolve(host, "443",
                          [](c_ares_async::error_code ec, c_ares_async::resolver::results_type) {
                             std::cout << (ec ? ec.message() : "found") << '\n';
                          });
}
// [token-callback-end]

// [both-at-once]
asio::awaitable<void> addresses_and_record(c_ares_async::resolver& resolver, std::string host)
{
   auto [order, ec, endpoints, https_ec, records] =
      co_await asio::experimental::make_parallel_group(
         resolver.async_resolve(host, "443", asio::deferred),
         resolver.async_resolve_https(host, asio::deferred))
         .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);

   std::cout << host << ": " << endpoints.size() << " endpoint(s), " << records.size()
             << " record(s)\n";
}
// [both-at-once-end]

// [cancel-after]
asio::awaitable<void> gives_up_after_a_while(c_ares_async::resolver& resolver, std::string host)
{
   using namespace std::chrono_literals;

   //
   // With a timer of its own: cancel_after() without one asks the operation for an executor, which
   // those of a resolver do not have.
   //
   asio::steady_timer timer(co_await asio::this_coro::executor);

   const auto [ec, endpoints] = co_await resolver.async_resolve(
      host, "443", asio::cancel_after(timer, 5s, asio::as_tuple(asio::use_awaitable)));

   if (ec == c_ares_async::errors::canceled)
      std::cerr << host << ": took too long\n";
}
// [cancel-after-end]

// [cancel-signal]
void cancels_when_told(c_ares_async::resolver& resolver, asio::cancellation_signal& signal)
{
   resolver.async_resolve("example.com", "443",
                          asio::bind_cancellation_slot(
                             signal.slot(), //
                             [](c_ares_async::error_code ec, c_ares_async::resolver::results_type) {
                                std::cout << ec.message() << '\n';
                             }));

   // and later, from the same thread:
   signal.emit(asio::cancellation_type::terminal);
}
// [cancel-signal-end]

// [strand]
void on_many_threads(asio::io_context& context)
{
   asio::co_spawn(
      asio::make_strand(context),
      []() -> asio::awaitable<void> {
         //
         // The executor of this coroutine is the strand, which makes it that of the resolver as
         // well, and everything in here calls the resolver from inside it.
         //
         c_ares_async::resolver resolver(co_await asio::this_coro::executor);

         co_await print_addresses(resolver, "example.com");
      },
      asio::detached);
}
// [strand-end]

// --------------------------------------------------------------------------------------------------
//
// What only corosio has.
//
#else

// [stop-token]
c_ares_async::Task<void> stops_when_told(c_ares_async::resolver& resolver, std::stop_token token)
{
   const auto [ec, endpoints] =
      co_await boost::capy::run(std::move(token))(resolver.resolve("example.com", "443"));

   if (ec == c_ares_async::errors::canceled)
      std::cerr << "stopped\n";
}
// [stop-token-end]

// [timeout]
c_ares_async::Task<void> times_out(c_ares_async::resolver& resolver)
{
   using namespace std::chrono_literals;

   const auto [ec, endpoints] =
      co_await boost::corosio::timeout(resolver.resolve("example.com", "443"), 5s);

   if (ec == boost::capy::error::timeout)
      std::cerr << "took too long\n";
}
// [timeout-end]

#endif

} // namespace snippets
