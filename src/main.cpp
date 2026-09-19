//
// Resolves the host names given on the command line, one after the other, using c-ares driven by
// the ASIO event loop. With --asio, ASIO's own resolver (a getaddrinfo() call on an internal
// thread) is used instead, which is the behaviour to compare against.
//
#include "utils.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/program_options.hpp>

#include <spdlog/spdlog.h>

#include "ares_resolver.hpp"

#include <expected>
#include <format>
#include <iostream>
#include <string>
#include <vector>

using namespace boost::asio;
namespace po = boost::program_options;

struct Config
{
   bool verbose = false;
   bool asio = false;
   std::string service = "443";
   std::string servers;
   std::vector<std::string> hosts;
   TlsOptions tls;
};

std::expected<Config, int> parseConfig(int argc, char* argv[])
{
   Config config;

   po::options_description desc("Allowed options");
   auto opts = desc.add_options();
   opts("help,h", "produce help message");
   opts("verbose,v", po::bool_switch(&config.verbose), "enable verbose logging");
   opts("asio,a", po::bool_switch(&config.asio), "use ASIO's own resolver instead of c-ares");
   opts("service,s", po::value(&config.service)->default_value(config.service),
        "service name or port number to resolve for");
   opts("server,n", po::value(&config.servers),
        "DNS server(s) to query instead of the ones from /etc/resolv.conf, comma separated "
        "(c-ares only) -- try 192.0.2.1 for a black hole");
   opts("dot,t", po::bool_switch(&config.tls.enabled),
        "use DNS over TLS (RFC 7858) -- forces TCP and moves the servers to port 853 (c-ares "
        "only)");
   opts("tls-hostname", po::value(&config.tls.hostname),
        "server name to use for SNI and to verify the certificate against, e.g. "
        "one.one.one.one");
   opts("tls-no-verify",
        po::value<bool>()->zero_tokens()->notifier([&config](bool) { config.tls.verify = false; }),
        "accept any certificate (testing only)");
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

   if (config.tls.enabled)
   {
      if (config.asio)
      {
         std::cerr << "--dot is not supported by the ASIO resolver" << std::endl << desc;
         return std::unexpected(EXIT_FAILURE);
      }

      //
      // --server takes an address, so there is no name to check the certificate against unless
      // one is given. Rather than quietly talk to whoever answers, say so.
      //
      if (config.tls.verify && config.tls.hostname.empty())
      {
         std::cerr << "--dot needs --tls-hostname to verify the server certificate against, "
                      "or --tls-no-verify to skip the check"
                   << std::endl
                   << desc;
         return std::unexpected(EXIT_FAILURE);
      }
   }
   else if (!config.tls.hostname.empty() || !config.tls.verify)
   {
      std::cerr << "--tls-hostname and --tls-no-verify only apply with --dot" << std::endl << desc;
      return std::unexpected(EXIT_FAILURE);
   }

   return config;
}

awaitable<void> resolveWithAsio(const Config& config)
{
   co_await this_coro::throw_if_cancelled(false);

   if (!config.servers.empty())
      spdlog::warn("--server is not supported by the ASIO resolver, ignoring it");

   //
   // getaddrinfo() answers with addresses and nothing else, so this path is addresses only.
   //
   spdlog::debug("the ASIO resolver cannot look up HTTPS records");

   ip::tcp::resolver resolver(co_await this_coro::executor);
   for (const auto& host : config.hosts)
   {
      spdlog::debug("resolving {}:{}...", host, config.service);
      auto [ec, results] = co_await resolver.async_resolve(host, config.service, as_tuple);
      if (ec)
      {
         if (ec == error::operation_aborted)
            co_return;
         spdlog::error("{}: {}", host, ec.message());
         continue;
      }

      for (const auto& result : results)
         spdlog::info("{}: {}", host, result.endpoint().address().to_string());
   }
}

//
// Everything asked about one name. The addresses and the HTTPS record (RFC 9460) are two separate
// questions, so they go out together rather than one after the other -- c-ares has no trouble
// keeping several queries in flight on a channel, and with --dot they share the connection as
// well as the round trip.
//
// Returns false when the lookup was cancelled and the caller should stop.
//
awaitable<bool> resolveHost(AresResolver& resolver, const Config& config, const std::string& host)
{
   spdlog::debug("resolving {}:{}...", host, config.service);

   auto [order, ec, endpoints, httpsEc, records] =
      co_await experimental::make_parallel_group(
         resolver.async_resolve(host, config.service, deferred),
         resolver.async_lookupHttps(host, deferred))
         .async_wait(experimental::wait_for_all(), use_awaitable);

   if (ec == error::operation_aborted || httpsEc == error::operation_aborted)
      co_return false;

   if (ec)
      spdlog::error("{}: {}", host, ec.message());

   for (const auto& endpoint : endpoints)
      spdlog::info("{}: {}", host, endpoint.address().to_string());

   //
   // A name that cannot be encoded as a DNS name at all is no question for any record type, and
   // c-ares turns that one down locally, without a query. But where ares_getaddrinfo() says
   // ARES_EBADNAME, ares_search_dnsrec() says ARES_ENOMEM (c-ares 1.34.5), so the address error
   // is the one to believe and repeating it as a memory problem would only mislead.
   //
   if (ec != make_ares_error(ARES_EBADNAME))
   {
      if (httpsEc == make_ares_error(ARES_ENODATA) || httpsEc == make_ares_error(ARES_ENOTFOUND))
         spdlog::debug("{}: no HTTPS record", host);
      else if (httpsEc)
         spdlog::warn("{}: HTTPS: {}", host, httpsEc.message());
   }

   for (const auto& record : records)
   {
      //
      // An empty target is the record's own name, which is a bare dot in presentation form.
      //
      std::string text =
         std::format("HTTPS {} {}", record.priority, record.target.empty() ? "." : record.target);
      for (const auto& param : record.params)
         text += ' ' + param;

      spdlog::info("{}: {}", host, text);
   }

   co_return true;
}

awaitable<void> resolveWithAres(const Config& config)
{
   co_await this_coro::throw_if_cancelled(false);

   AresResolver resolver(co_await this_coro::executor, config.tls);
   if (auto ec = resolver.setServers(config.servers))
   {
      spdlog::error("--server '{}': {}", config.servers, ec.message());
      co_return;
   }

   for (const auto& host : config.hosts)
      if (!co_await resolveHost(resolver, config, host))
         co_return;
}

int main(int argc, char* argv[])
{
   auto config = parseConfig(argc, argv);
   if (!config)
      return config.error();

   spdlog::set_pattern("%H:%M:%S.%e %^%l%$ %v");
   spdlog::set_level(config->verbose ? spdlog::level::debug : spdlog::level::info);

   io_context context;

   //
   // Cancel the lookup instead of stopping the io_context, so that everything gets a chance to
   // unwind -- the pending request belongs to c-ares, the io_context knows nothing about it.
   //
   cancellation_signal cancellation;

   signal_set signals(context, SIGINT, SIGTERM);
   signals.async_wait([&](const boost::system::error_code& ec, int signal) {
      if (ec)
         return;
      spdlog::warn("received signal {}, stopping...", signal);
      cancellation.emit(cancellation_type::all);
   });

   co_spawn(context, config->asio ? resolveWithAsio(*config) : resolveWithAres(*config),
            bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr ep) {
               signals.cancel();
               if (ep)
                  std::rethrow_exception(ep);
            }));

   if (config->verbose)
      run(context); // one line per completion handler, with timing
   else
      context.run();

   return EXIT_SUCCESS;
}
