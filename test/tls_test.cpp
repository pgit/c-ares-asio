// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// DNS over TLS, against a server that presents a certificate made up for the occasion.
//
#include "resolver_fixture.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

using c_ares_async::error_code;
using c_ares_async::log_level;
using c_ares_async::make_ares_error;
using c_ares_async::tls_options;
using testing::Contains;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Pair;

using transport = fake_dns_server::transport;

namespace
{

constexpr const char* server_name = "dns.example.test";

} // namespace

class DnsOverTls : public event_loop_fixture
{
protected:
   void SetUp() override
   {
      server.use_tls(certificate);
      server.add_address("www.example.test", "192.0.2.1");
      server.add_https("www.example.test", 1, ".", {{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}}});
   }

   void TearDown() override
   {
      resolver.reset();

      ::unsetenv("SSL_CERT_FILE");
      if (!trusted.empty())
         std::remove(trusted.c_str());
   }

   //
   // Makes the server's certificate one that the TLS library trusts, which has to be done
   // before the resolver is made. The resolver verifies against the default trust store and
   // has no way of being given another, but both OpenSSL and AWS-LC let SSL_CERT_FILE say which
   // file that is.
   //
   void trust_the_server()
   {
      trusted = testing::TempDir() + "c_ares_async_test_" + std::to_string(::getpid()) + ".pem";
      std::ofstream(trusted) << certificate.certificate_pem;
      ASSERT_EQ(::setenv("SSL_CERT_FILE", trusted.c_str(), 1), 0);
   }

   //
   // The resolver under test, which only ever asks the fake server.
   //
   void make_resolver(const tls_options& tls)
   {
      resolver = std::make_unique<c_ares_async::resolver>(executor(), tls);
      resolver->set_logger([this](log_level level, std::string_view message) {
         messages.emplace_back(level, message);
      });

      ASSERT_FALSE(resolver->set_servers(server.address()));
   }

   auto resolve(std::string_view host) { return run(resolver->resolve(std::string(host), "443")); }

   static inline const test_certificate certificate = make_test_certificate(server_name);

   fake_dns_server server{server_executor()};

   //
   // Held by pointer rather than in a std::optional, which asks whether a resolver converts to
   // an executor -- a question Capy's any_executor answers by failing to compile, see
   // doc/corosio.rst.
   //
   std::unique_ptr<c_ares_async::resolver> resolver;

   std::vector<std::pair<log_level, std::string>> messages;
   std::string trusted;
};

// --------------------------------------------------------------------------------------------------

TEST_F(DnsOverTls, ResolvesOverTlsAndNothingElse)
{
   make_resolver({.enabled = true, .hostname = {}, .verify = false});

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));

   EXPECT_EQ(server.handshakes(), 1u);
   EXPECT_GE(server.count(transport::tls), 1u);
   EXPECT_EQ(server.count(transport::udp), 0u);
   EXPECT_EQ(server.count(transport::tcp), 0u);
}

TEST_F(DnsOverTls, VerifiesTheCertificateAgainstTheName)
{
   trust_the_server();
   make_resolver({.enabled = true, .hostname = server_name, .verify = true});

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_EQ(server.handshakes(), 1u);
}

TEST_F(DnsOverTls, RejectsACertificateForAnotherName)
{
   trust_the_server();
   make_resolver({.enabled = true, .hostname = "other.example.test", .verify = true});

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_EQ(server.handshakes(), 0u);
   EXPECT_THAT(server.queries(), IsEmpty()) << "not a question before the server is who it says";

   //
   // c-ares only ever sees a connection that went away, so the reason is in the log or nowhere.
   //
   EXPECT_THAT(messages, Contains(Pair(log_level::error, HasSubstr("TLS handshake failed"))));
}

TEST_F(DnsOverTls, RejectsACertificateNobodyVouchesFor)
{
   // the name is right, but the certificate is in no trust store
   make_resolver({.enabled = true, .hostname = server_name, .verify = true});

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
   EXPECT_EQ(server.handshakes(), 0u);
   EXPECT_THAT(server.queries(), IsEmpty());
   EXPECT_THAT(messages, Contains(Pair(log_level::error, HasSubstr("TLS handshake failed"))));
}

