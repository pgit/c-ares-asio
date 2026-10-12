//
// Uses enough of the library to need its headers, its symbols and those of its dependencies, and
// nothing that needs a network: an address is resolved without asking anybody.
//
#include <cares_asio.hpp>

#include <boost/asio/io_context.hpp>

#include <cstdlib>
#include <iostream>

int main()
{
   boost::asio::io_context context;
   cares_asio::resolver resolver(context.get_executor());

   bool resolved = false;
   resolver.async_resolve("127.0.0.1", "443",
                          [&](boost::system::error_code ec,
                              const cares_asio::resolver::results_type& endpoints) {
                             resolved = !ec && endpoints.size() == 1 &&
                                        endpoints.front().address().is_loopback() &&
                                        endpoints.front().port() == 443;
                          });
   context.run();

   std::cout << "cares_asio " << CARES_ASIO_VERSION_STRING << ": "
             << (resolved ? "resolved" : "FAILED") << std::endl;

   return resolved ? EXIT_SUCCESS : EXIT_FAILURE;
}
