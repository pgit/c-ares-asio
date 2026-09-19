# c-ares-asio

Scratch project for experimenting with [c-ares](https://c-ares.org/) integration into
[Boost.Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html).

`resolve` looks up the host names given on the command line, one after the other. By default it
uses c-ares, driven by the ASIO event loop; with `--asio` it uses ASIO's own `ip::tcp::resolver`
instead, which runs `getaddrinfo()` on an internal thread. Both should print the same addresses.

## How the integration works

[`src/ares_resolver.cpp`](src/ares_resolver.cpp) holds all of it:

* the channel is created with `ARES_OPT_SOCK_STATE_CB`, so c-ares reports every socket it opens,
  closes, or changes its read/write interest on -- no c-ares event thread is involved;
* each such socket is wrapped in a non-owning `posix::stream_descriptor`, and readiness is awaited
  with `async_wait()`. The descriptor is `release()`d before c-ares closes the socket;
* readiness is fed back with [`ares_process_fds()`](https://c-ares.org/docs/ares_process_fds.html).
  Since the socket state callback only fires on *changes*, the waits are re-armed after every call;
* `ares_timeout()` drives a `steady_timer`, whose expiry calls `ares_process_fds()` with no events.

Queries are exposed as a regular ASIO asynchronous operation (`async_resolve()` via
`async_initiate`), so they work with `use_awaitable`, `as_tuple`, callbacks and futures alike.

Cancellation works too (`Ctrl-C` interrupts a lookup cleanly), but only bluntly: c-ares has no
handle for an individual `ares_getaddrinfo()` request, so the cancellation slot calls
`ares_cancel()`, which takes every pending query on the channel with it.

Not done yet: batching several ready descriptors into a single `ares_process_fds()` call -- ASIO
delivers one completion handler per descriptor, so each one currently triggers its own call.

## Building

The devcontainer brings the whole toolchain (clang, libc++, Boost, spdlog, c-ares).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Running

```sh
$ build/src/resolve -v localhost www.google.com
07:23:23.351 debug resolving localhost:80...
07:23:23.351 debug ares_getaddrinfo(localhost, 80)...
07:23:23.351 debug ares_getaddrinfo: Successful completion (0 timeout(s))
07:23:23.351 info localhost: ::1
07:23:23.351 info localhost: 127.0.0.1
...
```

`--service` selects the service name or port number to resolve for, `--asio` switches to the ASIO
resolver, `--help` lists all options.

`--server` overrides the servers from `/etc/resolv.conf` (c-ares only), which is the easy way to
watch the retry and timeout handling do its thing -- 192.0.2.1 is reserved for documentation and
goes nowhere:

```sh
$ build/src/resolve -v --server 192.0.2.1 www.google.com
07:23:49.737 debug ares_getaddrinfo(www.google.com, 80)...
07:23:49.737 debug socket state: fd=8 readable=true writable=false
07:23:51.737 debug query timeout
...
07:24:01.506 error www.google.com: Timeout while contacting DNS servers
```
