// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The event loop of Boost.Asio, for a test to drive: one io_context, which the resolver and the
// fake DNS server share, stepped by the test itself -- see run.hpp.
//
#pragma once

#include "run.hpp"

#include <c_ares_async/runtime.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string_view>
#include <utility>

class event_loop_fixture : public testing::Test
{
protected:
   //
   // What the resolver under test runs on.
   //
   c_ares_async::Executor executor() { return context.get_executor(); }

   //
   // What the fake DNS server runs on, which is Boost.Asio on either runtime. Here it is the one
   // context there is.
   //
   boost::asio::any_io_executor server_executor() { return context.get_executor(); }

   //
   // Runs the loop until 'task' is through, and returns what it returned.
   //
   template <typename T>
   T run(c_ares_async::Task<T> task)
   {
      return ::run(context, std::move(task));
   }

   //
   // Starts 'task' and comes back: for the tests that want more than one thing in flight.
   //
   void start(c_ares_async::Task<void> task)
   {
      boost::asio::co_spawn(context, std::move(task), boost::asio::detached);
   }

   //
   // Runs the loop until 'done' says so. False if that takes longer than 'limit'.
   //
   template <typename Predicate>
   [[nodiscard]] bool
   run_until(Predicate done, std::chrono::steady_clock::duration limit = std::chrono::seconds(20))
   {
      return ::run_until(context, std::move(done), limit);
   }

   static c_ares_async::Endpoint endpoint(std::string_view address, unsigned short port)
   {
      return {boost::asio::ip::make_address(address), port};
   }

   boost::asio::io_context context;
};
