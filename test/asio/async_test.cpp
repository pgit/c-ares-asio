// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The operations as ASIO asynchronous operations: every kind of completion token, the executor a
// handler runs on, cancellation, and what becomes of an operation whose resolver goes away.
//
#include "../resolver_fixture.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>

#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using namespace boost::asio;
using boost::system::error_code;
using c_ares_async::make_ares_error;
using testing::ElementsAre;
using testing::IsEmpty;

using results_type = c_ares_async::resolver::results_type;
using https_results_type = c_ares_async::resolver::https_results_type;

class Async : public resolver_fixture
{
protected:
   void SetUp() override
   {
      resolver_fixture::SetUp();
      server.add_address("www.example.test", "192.0.2.1");
      server.add_https("www.example.test", 1, ".", {{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}}});
   }
};

// --------------------------------------------------------------------------------------------------

TEST_F(Async, CompletesACallback)
{
   std::optional<error_code> result;
   results_type endpoints;

   resolver.async_resolve("www.example.test", "443", [&](error_code ec, results_type found) {
      result = ec;
      endpoints = std::move(found);
   });

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_FALSE(*result) << result->message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(Async, CompletesAFuture)
{
   auto future = resolver.async_resolve("www.example.test", "443", use_future);

   ASSERT_TRUE(run_until(
      [&] { return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }));

   EXPECT_THAT(future.get(), ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(Async, FailsAFutureWithAnException)
{
   auto future = resolver.async_resolve("nowhere.example.test", "443", use_future);

   ASSERT_TRUE(run_until(
      [&] { return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }));

   try
   {
      (void)future.get();
      FAIL() << "a name that does not exist was resolved";
   }
   catch (const boost::system::system_error& error)
   {
      EXPECT_EQ(error.code(), make_ares_error(ARES_ENOTFOUND));
   }
}

TEST_F(Async, CompletesACoroutine)
{
   const auto lookup = [&]() -> awaitable<std::size_t> {
      const auto endpoints =
         co_await resolver.async_resolve("www.example.test", "443", use_awaitable);
      const auto records = co_await resolver.async_resolve_https("www.example.test", use_awaitable);
      co_return endpoints.size() + records.size();
   };

   EXPECT_EQ(run(lookup()), 2u);
}

TEST_F(Async, StartsADeferredOperationOnlyWhenAskedTo)
{
   auto operation = resolver.async_resolve("www.example.test", "443", deferred);

   context.poll();
   EXPECT_THAT(server.queries(), IsEmpty()) << "nothing was asked for yet";

   std::optional<error_code> result;
   std::move(operation)([&](error_code ec, const results_type&) { result = ec; });

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_FALSE(*result) << result->message();
}

TEST_F(Async, KeepsWhatADeferredOperationWasGiven)
{
   //
   // The operation takes views of its arguments, and by the time it starts the strings they
   // were views of are long gone.
   //
   auto operation = [&] {
      const std::string host = "www.example.test";
      const std::string service = "8443";
      return resolver.async_resolve(host, service, deferred);
   }();

   std::optional<error_code> result;
   results_type endpoints;
   std::move(operation)([&](error_code ec, results_type found) {
      result = ec;
      endpoints = std::move(found);
   });

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_FALSE(*result) << result->message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 8443)));
}

// --------------------------------------------------------------------------------------------------

TEST_F(Async, NeverCompletesFromInsideTheInitiatingFunction)
{
   //
   // c-ares answers these two without a query, and with that before ares_getaddrinfo() has even
   // returned. The handler still has to wait for its turn.
   //
   int completed = 0;
   resolver.async_resolve("192.0.2.7", "443",
                          [&](error_code, const results_type&) { ++completed; });
   resolver.async_resolve_https(std::string(64, 'a') + ".example.test",
                                [&](error_code, const https_results_type&) { ++completed; });

   EXPECT_EQ(completed, 0);

   ASSERT_TRUE(run_until([&] { return completed == 2; }));
}

TEST_F(Async, RunsTheHandlerOnItsOwnExecutor)
{
   const auto strand = make_strand(context);

   std::optional<bool> in_strand;
   resolver.async_resolve("www.example.test", "443",
                          bind_executor(strand, [&](error_code, const results_type&) {
                             in_strand = strand.running_in_this_thread();
                          }));

   ASSERT_TRUE(run_until([&] { return in_strand.has_value(); }));
   EXPECT_TRUE(*in_strand);
}

TEST_F(Async, KeepsTheContextRunningUntilItIsDone)
{
   //
   // A context of its own with nothing in it but the lookup: run() must not return before the
   // answer is in, and must return once it is.
   //
   io_context other;
   c_ares_async::resolver lone(other.get_executor());

   std::optional<error_code> result;
   lone.async_resolve("192.0.2.7", "443", [&](error_code ec, const results_type&) { result = ec; });

   other.run();
   ASSERT_TRUE(result.has_value());
   EXPECT_FALSE(*result) << result->message();
}

