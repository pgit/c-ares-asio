// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// What nearly every test starts from: an io_context, a DNS server to ask and a resolver that asks
// nobody else.
//
#pragma once

#include "fake_dns_server.hpp"
#include "run.hpp"

#include <cares_asio/resolver.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string_view>

class resolver_fixture : public testing::Test
{
protected:
   void SetUp() override { ASSERT_FALSE(resolver.set_servers(server.address())); }

   //
   // Both operations, run to completion: a tuple of the error and the result.
   //
   auto resolve(std::string_view host, std::string_view service = "443")
   {
      return run(context, resolver.async_resolve(host, service, awaited));
   }

   auto resolve_https(std::string_view host)
   {
      return run(context, resolver.async_resolve_https(host, awaited));
   }

   static boost::asio::ip::tcp::endpoint endpoint(std::string_view address, unsigned short port)
   {
      return {boost::asio::ip::make_address(address), port};
   }

   static constexpr auto awaited = boost::asio::as_tuple(boost::asio::use_awaitable);

   //
   // In this order, as the resolver and the server both hang on to the context.
   //
   boost::asio::io_context context;
   fake_dns_server server{context.get_executor()};
   cares_asio::resolver resolver{context.get_executor()};
};
