// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#include "server_list.hpp"

#include <algorithm>
#include <format>
#include <memory>

namespace cares_async::detail
{

namespace
{

std::string_view trim(std::string_view text)
{
   const auto first = text.find_first_not_of(" \t");
   if (first == std::string_view::npos)
      return {};

   return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

//
// Whether a c-ares server entry carries an explicit port. "[::1]:853" has it behind the bracket,
// and "192.0.2.1:853" is the only unbracketed form that can have one -- a bare IPv6 address has
// several colons and no port at all.
//
bool has_port(std::string_view server)
{
   const auto colon = server.rfind(':');
   if (colon == std::string_view::npos || colon + 1 == server.size())
      return false;

   const auto bracket = server.rfind(']');
   if (bracket == std::string_view::npos ? server.find(':') != colon : colon < bracket)
      return false;

   return std::ranges::all_of(server.substr(colon + 1),
                              [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

// --------------------------------------------------------------------------------------------------

std::string with_port(std::string_view servers, unsigned port, bool force)
{
   std::string result;

   for (size_t begin = 0; begin <= servers.size();)
   {
      const auto comma = servers.find(',', begin);
      auto server = trim(servers.substr(begin, comma - begin));
      begin = comma == std::string_view::npos ? servers.size() + 1 : comma + 1;

      if (server.empty())
         continue;

      if (!result.empty())
         result += ',';

      const bool ported = has_port(server);
      if (server.find("://") != std::string_view::npos || (ported && !force))
      {
         result += server;
         continue;
      }

      if (ported)
         server = server.substr(0, server.rfind(':'));

      //
      // A bare IPv6 address only needs its brackets once there is a port behind it.
      //
      if (!server.starts_with('[') && server.find(':') != std::string_view::npos)
         result += std::format("[{}]:{}", server, port);
      else
         result += std::format("{}:{}", server, port);
   }

   return result;
}

ares_status_t set_port(ares_channel_t* channel, unsigned port)
{
   const std::unique_ptr<char, decltype(&ares_free_string)> current(ares_get_servers_csv(channel),
                                                                    &ares_free_string);
   if (!current)
      return ARES_ENOMEM;

   const auto moved = with_port(current.get(), port, true);
   if (moved.empty())
      return ARES_SUCCESS; // no servers, so none to move

   return static_cast<ares_status_t>(ares_set_servers_csv(channel, moved.c_str()));
}

} // namespace cares_async::detail
