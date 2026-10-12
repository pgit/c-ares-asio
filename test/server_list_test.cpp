//
// with_port() is not part of the interface: it is what resolver::set_servers() runs the list
// through for DNS over TLS, where nothing listens on the port that c-ares defaults to. The
// sources are compiled into the test to get at it.
//
#include "server_list.hpp"

#include <gtest/gtest.h>

using cares_asio::detail::with_port;

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
