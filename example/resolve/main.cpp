// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// Resolves the host names given on the command line, or those of the URLs listed in a file, using
// c-ares driven by the event loop of the runtime the library was built on. With --native, the
// resolver of that runtime itself (a getaddrinfo() call on a thread of its own) is used instead,
// which is the behaviour to compare against.
//
#include "runtime.hpp"
#include "utils.hpp"

#include <boost/program_options.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <c_ares_async/resolver.hpp>

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
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <unistd.h>

namespace po = boost::program_options;

using c_ares_async::error_code;
using c_ares_async::Task;

struct Config
{
   bool verbose = false;
   bool native = false;
   bool ordered = false;
   bool color = false;
   int concurrency = 10;
   std::string service = "443";
   std::string servers;
   std::string file; // where the URLs came from, which also asks for the one-line-per-host listing
   std::vector<std::string> hosts;
   c_ares_async::tls_options tls;
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

//
// The entries of the comma separated --server list.
//
std::vector<std::string_view> serversOf(std::string_view list)
{
   std::vector<std::string_view> servers;
   for (const auto part : std::views::split(list, ','))
   {
      const std::string_view server(part);
      if (const auto first = server.find_first_not_of(" \t"); first != std::string_view::npos)
         servers.push_back(server.substr(first, server.find_last_not_of(" \t") - first + 1));
   }

   return servers;
}

//
// A --server entry that is a host name, and the port behind it -- colon included -- if there is
// one.
//
struct ServerName
{
   std::string_view name;
   std::string_view port;
};

//
// Tells the names on --server from the addresses, which are all c-ares takes for a server:
// "192.0.2.1", "192.0.2.1:5353", "[::1]:5353", "fe80::1%eth0" or a dns:// URI. Whatever is none of
// these is a name, with or without a port.
//
std::optional<ServerName> serverName(std::string_view server)
{
   if (server.contains("://") || server.contains('[') || server.contains('%'))
      return std::nullopt;

   const auto colon = server.find(':');
   if (colon != server.rfind(':'))
      return std::nullopt; // a bare IPv6 address, which has several colons and no port

   const auto name = server.substr(0, colon);
   if (name.empty() || app::make_address(name))
      return std::nullopt;

   return ServerName{name, colon == std::string_view::npos ? "" : server.substr(colon)};
}

//
// The host names among the servers, each of them once.
//
std::vector<std::string> serverNames(std::string_view servers)
{
   std::vector<std::string> names;
   for (const auto server : serversOf(servers))
   {
      const auto named = serverName(server);
      if (named && !std::ranges::contains(names, named->name))
         names.emplace_back(named->name);
   }

   return names;
}

std::expected<Config, int> parseConfig(int argc, char* argv[])
{
   Config config;

   po::options_description desc("Allowed options");
   auto opts = desc.add_options();
   opts("help,h", "produce help message");
   opts("verbose,v", po::bool_switch(&config.verbose), "enable verbose logging");
   const auto native = std::format("use {} instead of c-ares", app::native_resolver::name);
   opts("native", po::bool_switch(&config.native), native.c_str());
   opts("service,s", po::value(&config.service)->default_value(config.service),
        "service name or port number to resolve for");
   opts("server,n", po::value(&config.servers),
        "DNS server(s) to query instead of the ones from /etc/resolv.conf, comma separated "
        "addresses or host names (c-ares only) -- try 192.0.2.1 for a black hole");
   opts("dot,t", po::bool_switch(&config.tls.enabled),
        "use DNS over TLS (RFC 7858) -- forces TCP and moves the servers to port 853 (c-ares "
        "only)");
   opts("tls-hostname", po::value(&config.tls.hostname),
        "server name to use for SNI and to verify the certificate against, e.g. "
        "one.one.one.one -- a host name given as --server is used unless told otherwise");
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
      if (config.native)
      {
         std::cerr << std::format("--dot is not supported by {}", app::native_resolver::name)
                   << std::endl
                   << desc;
         return std::unexpected(EXIT_FAILURE);
      }

      //
      // A server given by name has said what its certificate is for already, and saying it a
      // second time is a chore. An address is no name to check a certificate against, though, and
      // of several names none is the one for all of the servers -- rather than quietly talk to
      // whoever answers, say so.
      //
      if (config.tls.hostname.empty())
      {
         const auto names = serverNames(config.servers);
         if (names.size() == 1)
         {
            config.tls.hostname = names.front();
         }
         else if (config.tls.verify)
         {
            std::cerr << (names.empty() ? "--dot needs a name to verify the server certificate "
                                          "against: a host name as --server, or --tls-hostname"
                                        : "--server names several hosts, so --dot needs "
                                          "--tls-hostname to verify their certificates against")
                      << ", or --tls-no-verify to skip the check" << std::endl
                      << desc;
            return std::unexpected(EXIT_FAILURE);
         }
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
// The library does not log by itself, it only says what there is to log.
//
void logToSpdlog(c_ares_async::log_level level, std::string_view message)
{
   switch (level)
   {
   case c_ares_async::log_level::debug:
      return spdlog::debug("{}", message);
   case c_ares_async::log_level::info:
      return spdlog::info("{}", message);
   case c_ares_async::log_level::warning:
      return spdlog::warn("{}", message);
   case c_ares_async::log_level::error:
      return spdlog::error("{}", message);
   }
}

//
// What came back for one name. The resolver of the runtime has no way to ask for the HTTPS
// record, so that half stays empty there.
//
struct Lookup
{
   error_code ec;
   c_ares_async::resolver::results_type endpoints;
   error_code httpsEc;
   c_ares_async::resolver::https_results_type records;
};

using LookupFunction = std::function<Task<Lookup>(const std::string& host)>;

//
// A name without an HTTPS record is the common case rather than a problem.
//
bool isNoRecord(const error_code& ec)
{
   return ec == c_ares_async::make_ares_error(ARES_ENODATA) ||
          ec == c_ares_async::make_ares_error(ARES_ENOTFOUND);
}

//
// Addresses come in the colours "ip -color address" gives them: IPv4 in magenta, IPv6 in blue.
// Host names are yellow. Only a terminal gets any of it unless --color says otherwise, which is
// where spdlog colours the log level as well.
//
enum class Color
{
   name = 33,
   v6 = 34,
   v4 = 35
};

bool useColor = false;

std::string colored(std::string_view text, Color color)
{
   return useColor ? std::format("\x1b[{}m{}\x1b[0m", std::to_underlying(color), text)
                   : std::string(text);
}

//
// Padded to 'width' before the colour goes on, as the escape sequences would count towards it.
//
std::string toText(const c_ares_async::IpAddress& address, size_t width = 0)
{
   return colored(std::format("{:{}}", address.to_string(), width),
                  address.is_v4() ? Color::v4 : Color::v6);
}

std::string toText(const c_ares_async::https_record& record)
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
Task<void> resolveAll(const Config& config, LookupFunction lookup)
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
   auto worker = [&]() -> Task<void> {
      co_await app::observe_cancellation();

      //
      // Whether the worker was cancelled is what to ask, not the lookup's error: an answer that
      // was already on its way when the signal came is a good one, and going by that alone the
      // worker would start on the next name with nobody left to stop it.
      //
      while (next < hosts.size() && !co_await app::cancelled())
      {
         const auto index = next++;
         auto result = co_await lookup(hosts[index]);
         if (co_await app::cancelled())
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

   const auto count = std::min<size_t>(config.concurrency, hosts.size());
   spdlog::debug("resolving {} name(s), {} at a time", hosts.size(), count);

   std::vector<Task<void>> workers;
   workers.reserve(count);
   for (size_t i = 0; i < count; ++i)
      workers.push_back(worker());

   co_await app::when_all(std::move(workers));
}

// --------------------------------------------------------------------------------------------------

Task<Lookup> lookupWithNative(app::native_resolver& resolver, const Config& config,
                              const std::string& host)
{
   spdlog::debug("resolving {}:{}...", host, config.service);
   auto [ec, endpoints] = co_await resolver.resolve(host, config.service);

   co_return Lookup{.ec = ec, .endpoints = std::move(endpoints)};
}

Task<void> resolveWithNative(const c_ares_async::Executor& executor, const Config& config)
{
   co_await app::observe_cancellation();

   if (!config.servers.empty())
      spdlog::warn("--server is not supported by {}, ignoring it", app::native_resolver::name);

   //
   // getaddrinfo() answers with addresses and nothing else, so this path is addresses only -- and
   // it blocks, so what the runtime has to call it on decides how many lookups it really has in
   // flight at a time.
   //
   spdlog::debug("{} cannot look up HTTPS records", app::native_resolver::name);
   if (config.concurrency > 1)
      spdlog::debug("{}", app::native_resolver::concurrency_note);

   app::native_resolver resolver(executor);
   co_await resolveAll(
      config, [&](const std::string& host) { return lookupWithNative(resolver, config, host); });
}

//
// Everything asked about one name. The addresses and the HTTPS record (RFC 9460) are two separate
// questions, so they go out together rather than one after the other -- c-ares has no trouble
// keeping several queries in flight on a channel, and with --dot they share the connection as
// well as the round trip.
//
Task<Lookup> lookupWithAres(c_ares_async::resolver& resolver, const Config& config,
                            const std::string& host)
{
   spdlog::debug("resolving {}:{}...", host, config.service);

   //
   // An address is no name to hang a record on, so there is only one question to ask -- and
   // c-ares answers that one without a query. A list of URLs has such hosts in it sooner or later.
   //
   if (app::make_address(host))
   {
      auto [ec, endpoints] = co_await resolver.resolve(host, config.service);
      co_return Lookup{.ec = ec, .endpoints = std::move(endpoints)};
   }

   //
   // Both questions go out together rather than one after the other: c-ares has no trouble
   // keeping several queries in flight on a channel, and with --dot they share the connection as
   // well as the round trip.
   //
   // Each of them leaves its own answer behind, which is also what keeps them apart: a composite
   // operation that gives up on the first error would have the missing HTTPS record of a name --
   // the common case -- cut the addresses short.
   //
   Lookup lookup;

   std::vector<Task<void>> questions;
   questions.push_back([](c_ares_async::resolver& resolver, std::string host, std::string service,
                          Lookup& lookup) -> Task<void> {
      std::tie(lookup.ec, lookup.endpoints) =
         co_await resolver.resolve(std::move(host), std::move(service));
   }(resolver, host, config.service, lookup));

   questions.push_back([](c_ares_async::resolver& resolver, std::string host,
                          Lookup& lookup) -> Task<void> {
      std::tie(lookup.httpsEc, lookup.records) = co_await resolver.resolve_https(std::move(host));
   }(resolver, host, lookup));

   co_await app::when_all(std::move(questions));

   //
   // A name that cannot be encoded as a DNS name at all is no question for any record type, and
   // c-ares turns that one down locally, without a query. But where ares_getaddrinfo() says
   // ARES_EBADNAME, ares_search_dnsrec() says ARES_ENOMEM (c-ares 1.34.5), so the address error
   // is the one to believe and repeating it as a memory problem would only mislead.
   //
   if (lookup.ec == c_ares_async::make_ares_error(ARES_EBADNAME))
      lookup.httpsEc = {};

   //
   // The records of a set come in no particular order, and not in the same one twice. By priority
   // is the order a client goes through them in, and it puts the alias form first.
   //
   std::ranges::stable_sort(lookup.records, {}, &c_ares_async::https_record::priority);

   co_return lookup;
}

//
// c-ares takes addresses for its servers and nothing else, so a name on --server has to be looked
// up before there is anybody to ask -- the regular way, of the servers from /etc/resolv.conf and
// in the clear, whatever the queries proper are meant to go out as. Returns nothing if a name
// cannot be looked up, having said why.
//
Task<std::optional<std::string>> lookupServers(const c_ares_async::Executor& executor,
                                               std::string_view servers)
{
   //
   // Only if there is a name to look up at all. By pointer rather than in a std::optional, which
   // one of the runtimes does not take a resolver in -- see doc/corosio.rst.
   //
   std::unique_ptr<c_ares_async::resolver> resolver;
   std::string addresses;

   for (const auto server : serversOf(servers))
   {
      const auto add = [&](std::string_view address) {
         addresses += addresses.empty() ? "" : ",";
         addresses += address;
      };

      const auto named = serverName(server);
      if (!named)
      {
         add(server);
         continue;
      }

      if (!resolver)
      {
         resolver = std::make_unique<c_ares_async::resolver>(executor);
         resolver->set_logger(&logToSpdlog);
      }

      spdlog::debug("looking up DNS server {}...", named->name);
      const auto [ec, endpoints] = co_await resolver->resolve(std::string(named->name), "");
      if (ec || endpoints.empty())
      {
         if (ec != c_ares_async::errors::canceled)
            spdlog::error("--server {}: {}", named->name,
                          ec ? ec.message()
                             : c_ares_async::make_ares_error(ARES_ENODATA).message());
         co_return std::nullopt;
      }

      //
      // Only the first address the name has becomes a server -- the one c-ares sorted to the
      // front as the best to reach from here. Taking them all looks like failover for free, but
      // c-ares gives every query its tries once per server, with the timeout doubling each time
      // round: a name nobody has an answer for is given up on after 3 attempts with one server
      // and after 12 with the four a public resolver's name comes to, and a long list of names
      // then spends most of its time waiting for those.
      //
      // An IPv6 address only needs its brackets once there is a port behind it.
      //
      const auto first = endpoints.front().address();
      add(named->port.empty() ? first.to_string()
          : first.is_v6()     ? std::format("[{}]{}", first.to_string(), named->port)
                              : first.to_string() + std::string(named->port));
   }

   co_return addresses;
}

Task<void> resolveWithAres(const c_ares_async::Executor& executor, const Config& config)
{
   co_await app::observe_cancellation();

   const auto servers = co_await lookupServers(executor, config.servers);
   if (!servers)
      co_return;

   c_ares_async::resolver resolver(executor, config.tls);
   resolver.set_logger(&logToSpdlog);
   if (auto ec = resolver.set_servers(*servers))
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

   app::Context context;
   const auto executor = app::executor_of(context);

   //
   // Ctrl-C cancels the lookups instead of stopping the event loop, so that everything gets a
   // chance to unwind -- the pending request belongs to c-ares, the loop knows nothing about it.
   //
   app::run_main(context,
                 config->native ? resolveWithNative(executor, *config)
                                : resolveWithAres(executor, *config),
                 config->verbose);

   return EXIT_SUCCESS;
}
