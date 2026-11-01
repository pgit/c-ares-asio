# resolve

A command line for the library, and the place where it gets tried out.

`resolve` looks up the host names given on the command line, or those of the URLs listed in a
file, several at a time. By default it uses c-ares, driven by the event loop of the runtime the
library was built on; with `--native` it uses the resolver of that runtime instead -- Asio's
`ip::tcp::resolver` or Corosio's `resolver`, both of which run `getaddrinfo()` on a thread of
their own. Both should print the same addresses -- only the c-ares path also looks up the HTTPS
record, since `getaddrinfo()` answers with addresses and nothing else.

It is built along with the library where Boost.ProgramOptions and spdlog are found, as
`build/example/resolve/resolve`, and on either runtime: what it needs of its event loop -- the
loop itself, the signal handling, the workers of `--concurrency`, the resolver of the runtime --
is in [`runtime.hpp`](runtime.hpp), one file per runtime, so that the program itself is the same
on both. `build-corosio/example/resolve/resolve` is the one on Corosio, see
[doc/corosio.rst](../../doc/corosio.rst).

(`--native` was `--asio` until the library grew a second runtime.)

## Running

```sh
$ build/example/resolve/resolve -v localhost www.google.com
07:23:23.351 debug resolving localhost:443...
07:23:23.351 debug ares_getaddrinfo(localhost, 443)...
07:23:23.351 debug ares_getaddrinfo: Successful completion (0 timeout(s))
07:23:23.351 info localhost: ::1
07:23:23.351 info localhost: 127.0.0.1
...
```

`--service` selects the service name or port number to resolve for, `--native` switches to the
resolver of the runtime, `--file` reads the names from a file of URLs, `--concurrency` sets how many of them are
looked up at the same time (`--concurrency 1` for strictly one after the other, which is the
easiest to follow with `-v`), `--ordered` keeps the output in the order the names were given,
`--color` keeps the colours where the output does not go to a terminal (`| less -R`), `--help`
lists all options.

`Ctrl-C` interrupts a lookup cleanly: it cancels the operation instead of stopping the event loop,
which takes every pending query on the channel with it -- exactly what `Ctrl-C` is after, and
nothing else here cancels. The cancellation is a cancellation slot on Asio and a
`std::stop_token` on Corosio, which is the one thing `Ctrl-C` looks different behind.

`-v` also shows what the library has to say. It does not log by itself, so that is the function
given to `resolver::set_logger()` passing the messages on to spdlog.

## HTTPS records

Besides the addresses, every name is also looked up for its HTTPS record. The two questions go out
together, as [doc/https-records.rst](../../doc/https-records.rst) describes, and the printing stays
in a fixed order so the output does not depend on which answer lands first:

```sh
$ build/example/resolve/resolve --server 1.1.1.1 cloudflare.com
info cloudflare.com: 104.16.132.229
info cloudflare.com: 2606:4700::6810:84e5
...
info cloudflare.com: HTTPS 1 . alpn=h3,h2 ipv4hint=104.16.132.229,104.16.133.229 ipv6hint=2606:4700::6810:84e5,2606:4700::6810:85e5
```

A name with no such record says so at `--verbose` and is otherwise quiet, which is the common
case. Note that a resolver is free to answer type 65 with NODATA even where a record exists --
the Docker Desktop resolver in the devcontainer does, which is what `--server` is for.

## Many names at once

`--file` takes the names from a file of URLs instead, one per line. Only the host of each URL is of
any interest -- scheme, port, path and the rest are dropped, nothing is ever connected to -- and a
host that several URLs share is looked up once. [`hostnames.txt`](hostnames.txt) is such a file,
with a thousand names of popular sites. The results come as one line per host: the name, its first
IPv4 address, its first IPv6 address and its HTTPS record, with a dash for whatever there is none
of and, in parentheses, whatever went wrong:

```sh
$ build/example/resolve/resolve --server 1.1.1.1 --file url_list.txt
accounts.google.com                  142.251.127.84   2a00:1450:4001:c21::54                   -
accounts.nintendo.com                23.2.13.51       -                                        -
accounts.spotify.com                 151.101.131.42   2a04:4e42:600::810                       -
...
adsmanager.facebook.com              57.144.244.141   2a03:2880:f37a:8d:face:b00c:0:2          1 . alpn=h2,h3; 2 star.fallback.c10r.facebook.com alpn=h2,h3
...
serviceonline.bihar.gov.in           -                -                                        -  (Timeout while contacting DNS servers)
...
www.hobbylobby.com                   149.126.77.120   -                                        -  (HTTPS: DNS server returned general failure)
...
```

