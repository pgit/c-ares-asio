# c-ares-asio

Scratch project for experimenting with [c-ares](https://c-ares.org/) integration into
[Boost.Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html).

`resolve` looks up the host names given on the command line, one after the other. By default it
uses c-ares, driven by the ASIO event loop; with `--asio` it uses ASIO's own `ip::tcp::resolver`
instead, which runs `getaddrinfo()` on an internal thread. Both should print the same addresses --
only the c-ares path also looks up the HTTPS record, since `getaddrinfo()` answers with addresses
and nothing else.

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

## HTTPS records

Besides the addresses, every name is also looked up for its HTTPS record
([RFC 9460](https://www.rfc-editor.org/rfc/rfc9460), RR type 65) -- the record that tells a client
which protocols an endpoint speaks before it connects. The two lookups are independent, so a name
that has no addresses is still asked for its record.

c-ares parses these itself: `ares_search_dnsrec()` asks the question the same way
`ares_getaddrinfo()` does, search domains and all, and the answer comes back as an
`ares_dns_record_t` rather than as bytes. The priority and target come straight off it, but the
SvcParams are opaque values whose syntax depends on their key, so
[`ares_resolver.cpp`](src/ares_resolver.cpp) asks `ares_dns_opt_get_datatype()` which wire format
each one uses and renders it the way the RFC presents it:

```sh
$ build/src/resolve --server 1.1.1.1 cloudflare.com
14:20:46.175 info cloudflare.com: 104.16.133.229
...
14:20:46.192 info cloudflare.com: HTTPS 1 . alpn=h3,h2 ipv4hint=104.16.132.229,104.16.133.229 ipv6hint=2606:4700::6810:84e5,2606:4700::6810:85e5
```

A name with no such record says so at `--verbose` and is otherwise quiet, which is the common
case. Note that a resolver is free to answer type 65 with NODATA even where a record exists --
the Docker Desktop resolver in this devcontainer does, which is what `--server` is for.

## DNS over TLS

c-ares has no encrypted transport of its own. [`ares_set_servers_csv()`](https://c-ares.org/docs/ares_set_servers_csv.html)
documents `dns+tls://` (port 853) and `dns+https://` (port 443) alongside `dns://`, but, in its own
words, "the underlying implementations for those features do not yet exist and therefore will
result in errors if they are attempted to be used" -- the parser rejects both with `ARES_EBADSTR`,
in 1.34.5 as in 1.34.8. The only TLS-related things it does have are the `TLSA` and `HTTPS` record
*types*, which are things to ask about, not ways to ask.

What it does have is [`ares_set_socket_functions_ex()`](https://c-ares.org/docs/ares_set_socket_functions_ex.html),
which replaces its socket layer wholesale. DNS over TCP is a length-prefixed byte stream and c-ares
does that framing itself, so wrapping the stream in TLS is enough to make it DNS over TLS
([RFC 7858](https://www.rfc-editor.org/rfc/rfc7858)). That is what
[`src/tls_transport.cpp`](src/tls_transport.cpp) does, in OpenSSL:

* `--dot` puts the channel in `ARES_FLAG_USEVC | ARES_FLAG_STAYOPEN` -- TCP only, and the
  connection outlives the query, because otherwise it is a handshake per name;
* the servers move to port 853. A port spelled out on `--server` is taken at face value, the ones
  that came from `/etc/resolv.conf` with `:53` on them are moved regardless;
* `asocket()` creates the socket and remembers it, `arecvfrom()`/`asendto()` become `SSL_read()`
  and `SSL_write()`, and TCP fast open is refused with `ENOSYS` -- it would defer the connect to
  the first write, and the first write is the one that needs a finished handshake.

The handshake itself is deliberately *not* driven from those callbacks. c-ares decides what to wait
for from the queries it has pending, which has nothing to do with what OpenSSL needs next, and
answering a write-readiness event with `EAGAIN` because the handshake wants to read would just spin
-- a connected socket is writable nearly always. So `AresResolver::onSocketEvent()` calls
`TlsTransport::advance()` first, waits on whatever `SSL_connect()` asked for, and only lets c-ares
see the socket once there is a session on it. A handshake that fails leaves the socket poisoned,
and the next `SSL_read()` reports `ECONNRESET` so that c-ares fails the server the usual way.

DoH would need HTTP/2 framing and a request/response translation on top, which is a much worse fit
for a byte-stream hook -- it is not done here.

## Building

The devcontainer brings the whole toolchain (clang, libc++, Boost, spdlog, c-ares, OpenSSL).

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

`--dot` resolves over DNS over TLS. Since `--server` takes an address, there is no name to check
the certificate against unless `--tls-hostname` gives one -- pass it, or `--tls-no-verify` to skip
the check:

```sh
$ build/src/resolve -v --dot --server 1.1.1.1 --tls-hostname one.one.one.one www.google.com
13:32:16.573 debug using DNS server(s) 1.1.1.1:853
13:32:16.623 debug fd=8 TLS established: TLSv1.3 TLS_AES_256_GCM_SHA384
13:32:16.642 info www.google.com: 142.251.157.119
...

$ build/src/resolve --dot --server 1.1.1.1 --tls-hostname wrong.example.com www.google.com
13:32:27.547 error TLS handshake failed: hostname mismatch
13:32:27.624 error www.google.com: Could not contact DNS servers
```

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
