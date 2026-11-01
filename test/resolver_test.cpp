// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// Address lookups, start to finish: the resolver asks the fake server, over real sockets on the
// loopback interface, and nothing leaves the machine.
//
#include "resolver_fixture.hpp"

#include <cstdlib>
#include <format>
#include <map>
#include <tuple>
#include <utility>

using c_ares_async::error_code;
using c_ares_async::make_ares_error;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::UnorderedElementsAre;

using Resolver = resolver_fixture;

TEST_F(Resolver, HasTheExecutorItWasGiven) { EXPECT_EQ(resolver.get_executor(), executor()); }

TEST_F(Resolver, ResolvesBothAddressFamilies)
{
   server.add_address("www.example.test", "192.0.2.1");
   server.add_address("www.example.test", "2001:db8::1");

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints,
               UnorderedElementsAre(endpoint("192.0.2.1", 443), endpoint("2001:db8::1", 443)));

   EXPECT_GE(server.count("www.example.test", ARES_REC_TYPE_A), 1u);
   EXPECT_GE(server.count("www.example.test", ARES_REC_TYPE_AAAA), 1u);
}

TEST_F(Resolver, ResolvesEveryAddressOfAName)
{
   server.add_address("www.example.test", "192.0.2.1");
   server.add_address("www.example.test", "192.0.2.2");
   server.add_address("www.example.test", "192.0.2.3");

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints,
               UnorderedElementsAre(endpoint("192.0.2.1", 443), endpoint("192.0.2.2", 443),
                                    endpoint("192.0.2.3", 443)));
}

TEST_F(Resolver, MakesDoWithOneAddressFamily)
{
   server.add_address("v4.example.test", "192.0.2.1");
   server.add_address("v6.example.test", "2001:db8::1");

   {
      const auto [ec, endpoints] = resolve("v4.example.test");
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   }
   {
      const auto [ec, endpoints] = resolve("v6.example.test");
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_THAT(endpoints, ElementsAre(endpoint("2001:db8::1", 443)));
   }
}

TEST_F(Resolver, PutsThePortOfTheServiceOnTheEndpoints)
{
   server.add_address("www.example.test", "192.0.2.1");

   EXPECT_THAT(std::get<1>(resolve("www.example.test", "8443")),
               ElementsAre(endpoint("192.0.2.1", 8443)));

   // no service is no port
   EXPECT_THAT(std::get<1>(resolve("www.example.test", "")), ElementsAre(endpoint("192.0.2.1", 0)));
}

TEST_F(Resolver, TakesAnAddressForAName)
{
   {
      const auto [ec, endpoints] = resolve("192.0.2.7");
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.7", 443)));
   }
   {
      const auto [ec, endpoints] = resolve("2001:db8::7");
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_THAT(endpoints, ElementsAre(endpoint("2001:db8::7", 443)));
   }

   EXPECT_THAT(server.queries(), IsEmpty()) << "an address is nothing to ask a server about";
}