TEST(Strand, ServesAContextThatRunsOnSeveralThreads)
{
   //
   // A resolver is not for several threads at once, and a strand is how ASIO keeps them off one
   // another: as the resolver's executor it serializes everything the resolver does by itself,
   // and the lookups are started from inside it. The server gets a strand of its own.
   //
   io_context context;
   fake_dns_server server(make_strand(context));

   constexpr int count = 200;
   for (int i = 0; i < count; ++i)
      server.add_address(std::format("host{}.example.test", i), std::format("192.0.2.{}", i));

   const auto strand = make_strand(context);
   std::optional<c_ares_async::resolver> resolver(std::in_place, strand);
   ASSERT_FALSE(resolver->set_servers(server.address()));

   const auto lookups = [&]() -> awaitable<int> {
      int resolved = 0;
      for (int i = 0; i < count; ++i)
      {
         const auto [ec, endpoints] = co_await resolver->async_resolve(
            std::format("host{}.example.test", i), "443", as_tuple(use_awaitable));

         if (!ec && endpoints.size() == 1 &&
             endpoints.front().address() == ip::make_address(std::format("192.0.2.{}", i)))
            ++resolved;
      }

      resolver.reset(); // in the strand as well
      co_return resolved;
   };

   auto resolved = co_spawn(strand, lookups(), use_future);

   std::vector<std::thread> threads;
   for (int i = 0; i < 4; ++i)
      threads.emplace_back([&] { context.run_for(std::chrono::seconds(20)); });

   const bool done = resolved.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
   context.stop();
   for (auto& thread : threads)
      thread.join();

   ASSERT_TRUE(done);
   EXPECT_EQ(resolved.get(), count);
}

// --------------------------------------------------------------------------------------------------

using Cancellation = Async;

TEST_F(Cancellation, CompletesWithOperationAborted)
{
   server.set_silent(true);

   cancellation_signal signal;
   std::optional<error_code> result;
   resolver.async_resolve(
      "www.example.test", "443",
      bind_cancellation_slot(signal.slot(),
                             [&](error_code ec, const results_type&) { result = ec; }));

   ASSERT_TRUE(run_until([&] { return !server.queries().empty(); }));
   EXPECT_FALSE(result.has_value());

   signal.emit(cancellation_type::terminal);
   EXPECT_FALSE(result.has_value()) << "the handler has to wait for its turn here as well";

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_EQ(*result, error::operation_aborted);
}

TEST_F(Cancellation, CancelsAnHttpsLookup)
{
   server.set_silent(true);

   cancellation_signal signal;
   std::optional<error_code> result;
   resolver.async_resolve_https(
      "www.example.test",
      bind_cancellation_slot(signal.slot(),
                             [&](error_code ec, const https_results_type&) { result = ec; }));

   ASSERT_TRUE(run_until([&] { return !server.queries().empty(); }));
   signal.emit(cancellation_type::terminal);

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_EQ(*result, error::operation_aborted);
}

TEST_F(Cancellation, TakesEveryPendingOperationWithIt)
{
   //
   // Not what one would ask for, but what there is: c-ares has no handle for a single request,
   // so cancelling one of them is ares_cancel() for the channel.
   //
   server.set_silent(true);

   cancellation_signal signal;
   std::optional<error_code> cancelled, bystander;
   resolver.async_resolve(
      "www.example.test", "443",
      bind_cancellation_slot(signal.slot(),
                             [&](error_code ec, const results_type&) { cancelled = ec; }));
   resolver.async_resolve("other.example.test", "443",
                          [&](error_code ec, const results_type&) { bystander = ec; });

   ASSERT_TRUE(run_until([&] { return server.queries().size() >= 2; }));
   signal.emit(cancellation_type::terminal);

   ASSERT_TRUE(run_until([&] { return cancelled && bystander; }));
   EXPECT_EQ(*cancelled, error::operation_aborted);
   EXPECT_EQ(*bystander, error::operation_aborted);
}

