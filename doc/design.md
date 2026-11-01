# How the integration works

c-ares is told about nothing but its own sockets, and is driven entirely from the event loop of
the application. [`src/resolver.cpp`](../src/resolver.cpp) holds all of it:

* the channel is created with `ARES_OPT_SOCK_STATE_CB`, so c-ares reports every socket it opens,
  closes, or changes its read/write interest on -- no c-ares event thread is involved;
* each such socket gets a non-owning `detail::Descriptor` (`src/event_loop.hpp`), which waits for
  readiness on it and is detached again before c-ares closes the socket. On Boost.Asio that is a
  `posix::stream_descriptor` and `async_wait()`; the other runtime is in [corosio.md](corosio.md);
* readiness is fed back with [`ares_process_fds()`](https://c-ares.org/docs/ares_process_fds.html).
  Since the socket state callback only fires on *changes*, the waits are re-armed after every call;
* `ares_timeout()` drives a `detail::Timer`, whose expiry calls `ares_process_fds()` with no
  events;
* the socket functions of the channel are our own ([`src/socket_functions.hpp`](../src/socket_functions.hpp)),
  for one reason: they turn TCP fast open down. Fast open makes a socket writable before its
  connection is up, and the write event is what c-ares takes for the connect having finished --
  told otherwise, it writes the query a second time and drops the connection. DNS over TLS
  installs socket functions of its own, which refuse it just the same.

Queries come in two spellings, and are the same operation behind both: `resolve()` and
`resolve_https()`, coroutines that yield the error and the result as a tuple, which is what either
runtime has; and, on Boost.Asio, `async_resolve()` and `async_resolve_https()` through
`async_initiate`, which take `use_awaitable`, `as_tuple`, `deferred`, callbacks and futures alike.

## Completion

c-ares calls back from inside `ares_getaddrinfo()` or `ares_process_fds()`, and for a name it can
answer without a query -- an address, `localhost`, an entry of `/etc/hosts` -- that is before the
initiating function has even returned. A completion handler must never be invoked from in there, so
it is posted: to the executor of the resolver, from where it is passed on to the executor the
handler is associated with, if it has one.

The same holds in the other direction, and c-ares does not survive it being broken: a readiness
wait must not call `ares_process_fds()` from inside one that is already running. It would be,
otherwise, since the waits are armed from the socket state callback, which c-ares calls from in
there. On Boost.Asio a wait only ever completes from the event loop, so the question does not come
up; the other runtime completes one whose readiness holds already right where it is started, and
posts it for that reason -- see [corosio.md](corosio.md).

## Cancellation

Cancellation works, but only bluntly: c-ares has no handle for an individual `ares_getaddrinfo()`
request, so cancelling one calls `ares_cancel()`, which takes every pending query on the channel
with it -- the lookups of every other name in flight included. Each of them completes with
`errors::canceled`, which is what `ARES_ECANCELLED` and `ARES_EDESTRUCTION` are reported as, and
which is `boost::asio::error::operation_aborted` here and `boost::capy::error::canceled` on the
other runtime. What does the cancelling is a cancellation slot on Boost.Asio and the
`std::stop_token` of the awaiting coroutine on corosio.

That puts some weight on `ares_cancel()`, which is worth knowing about:
[GHSA-6wfj-rwm7-3542](https://github.com/c-ares/c-ares/security/advisories/GHSA-6wfj-rwm7-3542)
is a use-after-free/double-free in c-ares' query-completion handling, where a query's callback
runs while the query is still linked into the channel, and one of the two documented triggers is
an application calling `ares_cancel()`. It affects everything after 1.32.3 and is fixed in
**1.34.7** -- the devcontainer currently installs Debian's `libcares2`, which is 1.34.5, and CMake
says so when it configures.

## Lifetime

Destroying a resolver is `ares_destroy()`, which fails what is pending with `ARES_EDESTRUCTION`
and closes the sockets. The handlers of those operations are posted like any other, so they run
after the destructor has returned.

What the resolver had waiting in the event loop for itself needs more care. Cancelling a wait does
nothing for one that has completed already and only has its callback left to run, so such a
callback may find the resolver gone. The ones for socket readiness hold a `weak_ptr` to their
socket, which goes with the resolver and is the first thing they look at; the timer gives every
wait of its a stop token, and a wait that finds it triggered -- by the next `arm()`, by `cancel()`,
or by the timer being destroyed along with the resolver -- keeps quiet.

## Logging

The library writes nowhere. What it has to say -- the queries it starts and how they end, the
sockets c-ares opens, what a TLS handshake negotiated or why it failed -- goes to the function
given to `resolver::set_logger()`, and is not even formatted where there is none.

## Not done yet

* Batching several ready descriptors into a single `ares_process_fds()` call -- there is one
  completion per descriptor, so each one currently triggers its own call.
* Cancelling a single operation, see above.
* Anything but Linux: the sockets are waited on as POSIX descriptors, and the DNS over TLS
  transport creates its own with `SOCK_NONBLOCK`.