TEST_F(DnsOverTls, TakesAnyCertificateWhenToldNotToVerify)
{
   // neither trusted nor for this name
   make_resolver({.enabled = true, .hostname = "other.example.test", .verify = false});

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(DnsOverTls, VerifiesTheChainWhereThereIsNoNameToCheck)
{
   trust_the_server();
   make_resolver({.enabled = true, .hostname = {}, .verify = true});

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(DnsOverTls, KeepsItsServersWhenGivenNone)
{
   make_resolver({.enabled = true, .hostname = {}, .verify = false});

   //
   // That is all an empty list is to do. In particular it is not to send the server that was
   // set, with a port of its own, off to the one that DNS over TLS has by default.
   //
   EXPECT_FALSE(resolver->set_servers(""));

   const auto [ec, endpoints] = resolve("www.example.test");
   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
}

TEST_F(DnsOverTls, KeepsTheConnectionForTheNextQuestion)
{
   make_resolver({.enabled = true, .hostname = {}, .verify = false});

   for (int i = 0; i < 3; ++i)
   {
      const auto [ec, endpoints] = resolve("www.example.test");
      EXPECT_FALSE(ec) << ec.message();
   }

   EXPECT_EQ(server.connections(), 1u) << "a handshake per name is what this is there to avoid";
   EXPECT_EQ(server.handshakes(), 1u);
}

TEST_F(DnsOverTls, SendsAddressAndHttpsQueriesDownOneConnection)
{
   make_resolver({.enabled = true, .hostname = {}, .verify = false});

   //
   // Both in flight at the same time, so that it is one connection carrying the two of them
   // rather than one after the other.
   //
   std::optional<std::tuple<error_code, c_ares_async::resolver::results_type>> addresses;
   std::optional<std::tuple<error_code, c_ares_async::resolver::https_results_type>> https;

   start([](c_ares_async::resolver& resolver, auto& addresses) -> c_ares_async::Task<void> {
      addresses = co_await resolver.resolve("www.example.test", "443");
   }(*resolver, addresses));

   start([](c_ares_async::resolver& resolver, auto& https) -> c_ares_async::Task<void> {
      https = co_await resolver.resolve_https("www.example.test");
   }(*resolver, https));

   ASSERT_TRUE(run_until([&] { return addresses && https; }));

   const auto& [ec, endpoints] = *addresses;
   const auto& [https_ec, records] = *https;

   EXPECT_FALSE(ec) << ec.message();
   EXPECT_THAT(endpoints, ElementsAre(endpoint("192.0.2.1", 443)));
   EXPECT_FALSE(https_ec) << https_ec.message();
   ASSERT_EQ(records.size(), 1u);
   EXPECT_THAT(records[0].params, ElementsAre("alpn=h3"));

   EXPECT_EQ(server.connections(), 1u);
   EXPECT_EQ(server.count(transport::udp), 0u);
}

TEST_F(DnsOverTls, SaysWhatWasNegotiatedAndWithWhom)
{
   trust_the_server();
   make_resolver({.enabled = true, .hostname = server_name, .verify = true});

   resolve("www.example.test");

   EXPECT_THAT(messages, Contains(Pair(log_level::info, HasSubstr("TLS established: TLSv1.3"))));
   EXPECT_THAT(messages, Contains(Pair(log_level::info, HasSubstr("CN=dns.example.test"))));
   EXPECT_THAT(messages, Contains(Pair(log_level::info, HasSubstr(", verified"))));
}

TEST_F(DnsOverTls, DoesNotCallAnUncheckedCertificateVerified)
{
   make_resolver({.enabled = true, .hostname = {}, .verify = false});

   resolve("www.example.test");

   EXPECT_THAT(messages, Contains(Pair(log_level::info, HasSubstr("NOT verified"))));
}

TEST_F(DnsOverTls, ReportsAServerThatIsNotThere)
{
   make_resolver({.enabled = true, .hostname = {}, .verify = false});

   //
   // A port that was free a moment ago, with nobody listening on it now.
   //
   std::string nowhere;
   {
      fake_dns_server gone(server_executor());
      nowhere = gone.address();
   }
   ASSERT_FALSE(resolver->set_servers(nowhere));

   const auto [ec, endpoints] = resolve("www.example.test");

   EXPECT_TRUE(ec);
   EXPECT_THAT(endpoints, IsEmpty());
}
