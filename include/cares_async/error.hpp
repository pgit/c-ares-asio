// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cares_async/export.hpp>
#include <cares_async/runtime.hpp>

namespace cares_async
{

/** The error category of the c-ares status codes.

    A lookup that fails says why with one of the `ARES_E...` status codes of
    `<ares.h>`, and this is the category they travel in as an
    @ref error_code. The messages are those of `ares_strerror()`.

    @return The one object of the category, whose name is "c-ares".

    @see make_ares_error
 */
CARES_ASYNC_EXPORT const error_category& ares_category() noexcept;

/** Makes an error code out of a c-ares status code.

    This is what to compare the outcome of an operation against:

    @code
    if (ec == cares_async::make_ares_error(ARES_ENODATA))
       ; // the name is there, a record of the type asked for is not
    @endcode

    `ARES_SUCCESS` is 0, so the code of an operation that went well tests false
    like any other.

    @param status One of the `ARES_E...` status codes, or `ARES_SUCCESS`.

    @return `status` as an error code in @ref ares_category.
 */
CARES_ASYNC_EXPORT error_code make_ares_error(int status) noexcept;

} // namespace cares_async
