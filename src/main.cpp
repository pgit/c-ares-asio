//
// Resolves the host names given on the command line, or those of the URLs listed in a file, using
// c-ares driven by the ASIO event loop. With --asio, ASIO's own resolver (a getaddrinfo() call on
// an internal thread) is used instead, which is the behaviour to compare against.
//
#include "utils.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/program_options.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "ares_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <unistd.h>

using namespace boost::asio;
namespace po = boost::program_options;

struct Config
{
   bool verbose = false;
   bool asio = false;
   bool ordered = false;
   bool color = false;
   int concurrency = 10;
   std::string service = "443";
   std::string servers;
   std::string file; // where the URLs came from, which also asks for the one-line-per-host listing
   std::vector<std::string> hosts;
   TlsOptions tls;
};

//
// The host of a URL, which is all of it that DNS is ever asked about. Anything without a scheme
// is taken to start at the host already, so a plain host name comes back as it is.
//
std::string_view hostOf(std::string_view url)
{
   //
   // "://" only ends a scheme where it comes first -- further in, it belongs to a path or query.
   //
   if (const auto scheme = url.find("://"); scheme < url.find_first_of("/?#"))
      url.remove_prefix(scheme + 3);

   url = url.substr(0, url.find_first_of("/?#"));

   if (const auto at = url.rfind('@'); at != std::string_view::npos)
      url.remove_prefix(at + 1); // user:password@

   //
   // An IPv6 address comes in brackets, which is what keeps its colons apart from the one in
   // front of a port. Without brackets, more than one colon means there is no port.
   //
   if (url.starts_with('['))
      return url.substr(1, url.find(']') - 1);

   const auto colon = url.find(':');
   return colon == url.rfind(':') ? url.substr(0, colon) : url;
}

