#pragma once

#include <boost/system/error_category.hpp>
#include <boost/system/error_code.hpp>

namespace cares_asio
{

//
// Error category wrapping the ARES_* status codes, so that they can travel as an error_code.
//
const boost::system::error_category& ares_category();
boost::system::error_code make_ares_error(int status);

} // namespace cares_asio
