// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// Uses enough of the library to need its headers, its symbols and those of its dependencies, and
// nothing that needs a network: an address is resolved without asking anybody.
//
#include <cares_async.hpp>

#include <boost/asio/io_context.hpp>

#include <cstdlib>
#include <iostream>

int main()
{
   boost::asio::io_context context;
   cares_async::resolver resolver(context.get_executor());

   bool resolved = false;
   resolver.async_resolve(
      "127.0.0.1", "443",
      [&](boost::system::error_code ec, const cares_async::resolver::results_type& endpoints) {
         resolved = !ec && endpoints.size() == 1 && endpoints.front().address().is_loopback() &&
                    endpoints.front().port() == 443;
      });
   context.run();

   std::cout << "cares_async " << CARES_ASYNC_VERSION_STRING << ": "
             << (resolved ? "resolved" : "FAILED") << std::endl;

   return resolved ? EXIT_SUCCESS : EXIT_FAILURE;
}
