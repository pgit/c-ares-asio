// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The tests run their io_context themselves, a handler at a time, so that they can look at what
// happened in between -- and so that a test that waits for something that never comes fails
// instead of hanging.
//
#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <future>
#include <stdexcept>
#include <utility>

//
// Runs 'context' until 'done' says so. Returns false if that takes longer than 'limit', which is
// a lot more than anything in here has a reason to take.
//
template <typename Predicate>
[[nodiscard]] bool run_until(boost::asio::io_context& context, Predicate done,
                             std::chrono::steady_clock::duration limit = std::chrono::seconds(20))
{
   const auto deadline = std::chrono::steady_clock::now() + limit;

   context.restart();
   while (!done())
   {
      if (std::chrono::steady_clock::now() >= deadline)
         return false;

      context.run_one_until(deadline);
   }

   return true;
}

//
// Runs 'context' until the coroutine 'task' is through, and returns what it returned.
//
template <typename T>
T run(boost::asio::io_context& context, boost::asio::awaitable<T> task)
{
   auto future = boost::asio::co_spawn(context, std::move(task), boost::asio::use_future);

   const auto ready = [&] {
      return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
   };

   if (!run_until(context, ready))
      throw std::runtime_error("the coroutine did not complete in time");

   return future.get();
}
