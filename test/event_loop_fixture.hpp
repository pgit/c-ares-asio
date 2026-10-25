// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The event loop a test runs on, which is the one of the runtime the library was built on. See
// asio/event_loop_fixture.hpp for what a fixture gives a test, and corosio/event_loop_fixture.hpp
// for the same thing on the other runtime.
//
#pragma once

#include <cares_asio/runtime.hpp>

#if CARES_ASIO_COROSIO
#include "corosio/event_loop_fixture.hpp"
#else
#include "asio/event_loop_fixture.hpp"
#endif
