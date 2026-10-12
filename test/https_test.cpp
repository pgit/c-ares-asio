//
// HTTPS records (RFC 9460): what comes back for a name, and how the service parameters read once
// they are out of their wire format.
//
#include "resolver_fixture.hpp"

#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>

using namespace boost::asio;
using boost::system::error_code;
using cares_asio::make_ares_error;
using testing::AnyOf;
using testing::ElementsAre;
using testing::IsEmpty;

using Https = resolver_fixture;

TEST_F(Https, ResolvesTheRecordOfAName)
{
   server.add_https("www.example.test", 1, ".",
                    {{ARES_SVCB_PARAM_ALPN, {2, 'h', '3', 2, 'h', '2'}}});

   const auto [ec, records] = resolve_https("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   ASSERT_EQ(records.size(), 1u);
   EXPECT_EQ(records[0].priority, 1);
   EXPECT_THAT(records[0].target, AnyOf("", ".")) << "the record's own name";
   EXPECT_THAT(records[0].params, ElementsAre("alpn=h3,h2"));

   EXPECT_EQ(server.count("www.example.test", ARES_REC_TYPE_HTTPS), 1u);
}

TEST_F(Https, RendersEveryKindOfParameter)
{
   server.add_https(
      "www.example.test", 1, "svc.example.test",
      {
         {ARES_SVCB_PARAM_MANDATORY, {0, 1, 0, 3}}, // the keys of alpn and port
         {ARES_SVCB_PARAM_ALPN, {2, 'h', '3', 2, 'h', '2'}},
         {ARES_SVCB_PARAM_NO_DEFAULT_ALPN, {}},
         {ARES_SVCB_PARAM_PORT, {0x20, 0xfb}},
         {ARES_SVCB_PARAM_IPV4HINT, {192, 0, 2, 1, 192, 0, 2, 2}},
         {ARES_SVCB_PARAM_ECH, {0xde, 0xad, 0xbe, 0xef}},
         {ARES_SVCB_PARAM_IPV6HINT, {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
      });

   const auto [ec, records] = resolve_https("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   ASSERT_EQ(records.size(), 1u);
   EXPECT_EQ(records[0].target, "svc.example.test");
   EXPECT_THAT(records[0].params,
               ElementsAre("mandatory=alpn,port", "alpn=h3,h2", "no-default-alpn", "port=8443",
                           "ipv4hint=192.0.2.1,192.0.2.2", "ech=deadbeef", "ipv6hint=2001:db8::1"));
}

TEST_F(Https, ShowsAParameterItHasNoNameFor)
{
   //
   // 65280 and up are for private use, so no version of c-ares is going to know this one.
   //
   server.add_https("www.example.test", 1, ".", {{65280, {0x01, 0x02, 0xff}}});

   const auto [ec, records] = resolve_https("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   ASSERT_EQ(records.size(), 1u);
   EXPECT_THAT(records[0].params, ElementsAre("key65280=0102ff"));
}

TEST_F(Https, ResolvesAnAlias)
{
   //
   // Priority 0 is the alias form: no parameters, only the name to go on with.
   //
   server.add_https("example.test", 0, "pool.example.test");

   const auto [ec, records] = resolve_https("example.test");

   EXPECT_FALSE(ec) << ec.message();
   ASSERT_EQ(records.size(), 1u);
   EXPECT_EQ(records[0].priority, 0);
   EXPECT_EQ(records[0].target, "pool.example.test");
   EXPECT_THAT(records[0].params, IsEmpty());
}

TEST_F(Https, ResolvesEveryRecordOfAName)
{
   server.add_https("www.example.test", 1, ".", {{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}}});
   server.add_https("www.example.test", 2, "fallback.example.test",
                    {{ARES_SVCB_PARAM_ALPN, {2, 'h', '2'}}});

   auto [ec, records] = resolve_https("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   ASSERT_EQ(records.size(), 2u);

   // a record set has no order to rely on
   std::ranges::sort(records, {}, &cares_asio::https_record::priority);
   EXPECT_THAT(records[0].params, ElementsAre("alpn=h3"));
   EXPECT_EQ(records[1].target, "fallback.example.test");
   EXPECT_THAT(records[1].params, ElementsAre("alpn=h2"));
}

TEST_F(Https, LeavesOutTheAliasesThatLedToTheRecord)
{
   //
   // The answer has the CNAME in it as well, ahead of the record it leads to.
   //
   server.add_cname("www.example.test", "host.example.test");
   server.add_https("host.example.test", 1, ".", {{ARES_SVCB_PARAM_ALPN, {2, 'h', '2'}}});

   const auto [ec, records] = resolve_https("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   ASSERT_EQ(records.size(), 1u);
   EXPECT_THAT(records[0].params, ElementsAre("alpn=h2"));
}

TEST_F(Https, ReportsANameWithoutARecord)
{
   //
   // The common case: the name is there, an HTTPS record for it is not.
   //
   server.add_address("www.example.test", "192.0.2.1");

   const auto [ec, records] = resolve_https("www.example.test");

   EXPECT_EQ(ec, make_ares_error(ARES_ENODATA)) << ec.message();
   EXPECT_THAT(records, IsEmpty());
}

TEST_F(Https, ReportsANameThatDoesNotExist)
{
   const auto [ec, records] = resolve_https("nowhere.example.test");

   EXPECT_EQ(ec, make_ares_error(ARES_ENOTFOUND)) << ec.message();
   EXPECT_THAT(records, IsEmpty());
}

TEST_F(Https, GoesOutTogetherWithTheAddresses)
{
   //
   // Two questions about one name, in flight at the same time -- which is how a client that
   // wants both would ask them.
   //
   server.add_address("www.example.test", "192.0.2.1");
   server.add_https("www.example.test", 1, ".", {{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}}});

   const auto [order, ec, endpoints, https_ec, records] = run(
      context,
      experimental::make_parallel_group(resolver.async_resolve("www.example.test", "443", deferred),
                                        resolver.async_resolve_https("www.example.test", deferred))
         .async_wait(experimental::wait_for_all(), use_awaitable));

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_FALSE(https_ec) << https_ec.message();
   ASSERT_EQ(records.size(), 1u);
   EXPECT_THAT(records[0].params, ElementsAre("alpn=h3"));
}
