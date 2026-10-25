// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The event loop itself, as the implementation reaches it -- what the public runtime layer
// (cares_asio/runtime.hpp) does not need to show anybody:
//
//    wait_type, Descriptor   -- readiness on a descriptor of c-ares', which it keeps owning
//    Timer                   -- the one timer that ares_timeout() asks for
//    make_endpoint()         -- an Endpoint of the runtime's own, from a sockaddr of c-ares'
//
// asio/event_loop.hpp has the documentation of each, and corosio/event_loop.hpp the same
// definitions in the same order.
//
#pragma once

#include <cares_asio/runtime.hpp>

#if CARES_ASIO_COROSIO
#include "corosio/event_loop.hpp"
#else
#include "asio/event_loop.hpp"
#endif