`--concurrency` (10 unless told otherwise) is the number of names in flight at any time, with or
without `--file`. It is that many coroutines, each taking the next name off the list, waiting for
its answer and coming back for more, held together by a ranged
[`experimental::make_parallel_group`](https://www.boost.org/doc/libs/release/doc/html/boost_asio/reference/experimental__make_parallel_group.html)
-- or, on Corosio, by `capy::when_all()` over the same coroutines.
There is still one channel and one thread: c-ares keeps all the queries apart by itself, over UDP
as well as pipelined on the one connection `--dot` has. For the 1000 names of a list of popular
sites, asked of 1.1.1.1:

| `--concurrency` | 1 | 10 | 50 |
| --- | --- | --- | --- |
| time | 84 s | 9 s | 3 s |

Every name is printed the moment its answer is in, so the output doubles as the progress report --
and comes in whatever order the answers land in. `--ordered` prints them in the order they were
given instead, which makes two runs comparable line by line. The price is that a single name which
takes its time, and a timeout is several seconds, holds back every row behind it: none of the
lookups, but all of the progress.

With `--native`, the number makes little difference: `getaddrinfo()` blocks, and what the runtime
has to call it on is what it really has in flight. Asio uses a single thread unless its
`resolver.threads` configuration says otherwise, so its lookups are answered strictly one after
the other; Corosio has a small pool of them (`io_context_options::threads`).

## DNS over TLS

`--dot` resolves over DNS over TLS, which needs a name to check the server's certificate against.
The short way is to give `--server` that name instead of an address: it is what goes into SNI and
what the certificate has to be for. c-ares takes addresses for its servers and nothing else, so
the name is looked up first -- the regular way, in the clear, of the servers from
`/etc/resolv.conf`, on a resolver of its own that is gone again before the first query proper:

```sh
$ build/example/resolve/resolve -v --dot --server dns.google www.google.com
21:11:14.293 debug looking up DNS server dns.google...
21:11:14.293 debug ares_getaddrinfo(dns.google, )...
21:11:14.312 debug ares_getaddrinfo: Successful completion (0 timeout(s))
21:11:14.313 debug using DNS server(s) 8.8.4.4:853
...
21:11:14.377 info fd=8 TLS established: TLSv1.3, cipher=TLS_AES_128_GCM_SHA256, group=X25519MLKEM768
21:11:14.377 info fd=8 TLS peer: subject=CN=dns.google, issuer=CN=WE2,O=Google Trust Services,C=US, verified
...
```

Of the addresses the name has, only the first becomes a server. All of them would look like
failover for free, but c-ares gives every query its three tries once *per server*, with the timeout
doubling each time round: a name nobody has an answer for is given up on after 3 attempts with one
server and after 12 with the four that `one.one.one.one` comes to, which took the 1000 names of
the list above from under 3 s to 8 s and more -- nearly all of it spent waiting for a handful of
them.

An address on `--server` is no name to check anything against, so there `--tls-hostname` has to
give one -- as it has to where `--server` names several hosts, of which none is the one for all of
the servers. `--tls-hostname` also wins over the name on `--server` wherever both are given, and
`--tls-no-verify` skips the check altogether:

```sh
$ build/example/resolve/resolve --dot --server 1.1.1.1 --tls-hostname one.one.one.one www.google.com
...

$ build/example/resolve/resolve --dot --server 1.1.1.1 --tls-hostname wrong.example.com www.google.com
13:32:27.547 error TLS handshake failed: hostname mismatch
13:32:27.624 error www.google.com: Could not contact DNS servers
```

How the transport works is in [doc/dns-over-tls.rst](../../doc/dns-over-tls.rst).

## Other servers

`--server` overrides the servers from `/etc/resolv.conf` (c-ares only), by address or by name,
with or without `--dot`, and with a port behind either (`dns.google:53`). It is the easy way to
watch the retry and timeout handling do its thing -- 192.0.2.1 is reserved for documentation and
goes nowhere:

```sh
$ build/example/resolve/resolve -v --server 192.0.2.1 www.google.com
07:23:49.737 debug ares_getaddrinfo(www.google.com, 443)...
07:23:49.737 debug socket state: fd=8 readable=true writable=false
07:23:51.737 debug query timeout
...
07:24:01.506 error www.google.com: Timeout while contacting DNS servers
```
