// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// What nearly every test starts from: the event loop of the runtime, a DNS server to ask and a
// resolver that asks nobody else.
//
#pragma once

#include "event_loop_fixture.hpp"
#include "fake_dns_server.hpp"

#include <cares_asio/resolver.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

class resolver_fixture : public event_loop_fixture
{
protected:
   void SetUp() override { ASSERT_FALSE(resolver.set_servers(server.address())); }

   //
   // Both operations, run to completion: a tuple of the error and the result. This is the
   // spelling that both runtimes have, so the tests using it are the same on either.
   //
   auto resolve(std::string_view host, std::string_view service = "443")
   {
      return run(resolver.resolve(std::string(host), std::string(service)));
   }

   auto resolve_https(std::string_view host)
   {
      return run(resolver.resolve_https(std::string(host)));
   }

   //
   // In this order, as the resolver and the server both hang on to their loops.
   //
   fake_dns_server server{server_executor()};
   cares_asio::resolver resolver{executor()};
};
