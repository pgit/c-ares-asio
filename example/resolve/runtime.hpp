// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// What this program needs of its I/O runtime beyond what the library's own runtime layer
// (cares_asio/runtime.hpp) gives it:
//
//    Context                 -- the event loop to run everything on
//    run_main()              -- spawn the lookups, cancel them on a signal, run the loop
//    when_all()              -- the workers of --concurrency, all of them at once
//    observe_cancellation(), cancelled()
//    native_resolver         -- the resolver of the runtime itself, which --native compares against
//    make_address()          -- a host name that is an address already
//
// asio/runtime.hpp has the documentation of each, and corosio/runtime.hpp the same definitions in
// the same order.
//
#pragma once

#include <cares_asio/runtime.hpp>

#if CARES_ASIO_COROSIO
#include "corosio/runtime.hpp"
#else
#include "asio/runtime.hpp"
#endif
