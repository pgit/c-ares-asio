// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The shortest program that resolves a name: the addresses of a host, and then its HTTPS record,
// each awaited in a coroutine.
//
//    minimal [host]
//
#include <cares_asio.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <iostream>
#include <string>

namespace asio = boost::asio;

asio::awaitable<void> lookup(std::string host)
{
   cares_asio::resolver resolver(co_await asio::this_coro::executor);

   const auto [ec, endpoints] =
      co_await resolver.async_resolve(host, "443", asio::as_tuple(asio::use_awaitable));

   if (ec)
      std::cerr << host << ": " << ec.message() << '\n';

   for (const auto& endpoint : endpoints)
      std::cout << host << ": " << endpoint << '\n';

   //
   // Most names have no HTTPS record, which is no error worth a mention.
   //
   const auto [https_ec, records] =
      co_await resolver.async_resolve_https(host, asio::as_tuple(asio::use_awaitable));

   if (https_ec && https_ec != cares_asio::make_ares_error(ARES_ENODATA))
      std::cerr << host << ": HTTPS: " << https_ec.message() << '\n';

   for (const auto& record : records)
   {
      std::cout << host << ": HTTPS " << record.priority << ' '
                << (record.target.empty() ? "." : record.target);
      for (const auto& param : record.params)
         std::cout << ' ' << param;
      std::cout << '\n';
   }
}

int main(int argc, char* argv[])
{
   asio::io_context context;
   asio::co_spawn(context, lookup(argc > 1 ? argv[1] : "example.com"), asio::detached);
   context.run();
}
