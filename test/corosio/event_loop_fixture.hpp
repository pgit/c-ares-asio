// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The event loop of corosio, for a test to drive. asio/event_loop_fixture.hpp has the
// documentation of what a fixture offers, and what is said here is what differs.
//
// There are two loops: the resolver's, which is corosio's, and the fake DNS server's, which is
// Boost.Asio's either way (it serves UDP, TCP and TLS, and porting a test server to a second
// runtime would only put a second thing to keep right next to the first). Both are stepped from
// the one thread the test runs on, so the tests stay as deterministic as they are on ASIO -- a
// server on a thread of its own would race whoever looks at what it was asked.
//
#pragma once

#include <cares_async/runtime.hpp>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ip_address.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

//
// What a failed expectation shows of an endpoint. corosio has no output operator for one, and
// without this GoogleTest prints the bytes it is made of.
//
namespace boost::corosio
{

inline void PrintTo(const endpoint& ep, std::ostream* os)
{
   *os << ep.address() << ':' << ep.port();
}

} // namespace boost::corosio

class event_loop_fixture : public testing::Test
{
protected:
   //
   // A capy context has to run dry: destroying it with a coroutine still suspended on it leaks
   // the coroutine's frame, which LeakSanitizer finds. By the time this runs, the resolver of the
   // test is gone (it is a member of the fixture that derives from this one), so what is left is
   // the waits and timers it cancelled on its way out.
   //
   ~event_loop_fixture() override
   {
      context.restart();
      while (context.run_one_for(std::chrono::milliseconds(1)) > 0)
         ;
   }

   cares_async::Executor executor() { return cares_async::Executor(context.get_executor()); }

   boost::asio::any_io_executor server_executor() { return server_context.get_executor(); }

   template <typename T>
   T run(cares_async::Task<T> task)
   {
      std::optional<T> result;
      std::exception_ptr failed;

      boost::capy::run_async(
         executor(), [&](T value) noexcept { result.emplace(std::move(value)); },
         [&](std::exception_ptr error) noexcept { failed = error; })(std::move(task));

      if (!run_until([&] { return result.has_value() || failed != nullptr; }))
         throw std::runtime_error("the coroutine did not complete in time");

      if (failed)
         std::rethrow_exception(failed);

      return std::move(*result);
   }

   void start(cares_async::Task<void> task)
   {
      boost::capy::run_async(
         executor(), []() noexcept {}, [](std::exception_ptr) noexcept {})(std::move(task));
   }

   //
   // Neither loop may be waited on for long in here: whichever one has nothing to do may be the
   // one waiting for the other to answer it. So the server's is polled, and the resolver's is
   // stepped with a millisecond to wait at most -- corosio's poll() drains what is ready
   // already but does not ask the reactor, so it would never see a socket become readable.
   //
   // A context with nothing outstanding stops itself, and has to be restarted before it runs
   // again. Where neither of them had anything to do, this gives the processor back for a
   // moment rather than spinning on it.
   //
   template <typename Predicate>
   [[nodiscard]] bool
   run_until(Predicate done, std::chrono::steady_clock::duration limit = std::chrono::seconds(20))
   {
      const auto deadline = std::chrono::steady_clock::now() + limit;

      while (!done())
      {
         if (std::chrono::steady_clock::now() >= deadline)
            return false;

         if (server_context.stopped())
            server_context.restart();

         if (context.stopped())
            context.restart();

         const auto served = server_context.poll();
         const auto stepped = context.run_one_for(std::chrono::milliseconds(1));

         if (served == 0 && stepped == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
      }

      return true;
   }

   static cares_async::Endpoint endpoint(std::string_view address, unsigned short port)
   {
      return {boost::corosio::ip_address(address), port};
   }

   //
   // In this order: the server's loop outlives the server, and the resolver's the resolver.
   //
   boost::asio::io_context server_context;
   boost::corosio::io_context context;
};
