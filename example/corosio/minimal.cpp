// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The shortest program that resolves a name, on capy/corosio: the addresses of a host, and then
// its HTTPS record, each awaited in a coroutine. ../asio/minimal.cpp is the same on Boost.Asio.
//
//    minimal [host]
//
#include <cares_asio.hpp>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>

#include <exception>
#include <format>
#include <iostream>
#include <string>

cares_asio::Task<void> lookup(cares_asio::Executor executor, std::string host)
{
   cares_asio::resolver resolver(executor);

   const auto [ec, endpoints] = co_await resolver.resolve(host, "443");

   if (ec)
      std::cerr << host << ": " << ec.message() << '\n';

   //
   // corosio has no output operator for an endpoint, so here is one: the address, in brackets
   // where it is an IPv6 one, and the port behind it.
   //
   const auto text = [](const cares_asio::Endpoint& endpoint) {
      return endpoint.address().is_v6()
                ? std::format("[{}]:{}", endpoint.address().to_string(), endpoint.port())
                : std::format("{}:{}", endpoint.address().to_string(), endpoint.port());
   };

   for (const auto& endpoint : endpoints)
      std::cout << host << ": " << text(endpoint) << '\n';

   //
   // Most names have no HTTPS record, which is no error worth a mention.
   //
   const auto [https_ec, records] = co_await resolver.resolve_https(host);

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
   boost::corosio::io_context context;

   //
   // The executor is handed to the coroutine rather than taken from inside it: what
   // capy::this_coro::executor yields does not own its context, and the resolver keeps the one
   // it was given.
   //
   const cares_asio::Executor executor(context.get_executor());
   boost::capy::run_async(
      executor, []() noexcept {},
      [](std::exception_ptr) noexcept {})(lookup(executor, argc > 1 ? argv[1] : "example.com"));

   context.run();
}
