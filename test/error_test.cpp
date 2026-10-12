// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#include <cares_asio/error.hpp>

#include <boost/system/errc.hpp>
#include <boost/system/system_error.hpp>

#include <ares.h>

#include <gtest/gtest.h>

using cares_asio::ares_category;
using cares_asio::make_ares_error;

TEST(Error, CategoryHasAName) { EXPECT_STREQ(ares_category().name(), "c-ares"); }

TEST(Error, CategoryIsOneAndTheSame)
{
   EXPECT_EQ(&ares_category(), &ares_category());
   EXPECT_EQ(make_ares_error(ARES_ENODATA).category(), ares_category());
}

TEST(Error, SuccessIsNoError)
{
   EXPECT_FALSE(make_ares_error(ARES_SUCCESS));
   EXPECT_TRUE(make_ares_error(ARES_ENOTFOUND));
}

TEST(Error, KeepsTheStatusCode)
{
   EXPECT_EQ(make_ares_error(ARES_ETIMEOUT).value(), ARES_ETIMEOUT);
   EXPECT_EQ(make_ares_error(ARES_ENODATA), make_ares_error(ARES_ENODATA));
   EXPECT_NE(make_ares_error(ARES_ENODATA), make_ares_error(ARES_ENOTFOUND));
}

TEST(Error, MessageIsTheOneOfCares)
{
   EXPECT_EQ(make_ares_error(ARES_ENOTFOUND).message(), ares_strerror(ARES_ENOTFOUND));
   EXPECT_FALSE(make_ares_error(ARES_ETIMEOUT).message().empty());
}

TEST(Error, IsNoSystemError)
{
   //
   // The values overlap with errno -- ARES_ENODATA is 1, and so is EPERM -- which is what the
   // category is there to tell apart.
   //
   EXPECT_NE(make_ares_error(ARES_ENODATA),
             boost::system::error_code(ARES_ENODATA, boost::system::system_category()));
   EXPECT_NE(make_ares_error(ARES_ENODATA), boost::system::errc::operation_not_permitted);
}

TEST(Error, TravelsInAnException)
{
   const boost::system::system_error error(make_ares_error(ARES_ESERVFAIL), "lookup");
   EXPECT_EQ(error.code(), make_ares_error(ARES_ESERVFAIL));
}