TEST_F(Cancellation, LeavesTheResolverInWorkingOrder)
{
   server.set_silent(true);

   cancellation_signal signal;
   std::optional<error_code> result;
   resolver.async_resolve(
      "www.example.test", "443",
      bind_cancellation_slot(signal.slot(),
                             [&](error_code ec, const results_type&) { result = ec; }));

   ASSERT_TRUE(run_until([&] { return !server.queries().empty(); }));
   signal.emit(cancellation_type::terminal);
   ASSERT_TRUE(run_until([&] { return result.has_value(); }));

   server.set_silent(false);
   const auto [ec, endpoints] = resolve("www.example.test");
   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(Cancellation, IsNothingToAnOperationThatIsDone)
{
   cancellation_signal signal;
   std::optional<error_code> result;
   resolver.async_resolve(
      "www.example.test", "443",
      bind_cancellation_slot(signal.slot(),
                             [&](error_code ec, const results_type&) { result = ec; }));

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_FALSE(*result) << result->message();

   //
   // Too late for that one, and none of the business of the next.
   //
   signal.emit(cancellation_type::terminal);

   const auto [ec, endpoints] = resolve("www.example.test");
   EXPECT_FALSE(ec) << ec.message();
}

TEST_F(Cancellation, IsNothingOnceTheResolverIsGone)
{
   //
   // A signal outlives the operations it was bound to, and may be raised long after: when the
   // one that was answered, the one that failed without a query and the one that was still
   // pending have all completed, and the resolver they would cancel is no more.
   //
   server.add_address("slow.example.test", "192.0.2.2");

   cancellation_signal answered, failed, pending;
   int completed = 0;
   {
      auto doomed = std::make_unique<c_ares_async::resolver>(context.get_executor());
      ASSERT_FALSE(doomed->set_servers(server.address()));

      doomed->async_resolve(
         "www.example.test", "443",
         bind_cancellation_slot(answered.slot(),
                                [&](error_code, const results_type&) { ++completed; }));
      doomed->async_resolve_https(
         std::string(64, 'a') + ".example.test",
         bind_cancellation_slot(failed.slot(),
                                [&](error_code, const https_results_type&) { ++completed; }));

      ASSERT_TRUE(run_until([&] { return completed == 2; }));

      server.set_silent(true);
      doomed->async_resolve(
         "slow.example.test", "443",
         bind_cancellation_slot(pending.slot(),
                                [&](error_code, const results_type&) { ++completed; }));
   }

   ASSERT_TRUE(run_until([&] { return completed == 3; }));

   answered.emit(cancellation_type::terminal);
   failed.emit(cancellation_type::terminal);
   pending.emit(cancellation_type::terminal);
   context.poll();
}

// --------------------------------------------------------------------------------------------------

using Lifetime = Async;

TEST_F(Lifetime, CompletesWhatIsPendingWhenTheResolverGoes)
{
   server.set_silent(true);

   std::optional<error_code> result, https_result;
   {
      c_ares_async::resolver doomed(context.get_executor());
      ASSERT_FALSE(doomed.set_servers(server.address()));

      doomed.async_resolve("www.example.test", "443",
                           [&](error_code ec, const results_type&) { result = ec; });
      doomed.async_resolve_https(
         "www.example.test", [&](error_code ec, const https_results_type&) { https_result = ec; });

      ASSERT_TRUE(run_until([&] { return server.queries().size() >= 2; }));
   }

   EXPECT_FALSE(result.has_value()) << "not from inside the destructor";

   ASSERT_TRUE(run_until([&] { return result && https_result; }));
   EXPECT_EQ(*result, error::operation_aborted);
   EXPECT_EQ(*https_result, error::operation_aborted);
}

TEST_F(Lifetime, LeavesNothingBehindInTheContext)
{
   //
   // Once the resolver is gone and the handlers have run, there is nothing of it left to keep a
   // context busy: no timer, no socket being waited on.
   //
   io_context other;
   fake_dns_server silent(other.get_executor());
   silent.set_silent(true);

   std::optional<error_code> result;
   {
      c_ares_async::resolver doomed(other.get_executor());
      ASSERT_FALSE(doomed.set_servers(silent.address()));
      doomed.async_resolve("www.example.test", "443",
                           [&](error_code ec, const results_type&) { result = ec; });

      ASSERT_TRUE(::run_until(other, [&] { return !silent.queries().empty(); }));
   }

   ASSERT_TRUE(::run_until(other, [&] { return result.has_value(); }));
   EXPECT_EQ(*result, error::operation_aborted);
}

TEST_F(Lifetime, IgnoresATimeoutThatWasAlreadyDueWhenTheResolverWent)
{
   //
   // Two timers that are both due by the time the context gets to run again: one of the test's
   // and, a little after it, the timeout c-ares asked the resolver for. The context takes them
   // off its queue together and runs them in that order -- and the first one does away with the
   // resolver. Cancelling a timer does nothing for a handler that is on its way already, so the
   // second one still runs, and it had better not look for the resolver it belonged to.
   //
   // The resolver lives on the heap for the sake of the address sanitizer, which knows about
   // memory that was given back.
   //
   ASSERT_EQ(::setenv("RES_OPTIONS", "timeout:1 attempts:1", 1), 0);
   auto doomed = std::make_unique<c_ares_async::resolver>(context.get_executor());
   ASSERT_EQ(::unsetenv("RES_OPTIONS"), 0);

   ASSERT_FALSE(doomed->set_servers(server.address()));
   server.set_silent(true);

   std::optional<error_code> result;
   doomed->async_resolve("www.example.test", "443",
                         [&](error_code ec, const results_type&) { result = ec; });

   steady_timer earlier(context, std::chrono::milliseconds(500));
   earlier.async_wait([&](error_code) { doomed.reset(); });

   std::this_thread::sleep_for(std::chrono::milliseconds(1300));

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_EQ(*result, error::operation_aborted);
   EXPECT_EQ(doomed, nullptr);

   context.poll(); // whatever else was on its way
}

TEST_F(Lifetime, CanGoWithNothingEverAsked)
{
   c_ares_async::resolver unused(context.get_executor());
   context.poll();
}
