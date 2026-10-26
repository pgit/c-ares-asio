// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// with_port() is not part of the interface: it is what resolver::set_servers() runs the list
// through for DNS over TLS, where nothing listens on the port that c-ares defaults to. The
// sources are compiled into the test to get at it.
//
#include "server_list.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using cares_async::detail::set_port;
using cares_async::detail::with_port;

TEST(ServerList, AddsThePortWhereThereIsNone)
{
   EXPECT_EQ(with_port("192.0.2.1", 853, false), "192.0.2.1:853");
   EXPECT_EQ(with_port("192.0.2.1,192.0.2.2", 853, false), "192.0.2.1:853,192.0.2.2:853");
}

TEST(ServerList, KeepsAPortThatWasSpelledOut)
{
   EXPECT_EQ(with_port("192.0.2.1:5353", 853, false), "192.0.2.1:5353");
   EXPECT_EQ(with_port("192.0.2.1:5353,192.0.2.2", 853, false), "192.0.2.1:5353,192.0.2.2:853");
}

TEST(ServerList, ReplacesThePortWhenForced)
{
   EXPECT_EQ(with_port("192.0.2.1:53", 853, true), "192.0.2.1:853");
   EXPECT_EQ(with_port("192.0.2.1", 853, true), "192.0.2.1:853");
}

TEST(ServerList, PutsAnIPv6AddressInBrackets)
{
   //
   // The colons of the address are not the one in front of a port, which is what the brackets
   // are for.
   //
   EXPECT_EQ(with_port("2001:db8::1", 853, false), "[2001:db8::1]:853");
   EXPECT_EQ(with_port("::1", 853, false), "[::1]:853");
   EXPECT_EQ(with_port("[2001:db8::1]", 853, false), "[2001:db8::1]:853");
}

TEST(ServerList, KeepsThePortOfAnIPv6Address)
{
   EXPECT_EQ(with_port("[2001:db8::1]:5353", 853, false), "[2001:db8::1]:5353");
   EXPECT_EQ(with_port("[2001:db8::1]:53", 853, true), "[2001:db8::1]:853");
}

TEST(ServerList, KeepsTheScopeOfALinkLocalAddress)
{
   EXPECT_EQ(with_port("fe80::1%eth0", 853, false), "[fe80::1%eth0]:853");
   EXPECT_EQ(with_port("[fe80::1%eth0]:53", 853, true), "[fe80::1%eth0]:853");
}

TEST(ServerList, IgnoresBlanksAndEmptyEntries)
{
   EXPECT_EQ(with_port(" 192.0.2.1 ,\t192.0.2.2:53 ", 853, false), "192.0.2.1:853,192.0.2.2:53");
   EXPECT_EQ(with_port("192.0.2.1,,192.0.2.2,", 853, false), "192.0.2.1:853,192.0.2.2:853");
   EXPECT_EQ(with_port("", 853, false), "");
   EXPECT_EQ(with_port(" , ", 853, true), "");
}

TEST(ServerList, LeavesUrisAlone)
{
   EXPECT_EQ(with_port("dns://192.0.2.1", 853, true), "dns://192.0.2.1");
   EXPECT_EQ(with_port("dns://192.0.2.1:53,192.0.2.2", 853, false),
             "dns://192.0.2.1:53,192.0.2.2:853");
}

// --------------------------------------------------------------------------------------------------

namespace
{

std::string servers_of(ares_channel_t* channel)
{
   const std::unique_ptr<char, decltype(&ares_free_string)> csv(ares_get_servers_csv(channel),
                                                                &ares_free_string);
   return csv ? csv.get() : "";
}

} // namespace

TEST(ServerList, MovesTheServersOfAChannelToAnotherPort)
{
   //
   // What a resolver does to the servers from /etc/resolv.conf when it is made for DNS over
   // TLS -- with a channel of the test's own here, so that it is the test that says which
   // servers there are to move.
   //
   ares_channel_t* created = nullptr;
   ASSERT_EQ(ares_init_options(&created, nullptr, 0), ARES_SUCCESS);
   const std::unique_ptr<ares_channel_t, decltype(&ares_destroy)> channel(created, &ares_destroy);

   ASSERT_EQ(ares_set_servers_csv(channel.get(), "192.0.2.1,192.0.2.2:5353,[2001:db8::1]:53"),
             ARES_SUCCESS);
   ASSERT_EQ(servers_of(channel.get()), "192.0.2.1:53,192.0.2.2:5353,[2001:db8::1]:53");

   EXPECT_EQ(set_port(channel.get(), 853), ARES_SUCCESS);
   EXPECT_EQ(servers_of(channel.get()), "192.0.2.1:853,192.0.2.2:853,[2001:db8::1]:853");
}
