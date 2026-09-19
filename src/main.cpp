//
// Standalone ASIO starting point: resolves the host names given on the command line, one after
// the other, using ASIO's own resolver. That resolver is what the c-ares integration is meant to
// replace, so this is the behaviour to compare against.
//
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/program_options.hpp>

#include <spdlog/spdlog.h>

#include <expected>
#include <iostream>
#include <string>
#include <vector>

using namespace boost::asio;
namespace po = boost::program_options;

struct Config
{
   bool verbose = false;
   std::string service = "80";
   std::vector<std::string> hosts;
};

std::expected<Config, int> parseConfig(int argc, char* argv[])
{
   Config config;

   po::options_description desc("Allowed options");
   auto opts = desc.add_options();
   opts("help,h", "produce help message");
   opts("verbose,v", po::bool_switch(&config.verbose), "enable verbose logging");
   opts("service,s", po::value(&config.service)->default_value(config.service),
        "service name or port number to resolve for");
   opts("host", po::value(&config.hosts)->multitoken(), "host name(s) to resolve");

   po::positional_options_description positional;
   positional.add("host", -1);

   po::variables_map vm;
   try
   {
      po::store(po::command_line_parser(argc, argv).options(desc).positional(positional).run(), vm);
      po::notify(vm);
   }
   catch (const po::error& ex)
   {
      std::cerr << ex.what() << std::endl << desc;
      return std::unexpected(EXIT_FAILURE);
   }

   if (vm.count("help"))
   {
      std::cout << "Usage: resolve [options] <host>..." << std::endl << desc;
      return std::unexpected(EXIT_SUCCESS);
   }

   if (config.hosts.empty())
   {
      std::cerr << "no host given" << std::endl << desc;
      return std::unexpected(EXIT_FAILURE);
   }

   return config;
}

awaitable<void> resolve(const Config& config)
{
   ip::tcp::resolver resolver(co_await this_coro::executor);
   for (const auto& host : config.hosts)
   {
      spdlog::debug("resolving {}:{}...", host, config.service);
      auto [ec, results] = co_await resolver.async_resolve(host, config.service, as_tuple);
      if (ec)
      {
         spdlog::error("{}: {}", host, ec.message());
         continue;
      }

      for (const auto& result : results)
         spdlog::info("{}: {}", host, result.endpoint().address().to_string());
   }
}

int main(int argc, char* argv[])
{
   auto config = parseConfig(argc, argv);
   if (!config)
      return config.error();

   spdlog::set_pattern("%H:%M:%S.%e %^%l%$ %v");
   spdlog::set_level(config->verbose ? spdlog::level::debug : spdlog::level::info);

   io_context context;

   signal_set signals(context, SIGINT, SIGTERM);
   signals.async_wait(
      [&](const boost::system::error_code& ec, int signal)
      {
         if (ec)
            return;
         spdlog::warn("received signal {}, stopping...", signal);
         context.stop();
      });

   co_spawn(context, resolve(*config),
            [&](std::exception_ptr ep)
            {
               signals.cancel();
               if (ep)
                  std::rethrow_exception(ep);
            });

   context.run();
   return EXIT_SUCCESS;
}
