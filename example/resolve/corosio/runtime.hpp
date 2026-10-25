// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// What the program needs of capy/corosio, see ../runtime.hpp. asio/runtime.hpp has the
// documentation of each of these, and what is said here is what differs.
//
#pragma once

#include "../utils.hpp"

#include <cares_asio/resolver.hpp>
#include <cares_asio/runtime.hpp>

#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ip_address.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/signal_set.hpp>

#include <spdlog/spdlog.h>

#include <csignal>
#include <exception>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace app
{

using Context = boost::corosio::io_context;

inline cares_asio::Executor executor_of(Context& context)
{
   return cares_asio::Executor(context.get_executor());
}

//
// The signal handling is a task of its own here, since what corosio offers is a signal to wait
// for rather than a callback. Stopping the lookups is a stop request on the task that runs them.
//
inline void run_main(Context& context, cares_asio::Task<void> task, bool verbose)
{
   const auto executor = executor_of(context);

   std::stop_source stop;
   std::exception_ptr failed;

   boost::corosio::signal_set signals(context, SIGINT, SIGTERM);

   boost::capy::run_async(
      executor, []() noexcept {}, [](std::exception_ptr) noexcept {})(
      [](boost::corosio::signal_set& signals, std::stop_source& stop) -> cares_asio::Task<void> {
         auto [ec, signal] = co_await signals.wait();
         if (ec)
            co_return; // cancelled, which is what the lookups being through does

         spdlog::warn("received signal {}, stopping...", signal);
         stop.request_stop();
      }(signals, stop));

   boost::capy::run_async(
      executor, stop.get_token(), [&]() noexcept { signals.cancel(); },
      [&](std::exception_ptr ep) noexcept {
         signals.cancel();
         failed = ep;
      })(std::move(task));

   if (verbose)
      run(context); // one line per completion, with timing
   else
      context.run();

   if (failed)
      std::rethrow_exception(failed);
}

//
// capy::when_all() takes a range of operations that yield an io_result, so every task is wrapped
// in one that yields an empty one. A stop request reaches all of them, as it is the token of the
// coroutine awaiting here that they run with.
//
inline cares_asio::Task<void> when_all(std::vector<cares_asio::Task<void>> tasks)
{
   const auto wrap = [](cares_asio::Task<void> task) -> boost::capy::io_task<> {
      co_await std::move(task);
      co_return boost::capy::io_result<>{};
   };

   std::vector<boost::capy::io_task<>> operations;
   operations.reserve(tasks.size());
   for (auto& task : tasks)
      operations.push_back(wrap(std::move(task)));

   std::ignore = co_await boost::capy::when_all(std::move(operations));
}

//
// A stop request is nothing a coroutine is unwound for: it stops what is being awaited and the
// coroutine goes on. There is nothing to turn on for that.
//
inline cares_asio::Task<void> observe_cancellation() { co_return; }

inline cares_asio::Task<bool> cancelled()
{
   co_return (co_await boost::capy::this_coro::stop_token).stop_requested();
}

class native_resolver
{
public:
   explicit native_resolver(const cares_asio::Executor& executor) : resolver_(executor.context()) {}

   static constexpr std::string_view name = "the corosio resolver";

   //
   // corosio has a pool of threads for the blocking calls (io_context_options::threads), so a
   // handful of lookups do go out at the same time -- just not as many as --concurrency asks for.
   //
   static constexpr std::string_view concurrency_note =
      "the corosio resolver has as many lookups in flight as its thread pool has threads";

   cares_asio::Task<std::tuple<cares_asio::error_code, cares_asio::resolver::results_type>>
   resolve(const std::string& host, const std::string& service)
   {
      auto [ec, endpoints] = co_await resolver_.resolve(host, service);
      co_return std::tuple{ec, std::move(endpoints)};
   }

private:
   boost::corosio::resolver resolver_;
};

inline std::optional<cares_asio::IpAddress> make_address(std::string_view text)
{
   auto [ec, address] = boost::corosio::make_ip_address(text);
   return ec ? std::nullopt : std::optional(address);
}

} // namespace app
