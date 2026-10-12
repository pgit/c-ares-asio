//
// The operations as ASIO asynchronous operations: every kind of completion token, the executor a
// handler runs on, cancellation, and what becomes of an operation whose resolver goes away.
//
#include "resolver_fixture.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>

#include <memory>
#include <optional>

using namespace boost::asio;
using boost::system::error_code;
using cares_asio::make_ares_error;
using testing::ElementsAre;
using testing::IsEmpty;

using results_type = cares_asio::resolver::results_type;
using https_results_type = cares_asio::resolver::https_results_type;

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

   ASSERT_TRUE(run_until(context, [&] { return result.has_value(); }));
   EXPECT_FALSE(*result) << result->message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(Async, CompletesAFuture)
{
   auto future = resolver.async_resolve("www.example.test", "443", use_future);

   ASSERT_TRUE(run_until(context, [&] {
      return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
   }));

   EXPECT_THAT(future.get(), ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(Async, FailsAFutureWithAnException)
{
   auto future = resolver.async_resolve("nowhere.example.test", "443", use_future);

   ASSERT_TRUE(run_until(context, [&] {
      return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
   }));

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

   EXPECT_EQ(run(context, lookup()), 2u);
}

TEST_F(Async, StartsADeferredOperationOnlyWhenAskedTo)
{
   auto operation = resolver.async_resolve("www.example.test", "443", deferred);

   context.poll();
   EXPECT_THAT(server.queries(), IsEmpty()) << "nothing was asked for yet";

   std::optional<error_code> result;
   std::move(operation)([&](error_code ec, const results_type&) { result = ec; });

   ASSERT_TRUE(run_until(context, [&] { return result.has_value(); }));
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

   ASSERT_TRUE(run_until(context, [&] { return result.has_value(); }));
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

   ASSERT_TRUE(run_until(context, [&] { return completed == 2; }));
}

TEST_F(Async, RunsTheHandlerOnItsOwnExecutor)
{
   const auto strand = make_strand(context);

   std::optional<bool> in_strand;
   resolver.async_resolve("www.example.test", "443",
                          bind_executor(strand, [&](error_code, const results_type&) {
                             in_strand = strand.running_in_this_thread();
                          }));

   ASSERT_TRUE(run_until(context, [&] { return in_strand.has_value(); }));
   EXPECT_TRUE(*in_strand);
}

TEST_F(Async, KeepsTheContextRunningUntilItIsDone)
{
   //
   // A context of its own with nothing in it but the lookup: run() must not return before the
   // answer is in, and must return once it is.
   //
   io_context other;
   cares_asio::resolver lone(other.get_executor());

   std::optional<error_code> result;
   lone.async_resolve("192.0.2.7", "443", [&](error_code ec, const results_type&) { result = ec; });

   other.run();
   ASSERT_TRUE(result.has_value());
   EXPECT_FALSE(*result) << result->message();
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

   ASSERT_TRUE(run_until(context, [&] { return !server.queries().empty(); }));
   EXPECT_FALSE(result.has_value());

   signal.emit(cancellation_type::terminal);
   EXPECT_FALSE(result.has_value()) << "the handler has to wait for its turn here as well";

   ASSERT_TRUE(run_until(context, [&] { return result.has_value(); }));
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

   ASSERT_TRUE(run_until(context, [&] { return !server.queries().empty(); }));
   signal.emit(cancellation_type::terminal);

   ASSERT_TRUE(run_until(context, [&] { return result.has_value(); }));
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

   ASSERT_TRUE(run_until(context, [&] { return server.queries().size() >= 2; }));
   signal.emit(cancellation_type::terminal);

   ASSERT_TRUE(run_until(context, [&] { return cancelled && bystander; }));
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

   ASSERT_TRUE(run_until(context, [&] { return !server.queries().empty(); }));
   signal.emit(cancellation_type::terminal);
   ASSERT_TRUE(run_until(context, [&] { return result.has_value(); }));

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

   ASSERT_TRUE(run_until(context, [&] { return result.has_value(); }));
   EXPECT_FALSE(*result) << result->message();

   //
   // Too late for that one, and none of the business of the next.
   //
   signal.emit(cancellation_type::terminal);

   const auto [ec, endpoints] = resolve("www.example.test");
   EXPECT_FALSE(ec) << ec.message();
}

// --------------------------------------------------------------------------------------------------

using Lifetime = Async;

TEST_F(Lifetime, CompletesWhatIsPendingWhenTheResolverGoes)
{
   server.set_silent(true);

   std::optional<error_code> result, https_result;
   {
      cares_asio::resolver doomed(context.get_executor());
      ASSERT_FALSE(doomed.set_servers(server.address()));

      doomed.async_resolve("www.example.test", "443",
                           [&](error_code ec, const results_type&) { result = ec; });
      doomed.async_resolve_https(
         "www.example.test", [&](error_code ec, const https_results_type&) { https_result = ec; });

      ASSERT_TRUE(run_until(context, [&] { return server.queries().size() >= 2; }));
   }

   EXPECT_FALSE(result.has_value()) << "not from inside the destructor";

   ASSERT_TRUE(run_until(context, [&] { return result && https_result; }));
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
      cares_asio::resolver doomed(other.get_executor());
      ASSERT_FALSE(doomed.set_servers(silent.address()));
      doomed.async_resolve("www.example.test", "443",
                           [&](error_code ec, const results_type&) { result = ec; });

      ASSERT_TRUE(run_until(other, [&] { return !silent.queries().empty(); }));
   }

   ASSERT_TRUE(run_until(other, [&] { return result.has_value(); }));
   EXPECT_EQ(*result, error::operation_aborted);
}

TEST_F(Lifetime, CanGoWithNothingEverAsked)
{
   cares_asio::resolver unused(context.get_executor());
   context.poll();
}
