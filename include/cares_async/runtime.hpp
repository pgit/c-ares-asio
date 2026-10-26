// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The I/O runtime the library is built on, and the vocabulary its interface is written in:
// Boost.Asio with completion tokens (CARES_ASYNC_ASIO=1), or capy/corosio with coroutines only
// (CARES_ASYNC_COROSIO=1). A build tree is one or the other, see doc/corosio.md.
//
//    Task<T>, Executor, error_code, error_category, system_error  -- what the runtime calls things
//    IpAddress, Endpoint                                          -- the runtime's own
//    errors::canceled                                             -- part of the API contract
//    detail::Completion<Signature>, detail::initiate()            -- parked operations
//    detail::complete_later(), detail::on_cancel(), detail::clear_on_cancel()
//
// asio/runtime.hpp has the documentation of each. The implementation reaches the event loop
// itself through src/event_loop.hpp, which nothing public includes.
//
#pragma once

#include <cares_async/config.hpp>

#if CARES_ASYNC_ASIO == CARES_ASYNC_COROSIO
#error "exactly one of CARES_ASYNC_ASIO and CARES_ASYNC_COROSIO has to be 1"
#endif

#if CARES_ASYNC_COROSIO
#include <cares_async/corosio/runtime.hpp>
#else
#include <cares_async/asio/runtime.hpp>
#endif
