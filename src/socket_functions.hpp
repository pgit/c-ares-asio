// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The socket layer c-ares calls, in the plain case: what its own one does, except that it refuses
// TCP Fast Open.
//
// Fast open makes a socket writable before its connection is established -- the point of it is
// that the first write goes out with the SYN. c-ares asks to be told when a socket of its becomes
// writable and takes that for the connect having finished, which is what
// ares_set_socket_functions_ex() documents an implementation to do; an event loop that answers
// the question "is this socket writable" cannot tell the two apart. Told so anyway, c-ares writes
// the query a second time and drops the connection, which costs a lookup every retry it has.
//
// Refusing the option with ENOSYS is what c-ares takes for "this one does not do fast open",
// which is also what src/tls_transport.cpp does for the TLS transport -- the handshake there
// needs a connection before the first write just as much.
//
// https://c-ares.org/docs/ares_set_socket_functions_ex.html
//
#pragma once

#include <ares.h>

namespace cares_async::detail
{

//
// Hands our socket functions to the channel, which must not have opened any sockets yet. Not for
// a channel with a TLS transport on it: that one has socket functions of its own.
//
ares_status_t install_socket_functions(ares_channel_t* channel);

//
// The one socket option that both the plain and the TLS socket functions answer the same way,
// which is the one place TCP Fast Open is turned down. Returns what c-ares expects of
// asetsockopt(): 0, or -1 with errno set.
//
int set_socket_option(ares_socket_t fd, ares_socket_opt_t opt, const void* value,
                      ares_socklen_t size);

} // namespace cares_async::detail