//
// Appends the hosts of the URLs in 'path' to 'hosts', one URL per line, skipping blank lines and
// '#' comments. Several URLs on one host are still only one name to look up, so every host is
// taken once, where it comes first. Returns false if the file cannot be read.
//
bool readHosts(const std::string& path, std::vector<std::string>& hosts)
{
   std::ifstream file(path);
   if (!file)
      return false;

   std::unordered_set<std::string> seen(hosts.begin(), hosts.end());
   for (std::string line; std::getline(file, line);)
   {
      const auto first = line.find_first_not_of(" \t\r");
      if (first == std::string::npos || line[first] == '#')
         continue;

      const auto last = line.find_last_not_of(" \t\r");
      std::string host(hostOf(std::string_view(line).substr(first, last - first + 1)));
      std::ranges::transform(host, host.begin(), [](unsigned char c) { return std::tolower(c); });

      if (!host.empty() && seen.insert(host).second)
         hosts.push_back(std::move(host));
   }

   return !file.bad();
}

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
   opts("file,f", po::value(&config.file),
        "file of URLs to resolve the hosts of, one per line -- the results are listed one line "
        "per host: name, first IPv4 address, first IPv6 address, HTTPS record");
   opts("concurrency,c", po::value(&config.concurrency)->default_value(config.concurrency),
        "number of hosts to look up at the same time");
   opts("ordered,o", po::bool_switch(&config.ordered),
        "print the results in the order the hosts were given, rather than as the answers come in");
   opts("color", po::bool_switch(&config.color),
        "colour the output even where it does not go to a terminal, e.g. into 'less -R'");
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
      std::cout << "Usage: resolve [options] <host>..." << std::endl
                << "       resolve [options] --file <urls>" << std::endl
                << desc;
      return std::unexpected(EXIT_SUCCESS);
   }

   if (config.concurrency < 1)
   {
      std::cerr << "--concurrency has to be at least 1" << std::endl << desc;
      return std::unexpected(EXIT_FAILURE);
   }

   if (!config.file.empty() && !readHosts(config.file, config.hosts))
   {
      std::cerr << "cannot read " << config.file << ": " << std::strerror(errno) << std::endl;
      return std::unexpected(EXIT_FAILURE);
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

//
// What came back for one name. The ASIO resolver has no way to ask for the HTTPS record, so that
// half stays empty there.
//
struct Lookup
{
   boost::system::error_code ec;
   AresResolver::Results endpoints;
   boost::system::error_code httpsEc;
   AresResolver::HttpsResults records;
};

using LookupFunction = std::function<awaitable<Lookup>(const std::string& host)>;

//
// A name without an HTTPS record is the common case rather than a problem.
//
bool isNoRecord(const boost::system::error_code& ec)
{
   return ec == make_ares_error(ARES_ENODATA) || ec == make_ares_error(ARES_ENOTFOUND);
}

//
// Addresses come in the colours "ip -color address" gives them: IPv4 in magenta, IPv6 in blue.
// Host names are yellow. Only a terminal gets any of it unless --color says otherwise, which is
// where spdlog colours the log level as well.
//
enum class Color { name = 33, v6 = 34, v4 = 35 };

bool useColor = false;

std::string colored(std::string_view text, Color color)
{
   return useColor ? std::format("\x1b[{}m{}\x1b[0m", std::to_underlying(color), text)
                   : std::string(text);
}

//
// Padded to 'width' before the colour goes on, as the escape sequences would count towards it.
//
std::string toText(const ip::address& address, size_t width = 0)
{
   return colored(std::format("{:{}}", address.to_string(), width),
                  address.is_v4() ? Color::v4 : Color::v6);
}

std::string toText(const HttpsRecord& record)
{
   //
   // An empty target is the record's own name, which is a bare dot in presentation form.
   //
   std::string text = std::format(
      "{} {}", record.priority, record.target.empty() ? "." : colored(record.target, Color::name));
   for (std::string_view param : record.params)
   {
      //
      // The address hints are addresses like any other, the rest of the parameters are not.
      //
      const bool v4 = param.starts_with("ipv4hint=");
      const auto value = v4 || param.starts_with("ipv6hint=") ? param.find('=') + 1 : param.size();

      text += ' ';
      text += param.substr(0, value);
      if (value < param.size())
         text += colored(param.substr(value), v4 ? Color::v4 : Color::v6);
   }

   return text;
}

//
// Everything there is to say about one name: every address and every HTTPS record on a line of
// its own.
//
void reportInFull(const std::string& name, const Lookup& lookup)
{
   const auto host = colored(name, Color::name);

   if (lookup.ec)
      spdlog::error("{}: {}", host, lookup.ec.message());

   for (const auto& endpoint : lookup.endpoints)
      spdlog::info("{}: {}", host, toText(endpoint.address()));

   if (isNoRecord(lookup.httpsEc))
      spdlog::debug("{}: no HTTPS record", host);
   else if (lookup.httpsEc)
      spdlog::warn("{}: HTTPS: {}", host, lookup.httpsEc.message());

   for (const auto& record : lookup.records)
      spdlog::info("{}: HTTPS {}", host, toText(record));
}

//
// One name per line: the name, its first IPv4 address, its first IPv6 address and its HTTPS
// record, with a dash for whatever there is none of. What went wrong, if anything did, follows in
// parentheses.
//
void reportCompact(const std::string& host, const Lookup& lookup, size_t width)
{
   const auto first = [&](bool v4, size_t width) {
      const auto it = std::ranges::find(
         lookup.endpoints, v4, [](const auto& endpoint) { return endpoint.address().is_v4(); });
      return it == lookup.endpoints.end() ? std::format("{:{}}", "-", width)
                                          : toText(it->address(), width);
   };

   std::string https;
   for (const auto& record : lookup.records)
      https += (https.empty() ? "" : "; ") + toText(record);

   //
   // Both questions usually fail the same way -- no server, no answer -- and once is enough then.
   //
   std::string failure = lookup.ec ? lookup.ec.message() : std::string();
   if (lookup.httpsEc && !isNoRecord(lookup.httpsEc) && lookup.httpsEc != lookup.ec)
      failure += (failure.empty() ? "" : "; ") + ("HTTPS: " + lookup.httpsEc.message());

   std::println("{}  {}  {}  {}{}", colored(std::format("{:{}}", host, width), Color::name),
                first(true, 15), first(false, 39), https.empty() ? "-" : https,
                failure.empty() ? "" : std::format("  ({})", failure));

   //
   // The rows are the progress report as well. Only a terminal gets each line as it is written,
   // a pipe would sit on them until its buffer is full.
   //
   std::fflush(stdout);
}

//
// Looks up config.hosts using 'lookup', with at most config.concurrency of them in flight at any
// time, and reports each one the moment its answer is in -- or, with --ordered, in the order they
// were given.
//
awaitable<void> resolveAll(const Config& config, LookupFunction lookup)
{
   const auto& hosts = config.hosts;
   const auto width = std::ranges::max(hosts, {}, &std::string::size).size();

   const auto report = [&](const std::string& host, const Lookup& result) {
      if (config.file.empty())
         reportInFull(host, result);
      else
         reportCompact(host, result, width);
   };

   size_t next = 0; // the name the next free worker takes

   //
   // Only with --ordered: the answers that are in but have to wait, because a name given before
   // theirs has none yet. One slow name holds up every row behind it this way, though none of the
   // lookups.
   //
   std::vector<std::optional<Lookup>> waiting(config.ordered ? hosts.size() : 0);
   size_t reported = 0; // the first name that has not been printed yet

   //
   // One worker is one lookup in flight: it takes the next name, waits for the answer, prints it
   // and comes back for more. All of them run on the one thread and only ever switch at a
   // co_await, so they can share the above without further ado.
   //
   auto worker = [&]() -> awaitable<void> {
      co_await this_coro::throw_if_cancelled(false);
      const auto state = co_await this_coro::cancellation_state;

      //
      // The cancellation state is what to ask, not the lookup's error: an answer that was already
      // on its way when the signal came is a good one, and going by that alone the worker would
      // start on the next name with nobody left to stop it.
      //
      while (next < hosts.size() && state.cancelled() == cancellation_type::none)
      {
         const auto index = next++;
         auto result = co_await lookup(hosts[index]);
         if (state.cancelled() != cancellation_type::none)
            co_return;

         if (!config.ordered)
         {
            report(hosts[index], result);
            continue;
         }

         waiting[index] = std::move(result);
         for (; reported < hosts.size() && waiting[reported]; ++reported)
         {
            report(hosts[reported], *waiting[reported]);
            waiting[reported].reset();
         }
      }
   };

   const auto executor = co_await this_coro::executor;
   const auto count = std::min<size_t>(config.concurrency, hosts.size());
   spdlog::debug("resolving {} name(s), {} at a time", hosts.size(), count);

   std::vector<decltype(co_spawn(executor, worker(), deferred))> workers;
   for (size_t i = 0; i < count; ++i)
      workers.push_back(co_spawn(executor, worker(), deferred));

   auto [order, exceptions] = co_await experimental::make_parallel_group(std::move(workers))
                                 .async_wait(experimental::wait_for_all(), use_awaitable);

   for (const auto& exception : exceptions)
      if (exception)
         std::rethrow_exception(exception);
}

// --------------------------------------------------------------------------------------------------

awaitable<Lookup> lookupWithAsio(ip::tcp::resolver& resolver, const Config& config,
                                 const std::string& host)
{
   spdlog::debug("resolving {}:{}...", host, config.service);
   auto [ec, results] = co_await resolver.async_resolve(host, config.service, as_tuple);

   Lookup lookup{.ec = ec};
   for (const auto& result : results)
      lookup.endpoints.push_back(result.endpoint());

   co_return lookup;
}

awaitable<void> resolveWithAsio(const Config& config)
{
   co_await this_coro::throw_if_cancelled(false);

   if (!config.servers.empty())
      spdlog::warn("--server is not supported by the ASIO resolver, ignoring it");

   //
   // getaddrinfo() answers with addresses and nothing else, so this path is addresses only. It
   // also blocks, and ASIO has a single thread to call it on unless configured otherwise, so
   // however many lookups are handed over at once, they are still answered one after the other.
   //
   spdlog::debug("the ASIO resolver cannot look up HTTPS records");
   if (config.concurrency > 1)
      spdlog::debug("the ASIO resolver works through its lookups one at a time");

   ip::tcp::resolver resolver(co_await this_coro::executor);
   co_await resolveAll(
      config, [&](const std::string& host) { return lookupWithAsio(resolver, config, host); });
}

//
// Everything asked about one name. The addresses and the HTTPS record (RFC 9460) are two separate
// questions, so they go out together rather than one after the other -- c-ares has no trouble
// keeping several queries in flight on a channel, and with --dot they share the connection as
// well as the round trip.
//
awaitable<Lookup> lookupWithAres(AresResolver& resolver, const Config& config,
                                 const std::string& host)
{
   spdlog::debug("resolving {}:{}...", host, config.service);

   //
   // An address is no name to hang a record on, so there is only one question to ask -- and
   // c-ares answers that one without a query. A list of URLs has such hosts in it sooner or later.
   //
   if (boost::system::error_code notAnAddress; ip::make_address(host, notAnAddress), !notAnAddress)
   {
      auto [ec, endpoints] = co_await resolver.async_resolve(host, config.service, as_tuple);
      co_return Lookup{.ec = ec, .endpoints = std::move(endpoints)};
   }

   auto [order, ec, endpoints, httpsEc, records] =
      co_await experimental::make_parallel_group(
         resolver.async_resolve(host, config.service, deferred),
         resolver.async_lookupHttps(host, deferred))
         .async_wait(experimental::wait_for_all(), use_awaitable);

   //
   // A name that cannot be encoded as a DNS name at all is no question for any record type, and
   // c-ares turns that one down locally, without a query. But where ares_getaddrinfo() says
   // ARES_EBADNAME, ares_search_dnsrec() says ARES_ENOMEM (c-ares 1.34.5), so the address error
   // is the one to believe and repeating it as a memory problem would only mislead.
   //
   if (ec == make_ares_error(ARES_EBADNAME))
      httpsEc = {};

   //
   // The records of a set come in no particular order, and not in the same one twice. By priority
   // is the order a client goes through them in, and it puts the alias form first.
   //
   std::ranges::stable_sort(records, {}, &HttpsRecord::priority);

   co_return Lookup{ec, std::move(endpoints), httpsEc, std::move(records)};
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

   co_await resolveAll(
      config, [&](const std::string& host) { return lookupWithAres(resolver, config, host); });
}

int main(int argc, char* argv[])
{
   auto config = parseConfig(argc, argv);
   if (!config)
      return config.error();

   //
   // spdlog makes up its own mind about the terminal, so it has to be told as well.
   //
   useColor = config->color || isatty(STDOUT_FILENO);
   if (config->color)
      spdlog::set_default_logger(spdlog::stdout_color_mt("resolve", spdlog::color_mode::always));

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
