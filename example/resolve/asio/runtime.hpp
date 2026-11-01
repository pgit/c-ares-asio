// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// What the program needs of Boost.Asio, see ../runtime.hpp.
//
#pragma once

#include "../utils.hpp"

#include <c_ares_async/resolver.hpp>
#include <c_ares_async/runtime.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <spdlog/spdlog.h>

#include <csignal>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace app
{

//
// The event loop. Everything runs on this one, on the one thread that main() gives it.
//
using Context = boost::asio::io_context;

//
// The executor of 'context', as the library takes it.
//
inline c_ares_async::Executor executor_of(Context& context) { return context.get_executor(); }

//
// Runs 'task' on 'context' until it is through, and cancels it on SIGINT or SIGTERM -- rather
// than stopping the context, so that everything gets a chance to unwind: the pending request
// belongs to c-ares, the event loop knows nothing about it.
//
// With 'verbose' the loop is stepped one completion at a time, with a ruler printed per step.
// What the task threw comes back out of here.
//
inline void run_main(Context& context, c_ares_async::Task<void> task, bool verbose)
{
   boost::asio::cancellation_signal cancellation;

   boost::asio::signal_set signals(context, SIGINT, SIGTERM);
   signals.async_wait([&](const c_ares_async::error_code& ec, int signal) {
      if (ec)
         return;

      spdlog::warn("received signal {}, stopping...", signal);
      cancellation.emit(boost::asio::cancellation_type::all);
   });

   boost::asio::co_spawn(
      context, std::move(task),
      boost::asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr ep) {
         signals.cancel();
         if (ep)
            std::rethrow_exception(ep);
      }));

   if (verbose)
      run(context); // one line per completion handler, with timing
   else
      context.run();
}

//
// Runs every one of 'tasks' at once and waits for all of them. Cancelling the wait cancels every
// task in it; what one of them throws is rethrown once they are all done.
//
inline c_ares_async::Task<void> when_all(std::vector<c_ares_async::Task<void>> tasks)
{
   using Operation = decltype(boost::asio::co_spawn(std::declval<c_ares_async::Executor>(),
                                                    std::declval<c_ares_async::Task<void>>(),
                                                    boost::asio::deferred));

   const auto executor = co_await boost::asio::this_coro::executor;

   std::vector<Operation> operations;
   operations.reserve(tasks.size());
   for (auto& task : tasks)
      operations.push_back(boost::asio::co_spawn(executor, std::move(task), boost::asio::deferred));

   auto [order, exceptions] =
      co_await boost::asio::experimental::make_parallel_group(std::move(operations))
         .async_wait(boost::asio::experimental::wait_for_all(), boost::asio::use_awaitable);

   for (const auto& exception : exceptions)
      if (exception)
         std::rethrow_exception(exception);
}

//
// From here on, a cancellation of the calling coroutine is something to look at -- see
// cancelled() -- rather than something that unwinds it at the next co_await.
//
inline c_ares_async::Task<void> observe_cancellation()
{
   co_await boost::asio::this_coro::throw_if_cancelled(false);
}

//
// Whether the calling coroutine has been cancelled. Asking this is not the same as looking at the
// error of a lookup: an answer that was already on its way when the signal came is a good one.
//
inline c_ares_async::Task<bool> cancelled()
{
   const auto state = co_await boost::asio::this_coro::cancellation_state;
   co_return state.cancelled() != boost::asio::cancellation_type::none;
}

//
// The resolver of the runtime itself, which is what --native is for: a getaddrinfo() on a thread
// of its own, addresses and nothing else.
//
class native_resolver
{
public:
   explicit native_resolver(const c_ares_async::Executor& executor) : resolver_(executor) {}

   static constexpr std::string_view name = "the ASIO resolver";

   //
   // However many lookups are handed over at once, they are answered one after the other: ASIO
   // calls getaddrinfo() on a single thread unless its resolver.threads configuration says
   // otherwise.
   //
   static constexpr std::string_view concurrency_note =
      "the ASIO resolver works through its lookups one at a time";

   c_ares_async::Task<std::tuple<c_ares_async::error_code, c_ares_async::resolver::results_type>>
   resolve(const std::string& host, const std::string& service)
   {
      auto [ec, results] = co_await resolver_.async_resolve(host, service, boost::asio::as_tuple);

      c_ares_async::resolver::results_type endpoints;
      for (const auto& result : results)
         endpoints.push_back(result.endpoint());

      co_return std::tuple{ec, std::move(endpoints)};
   }

private:
   boost::asio::ip::tcp::resolver resolver_;
};

//
// 'text' as an address, or nothing where it is a name. A name that is an address already is no
// name to hang a record on, and c-ares answers it without a query.
//
inline std::optional<c_ares_async::IpAddress> make_address(std::string_view text)
{
   c_ares_async::error_code ec;
   const auto address = boost::asio::ip::make_address(text, ec);
   return ec ? std::nullopt : std::optional(address);
}

} // namespace app
