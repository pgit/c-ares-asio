// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The operations as corosio asynchronous operations: what a lookup yields, how a stop token
// cancels it, and what becomes of one whose resolver goes away. The counterpart of
// asio/async_test.cpp, which does the same for completion tokens and cancellation slots.
//
#include "../resolver_fixture.hpp"

#include <boost/capy/error.hpp>
#include <boost/capy/ex/run.hpp>
#include <boost/corosio/timeout.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>

using namespace testing;

using cares_asio::error_code;
using cares_asio::make_ares_error;
using cares_asio::Task;
using cares_asio::errors::canceled;

using results_type = cares_asio::resolver::results_type;
using lookup_result = std::tuple<error_code, results_type>;

class Corosio : public resolver_fixture
{
protected:
   void SetUp() override
   {
      resolver_fixture::SetUp();
      server.add_address("www.example.test", "192.0.2.1");
   }

   //
   // Starts a lookup that 'token' cancels, and leaves its outcome in 'result'.
   //
   void start_lookup(std::optional<lookup_result>& result, std::stop_token token,
                     std::string host = "www.example.test")
   {
      start([](cares_asio::resolver& resolver, std::optional<lookup_result>& result,
               std::stop_token token, std::string host) -> Task<void> {
         result = co_await boost::capy::run(std::move(token))(resolver.resolve(host, "443"));
      }(resolver, result, std::move(token), std::move(host)));
   }
};

// --------------------------------------------------------------------------------------------------

TEST_F(Corosio, YieldsTheErrorAndTheResultAsATuple)
{
   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(Corosio, YieldsAnIoResultThatCorosioTakes)
{
   //
   // capy::io_result<T> is std::tuple<std::error_code, T>, so what a lookup yields is what
   // corosio::timeout() and capy::when_all() take -- no adapter in between.
   //
   static_assert(boost::corosio::detail::is_io_result_v<std::tuple<error_code, results_type>>,
                 "resolve() has to yield something corosio's composite operations can be given");
}

// --------------------------------------------------------------------------------------------------

using Stop = Corosio;

TEST_F(Stop, CancelsALookupWithItsStopToken)
{
   server.set_silent(true);

   std::stop_source stop;
   std::optional<lookup_result> result;
   start_lookup(result, stop.get_token());

   ASSERT_TRUE(run_until([&] { return !server.queries().empty(); }));
   stop.request_stop();

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_EQ(std::get<0>(*result), canceled) << std::get<0>(*result).message();
   EXPECT_THAT(std::get<1>(*result), IsEmpty());
}

TEST_F(Stop, TakesEveryOtherLookupOnTheResolverWithIt)
{
   //
   // c-ares has no handle for a single request, so cancelling one cancels the channel -- which
   // the documentation of the resolver says, and this is it.
   //
   server.set_silent(true);
   server.add_address("other.example.test", "192.0.2.2");

   std::stop_source stop;
   std::optional<lookup_result> cancelled;
   std::optional<lookup_result> bystander;
   start_lookup(cancelled, stop.get_token());
   start_lookup(bystander, {}, "other.example.test");

   ASSERT_TRUE(run_until([&] { return server.queries().size() >= 2; }));
   stop.request_stop();

   ASSERT_TRUE(run_until([&] { return cancelled && bystander; }));
   EXPECT_EQ(std::get<0>(*cancelled), canceled);
   EXPECT_EQ(std::get<0>(*bystander), canceled);
}

TEST_F(Stop, NeverAsksForALookupThatWasStoppedBeforeItStarted)
{
   std::stop_source stop;
   stop.request_stop();

   std::optional<lookup_result> result;
   start_lookup(result, stop.get_token());

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_EQ(std::get<0>(*result), canceled);
   EXPECT_THAT(server.queries(), IsEmpty());
}

TEST_F(Stop, GivesUpWhenCorosioTimesTheLookupOut)
{
   server.set_silent(true);

   //
   // What a lookup yields is an io_result, so corosio::timeout() takes it as it is -- this is
   // what cancel_after() is on the other runtime. The code is corosio's own: the deadline
   // cancels the lookup and reports the timeout in its place.
   //
   std::optional<lookup_result> result;
   start([](cares_asio::resolver& resolver,
            std::optional<lookup_result>& result) -> Task<void> { //
      result = co_await boost::corosio::timeout(resolver.resolve("www.example.test", "443"),
                                                std::chrono::milliseconds(300));
   }(resolver, result));

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_EQ(std::get<0>(*result), boost::capy::error::timeout) << std::get<0>(*result).message();
   EXPECT_THAT(std::get<1>(*result), IsEmpty());
}

// --------------------------------------------------------------------------------------------------

using Lifetime = Corosio;

TEST_F(Lifetime, CompletesWhatIsPendingWhenTheResolverGoes)
{
   server.set_silent(true);

   //
   // The resolver lives on the heap for the sake of the address sanitizer, which knows about
   // memory that was given back.
   //
   auto doomed = std::make_unique<cares_asio::resolver>(executor());
   ASSERT_FALSE(doomed->set_servers(server.address()));

   std::optional<lookup_result> result;
   start([](cares_asio::resolver& resolver,
            std::optional<lookup_result>& result) -> Task<void> { //
      result = co_await resolver.resolve("www.example.test", "443");
   }(*doomed, result));

   ASSERT_TRUE(run_until([&] { return !server.queries().empty(); }));
   doomed.reset();

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));
   EXPECT_EQ(std::get<0>(*result), canceled);
}

TEST_F(Lifetime, LeavesNothingBehindInTheContext)
{
   //
   // Once the resolver is gone and its completions have run, there is nothing of it left to keep
   // a context busy: no timer, no socket being waited on. A context with nothing outstanding
   // stops itself, which is how that shows.
   //
   server.set_silent(true);

   std::optional<lookup_result> result;
   {
      auto doomed = std::make_unique<cares_asio::resolver>(executor());
      ASSERT_FALSE(doomed->set_servers(server.address()));

      start([](cares_asio::resolver& resolver,
               std::optional<lookup_result>& result) -> Task<void> { //
         result = co_await resolver.resolve("www.example.test", "443");
      }(*doomed, result));

      ASSERT_TRUE(run_until([&] { return !server.queries().empty(); }));
   }

   ASSERT_TRUE(run_until([&] { return result.has_value(); }));

   context.restart();
   context.run_for(std::chrono::milliseconds(200));
   EXPECT_TRUE(context.stopped()) << "something of the resolver is still waiting";
}
