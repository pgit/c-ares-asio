# DNS over TLS

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
[`src/tls_transport.cpp`](../src/tls_transport.cpp) does, against the OpenSSL API:

* `tls_options::enabled` puts the channel in `ARES_FLAG_USEVC | ARES_FLAG_STAYOPEN` -- TCP only,
  and the connection outlives the query, because otherwise it is a handshake per name;
* the servers move to port 853. The ones that came from `/etc/resolv.conf` with `:53` on them are
  moved when the resolver is made, and a list given to `set_servers()` gets the port wherever it
  names none -- a port that it does spell out is taken at face value;
* `asocket()` creates the socket and remembers it, `arecvfrom()`/`asendto()` become `SSL_read()`
  and `SSL_write()`, and TCP fast open is refused with `ENOSYS` -- it would defer the connect to
  the first write, and the first write is the one that needs a finished handshake.

The handshake itself is deliberately *not* driven from those callbacks. c-ares decides what to wait
for from the queries it has pending, which has nothing to do with what the TLS library needs next,
and answering a write-readiness event with `EAGAIN` because the handshake wants to read would just
spin -- a connected socket is writable nearly always. So `resolver::on_socket_event()` calls
`tls_transport::advance()` first, waits on whatever `SSL_connect()` asked for, and only lets c-ares
see the socket once there is a session on it. A handshake that fails leaves the socket poisoned,
and the next `SSL_read()` reports `ECONNRESET` so that c-ares fails the server the usual way.

DoH would need HTTP/2 framing and a request/response translation on top, which is a much worse fit
for a byte-stream hook -- it is not done here.

## Whose certificate

A certificate is verified against the trust store that the TLS library uses by default, and
against `tls_options::hostname`, which also goes into SNI. c-ares takes addresses for its servers
and nothing else, so the name cannot come from the server list: it has to be given, and a resolver
that only has the name of its server needs that name looked up first -- by another resolver, in
the clear. [`example/resolve`](../example/resolve) does exactly that for a name on `--server`.

Without a name there is nothing to check the certificate against, so only its chain is verified.
`tls_options::verify` set to false skips the check altogether, which is for testing.

c-ares only ever gets to see a connection that went away, so a lookup that fails for a certificate
says no more than `ARES_ECONNREFUSED`. Why the handshake failed is in the message that goes to the
logger, and nowhere else:

```
error TLS handshake failed: hostname mismatch
```

## One server per name

Of the addresses that the name of a server has, it is best to make only the first a server. All of
them would look like failover for free, but c-ares gives every query its three tries once *per
server*, with the timeout doubling each time round: a name nobody has an answer for is given up on
after 3 attempts with one server and after 12 with the four that `one.one.one.one` comes to. For a
list of 1000 names of popular sites that took the run from under 3 s to 8 s and more -- nearly all
of it spent waiting for a handful of them.

## Which TLS library

`-DC_ARES_ASYNC_TLS=OpenSSL` (the default) or `-DC_ARES_ASYNC_TLS=AWS-LC`, with `-Dssl_ROOT=` saying
where the latter is installed. The transport uses nothing beyond the OpenSSL API that AWS-LC
implements too, so the switch is a link-time one: no source file knows which of the two it got.
