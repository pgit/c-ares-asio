# Documentation

## Reference

The reference is made by [MrDocs](https://www.mrdocs.com) from the comments in the public headers,
[`include/cares_async/resolver.hpp`](../include/cares_async/resolver.hpp) and
[`include/cares_async/error.hpp`](../include/cares_async/error.hpp), as HTML with a page per symbol:

```sh
cmake --preset dev -DMRDOCS_EXECUTABLE=/path/to/mrdocs
cmake --build build --target docs
```

The pages end up in `build/doc/html`, with `index.html` to start from. MrDocs is a single program
that the [releases](https://github.com/cppalliance/mrdocs/releases) have ready to run, and where it
is on the `PATH` there is no need to say where. The target is not part of a regular build, not
there at all where CMake finds no `mrdocs`, and not there in a COROSIO build -- the comments are
the same in both, see [corosio.md](corosio.md).

[`mrdocs.yml`](mrdocs.yml) is the configuration, which also has MrDocs warn about whatever is
public but not documented. It reads a single source file, [`mrdocs.cpp`](mrdocs.cpp), that includes
the library and nothing else.

## How it works

* [How the integration works](design.md) -- c-ares in an event loop: sockets, timeouts,
  completion, cancellation and what happens when a resolver goes away.
* [The corosio runtime](corosio.md) -- the second runtime the library builds on, what it maps to
  and what porting to it found.
* [HTTPS records](https-records.md) -- the second kind of lookup, and what comes back from it.
* [DNS over TLS](dns-over-tls.md) -- the transport that c-ares does not have.

## Examples

* [`example/asio/minimal.cpp`](../example/asio/minimal.cpp) and
  [`example/corosio/minimal.cpp`](../example/corosio/minimal.cpp) -- the shortest program that
  resolves a name, once per runtime.
* [`example/resolve`](../example/resolve) -- a command line with all of it: many names at once,
  HTTPS records, DNS over TLS, cancellation.
