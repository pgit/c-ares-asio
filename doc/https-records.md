# HTTPS records

Besides the addresses of a name, a resolver can look up its HTTPS record
([RFC 9460](https://www.rfc-editor.org/rfc/rfc9460), RR type 65) -- the record that tells a client
which protocols an endpoint speaks before it connects.

The addresses and the record are two separate questions, so `async_resolve()` and
`async_resolve_https()` are two separate operations -- and can go out *together* rather than one
after the other. c-ares has no trouble keeping several queries in flight on a channel, so the
record costs no second round trip, and over TLS the two share the connection as well. An
[`experimental::make_parallel_group`](https://www.boost.org/doc/libs/release/doc/html/boost_asio/reference/experimental__make_parallel_group.html)
over the two operations is all the waiting takes:

```cpp
auto [order, ec, endpoints, https_ec, records] =
   co_await boost::asio::experimental::make_parallel_group(
      resolver.async_resolve(host, "443", boost::asio::deferred),
      resolver.async_resolve_https(host, boost::asio::deferred))
      .async_wait(boost::asio::experimental::wait_for_all(), boost::asio::use_awaitable);
```

Being separate questions, a name that has no addresses can still be asked for its record. The
exception is a name that cannot be encoded as a DNS name at all, which is no question for any
record type: c-ares turns that one down locally without a query, but where `ares_getaddrinfo()`
correctly says `ARES_EBADNAME`, `ares_search_dnsrec()` says `ARES_ENOMEM` (1.34.5) -- so the
address error is the one to believe, rather than one that blames memory for a typo.

A name with no such record completes with `ARES_ENODATA`, which is the common case rather than a
problem. Note that a resolver is free to answer type 65 with NODATA even where a record exists --
the Docker Desktop resolver in the devcontainer does, which is what `set_servers()` is for.

## What comes back

c-ares parses these itself: `ares_search_dnsrec()` asks the question the same way
`ares_getaddrinfo()` does, search domains and all, and the answer comes back as an
`ares_dns_record_t` rather than as bytes. The priority and target come straight off it, but the
SvcParams are opaque values whose syntax depends on their key, so
[`src/resolver.cpp`](../src/resolver.cpp) asks `ares_dns_opt_get_datatype()` which wire format
each one uses and renders it the way the RFC presents it. An `https_record` has them as one string
per parameter:

| `priority` | `target` | `params` |
| --- | --- | --- |
| 1 | `.` | `alpn=h3,h2` `ipv4hint=104.16.132.229,104.16.133.229` `ipv6hint=2606:4700::6810:84e5,2606:4700::6810:85e5` |

A `priority` of 0 is the alias form, where `target` names the host to continue at and there are no
parameters. A key that c-ares has no name for reads `key` and its number, and a value it has no
format for, like that of `ech`, is given in hexadecimal.

The records of a set come in no particular order, and not in the same one twice. Sorting by
`priority` gives the order a client goes through them in, and puts the alias form first.
