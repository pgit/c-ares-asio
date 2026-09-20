#pragma once

#include <boost/asio/io_context.hpp>

// =================================================================================================

//
// Similar to io_context::run(), and returns the same count of handlers run, but prints a
// horizontal ruler per completion handler, highlighting the ones that took more than 10ms. The
// time measured spans the handler's own execution as well as the idle wait before it.
//
// Under NDEBUG this is plain io_context::run() and prints nothing.
//
size_t run(boost::asio::io_context& context);

// =================================================================================================