TEST_F(Resolver, FollowsAnAlias)
{
   server.add_cname("www.example.test", "host.example.test");
   server.add_address("host.example.test", "192.0.2.1");

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(Resolver, ComparesNamesWhateverTheirCase)
{
   server.add_address("www.example.test", "192.0.2.1");

   const auto [ec, endpoints] = resolve("WWW.Example.TEST");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(Resolver, ReportsANameThatDoesNotExist)
{
   const auto [ec, endpoints] = resolve("nowhere.example.test");

   EXPECT_EQ(ec, make_ares_error(ARES_ENOTFOUND)) << ec.message();
   EXPECT_THAT(endpoints, IsEmpty());
}

TEST_F(Resolver, ReportsANameThatIsNone)
{
   //
   // A label is 63 octets at most, so this one cannot be put into a query at all.
   //
   const auto [ec, endpoints] = resolve(std::string(64, 'a') + ".example.test");

   EXPECT_EQ(ec, make_ares_error(ARES_EBADNAME)) << ec.message();
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_THAT(server.queries(), IsEmpty());
}

TEST_F(Resolver, ReportsAServerThatFails)
{
   server.set_rcode(ARES_RCODE_SERVFAIL);

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_EQ(ec, make_ares_error(ARES_ESERVFAIL)) << ec.message();
   EXPECT_THAT(endpoints, IsEmpty());
}

TEST_F(Resolver, GivesUpOnAServerThatDoesNotAnswer)
{
   //
   // Left to itself c-ares waits two seconds, and then tries again. RES_OPTIONS is how the
   // options of resolv.conf are overridden for a process, and it is read when the channel is
   // set up -- so this takes a resolver of its own.
   //
   ASSERT_EQ(::setenv("RES_OPTIONS", "timeout:1 attempts:1", 1), 0);
   c_ares_async::resolver impatient(executor());
   ASSERT_EQ(::unsetenv("RES_OPTIONS"), 0);

   ASSERT_FALSE(impatient.set_servers(server.address()));
   server.set_silent(true);

   const auto started = std::chrono::steady_clock::now();
   const auto [ec, endpoints] = run(impatient.resolve("www.example.test", "443"));
   const auto elapsed = std::chrono::steady_clock::now() - started;

   EXPECT_EQ(ec, make_ares_error(ARES_ETIMEOUT)) << ec.message();
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_GE(elapsed, std::chrono::milliseconds(900));
   EXPECT_FALSE(server.queries().empty());
}

TEST_F(Resolver, AsksAgainOverTcpWhenTheAnswerDidNotFit)
{
   server.add_address("www.example.test", "192.0.2.1");
   server.set_truncate_udp(true);

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_GE(server.count(fake_dns_server::transport::udp), 1u);
   EXPECT_GE(server.count(fake_dns_server::transport::tcp), 1u);
}

TEST_F(Resolver, KeepsManyLookupsApart)
{
   //
   // All of them in flight at once, on the one channel and its one socket. Every name has an
   // address of its own, so an answer that went to the wrong lookup would show.
   //
   constexpr int count = 100;
   for (int i = 0; i < count; ++i)
      server.add_address(std::format("host{}.example.test", i), std::format("192.0.2.{}", i));

   std::map<int, std::tuple<error_code, c_ares_async::resolver::results_type>> results;
   for (int i = 0; i < count; ++i)
      start([](c_ares_async::resolver& resolver, auto& results, int i) -> c_ares_async::Task<void> {
         results[i] = co_await resolver.resolve(std::format("host{}.example.test", i), "443");
      }(resolver, results, i));

   ASSERT_TRUE(run_until([&] { return results.size() == count; }));

   for (int i = 0; i < count; ++i)
   {
      const auto& [ec, endpoints] = results[i];
      EXPECT_FALSE(ec) << i << ": " << ec.message();
      EXPECT_THAT(endpoints, ElementsAre(endpoint(std::format("192.0.2.{}", i), 443))) << i;
   }
}

TEST_F(Resolver, CanBeUsedAgainAndAgain)
{
   server.add_address("www.example.test", "192.0.2.1");

   for (int i = 0; i < 3; ++i)
   {
      const auto [ec, endpoints] = resolve("www.example.test");
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   }
}

// --------------------------------------------------------------------------------------------------

TEST_F(Resolver, RejectsAServerListItCannotMakeSenseOf)
{
   server.add_address("www.example.test", "192.0.2.1");

   EXPECT_TRUE(resolver.set_servers("not an address"));
   EXPECT_TRUE(resolver.set_servers("192.0.2.1:port"));

   // and goes on asking the one it had
   const auto [ec, endpoints] = resolve("www.example.test");
   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(Resolver, KeepsItsServersWhenGivenNone)
{
   server.add_address("www.example.test", "192.0.2.1");

   EXPECT_FALSE(resolver.set_servers(""));

   const auto [ec, endpoints] = resolve("www.example.test");
   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(Resolver, AsksTheServerItWasToldLast)
{
   fake_dns_server other(server_executor());
   other.add_address("www.example.test", "192.0.2.2");
   server.add_address("www.example.test", "192.0.2.1");

   ASSERT_FALSE(resolver.set_servers(other.address()));

   const auto [ec, endpoints] = resolve("www.example.test");
   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.2", 443)));
   EXPECT_THAT(server.queries(), IsEmpty());
}

// --------------------------------------------------------------------------------------------------

using c_ares_async::log_level;
using testing::Contains;
using testing::HasSubstr;
using testing::Pair;

TEST_F(Resolver, TellsItsLoggerWhatItDoes)
{
   server.add_address("www.example.test", "192.0.2.1");

   std::vector<std::pair<log_level, std::string>> messages;
   resolver.set_logger(
      [&](log_level level, std::string_view message) { messages.emplace_back(level, message); });

   resolve("www.example.test");

   EXPECT_THAT(messages, Contains(Pair(log_level::debug,
                                       HasSubstr("ares_getaddrinfo(www.example.test, 443)"))));
   EXPECT_THAT(messages, Contains(Pair(log_level::debug, HasSubstr("Successful completion"))));
}

TEST_F(Resolver, SaysNothingMoreOnceTheLoggerIsGone)
{
   server.add_address("www.example.test", "192.0.2.1");

   int messages = 0;
   resolver.set_logger([&](log_level, std::string_view) { ++messages; });
   resolve("www.example.test");
   EXPECT_GT(messages, 0);

   resolver.set_logger({});
   messages = 0;
   resolve("www.example.test");
   EXPECT_EQ(messages, 0);
}
