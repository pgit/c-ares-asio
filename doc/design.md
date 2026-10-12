# How the integration works

c-ares is told about nothing but its own sockets, and is driven entirely from the ASIO event loop.
[`src/resolver.cpp`](../src/resolver.cpp) holds all of it:

* the channel is created with `ARES_OPT_SOCK_STATE_CB`, so c-ares reports every socket it opens,
  closes, or changes its read/write interest on -- no c-ares event thread is involved;
* each such socket is wrapped in a non-owning `posix::stream_descriptor`, and readiness is awaited
  with `async_wait()`. The descriptor is `release()`d before c-ares closes the socket;
* readiness is fed back with [`ares_process_fds()`](https://c-ares.org/docs/ares_process_fds.html).
  Since the socket state callback only fires on *changes*, the waits are re-armed after every call;
* `ares_timeout()` drives a `steady_timer`, whose expiry calls `ares_process_fds()` with no events.

Queries are exposed as regular ASIO asynchronous operations (`async_resolve()` and
`async_resolve_https()`, via `async_initiate`), so they work with `use_awaitable`, `as_tuple`,
`deferred`, callbacks and futures alike.

## Completion

c-ares calls back from inside `ares_getaddrinfo()` or `ares_process_fds()`, and for a name it can
answer without a query -- an address, `localhost`, an entry of `/etc/hosts` -- that is before the
initiating function has even returned. A completion handler must never be invoked from in there, so
it is posted: to the executor of the resolver, from where it is passed on to the executor the
handler is associated with, if it has one.

## Cancellation

Cancellation works, but only bluntly: c-ares has no handle for an individual `ares_getaddrinfo()`
request, so the cancellation slot calls `ares_cancel()`, which takes every pending query on the
channel with it -- the lookups of every other name in flight included. Each of them completes with
`boost::asio::error::operation_aborted`, which is what `ARES_ECANCELLED` and `ARES_EDESTRUCTION`
are reported as.

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
nothing for one that has completed already and only has its handler left to run, so such a handler
may find the resolver gone. The ones for socket readiness hold a `weak_ptr` to their socket and the
one for the timeout holds a `weak_ptr` to the timer; both go with the resolver, and the handlers
look at that before they look at anything else.

## Logging

The library writes nowhere. What it has to say -- the queries it starts and how they end, the
sockets c-ares opens, what a TLS handshake negotiated or why it failed -- goes to the function
given to `resolver::set_logger()`, and is not even formatted where there is none.

## Not done yet

* Batching several ready descriptors into a single `ares_process_fds()` call -- ASIO delivers one
  completion handler per descriptor, so each one currently triggers its own call.
* Cancelling a single operation, see above.
* Anything but Linux: the sockets are waited on as POSIX descriptors, and the DNS over TLS
  transport creates its own with `SOCK_NONBLOCK`.
