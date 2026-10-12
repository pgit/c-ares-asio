//
// The server lists c-ares takes, as ares_set_servers_csv() describes them: comma separated
// addresses, each with or without a port ("192.0.2.1", "192.0.2.1:5353", "[2001:db8::1]:5353").
//
#pragma once

#include <string>
#include <string_view>

namespace cares_asio::detail
{

//
// Puts 'port' on every entry of a server list that has none, or on all of them when 'force' is
// set. Entries in the dns:// URI form are left alone, c-ares knows what to do with those.
//
std::string with_port(std::string_view servers, unsigned port, bool force);

} // namespace cares_asio::detail
