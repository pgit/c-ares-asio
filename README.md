# c-ares-async

[c-ares](https://c-ares.org/) name resolution as asynchronous operations, on
[Boost.Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html) or on
capy/corosio.

ASIO's own `ip::tcp::resolver` runs `getaddrinfo()` on an internal thread: one lookup at a time,
blocking, and with addresses for an answer and nothing else. `c_ares_async::resolver` has c-ares do
the asking instead, driven by the event loop of the application -- no thread of its own, any
number of lookups in flight at once, and more to ask for than addresses:

* **addresses** of a host name, as endpoints to connect to;
* **HTTPS records** ([RFC 9460](https://www.rfc-editor.org/rfc/rfc9460)), which tell a client
  which protocols an endpoint speaks before it connects;
* over UDP and TCP, or over **DNS over TLS** ([RFC 7858](https://www.rfc-editor.org/rfc/rfc7858)),
  which c-ares has no transport for by itself;
* as operations that take any completion token -- `use_awaitable`, `deferred`, `use_future`, a
  callback -- and can be cancelled.

The same library also builds on [capy](https://github.com/cppalliance/capy)/[corosio](https://github.com/cppalliance/corosio)
instead of Boost.Asio, where the operations are coroutines and nothing else. One build tree is one
runtime or the other, `-DC_ARES_ASYNC_API=ASIO` or `COROSIO`; see [the corosio runtime](doc/corosio.rst)
for what the two map to.

## Example

```cpp
#include <c_ares_async.hpp>

#include <boost/asio.hpp>

#include <iostream>

namespace asio = boost::asio;

asio::awaitable<void> lookup(std::string host)
{
   c_ares_async::resolver resolver(co_await asio::this_coro::executor);

   const auto [ec, endpoints] =
      co_await resolver.async_resolve(host, "443", asio::as_tuple(asio::use_awaitable));

   if (ec)
      std::cerr << host << ": " << ec.message() << '\n';

   for (const auto& endpoint : endpoints)
      std::cout << host << ": " << endpoint << '\n';
}

int main()
{
   asio::io_context context;
   asio::co_spawn(context, lookup("example.com"), asio::detached);
   context.run();
}
```

Both runtimes have the same operation as a coroutine, which yields the error and the result as a
tuple -- `resolve()` and `resolve_https()`. On corosio that is all there is, and the program around
it is capy's:

```cpp
c_ares_async::Task<void> lookup(c_ares_async::Executor executor, std::string host)
{
   c_ares_async::resolver resolver(executor);

   const auto [ec, endpoints] = co_await resolver.resolve(host, "443");

   if (ec)
      std::cerr << host << ": " << ec.message() << '\n';

   for (const auto& endpoint : endpoints)
      std::cout << host << ": " << endpoint.address() << ':' << endpoint.port() << '\n';
}
```

[`example/asio/minimal.cpp`](example/asio/minimal.cpp) and
[`example/corosio/minimal.cpp`](example/corosio/minimal.cpp) are these two, going on to the HTTPS
record, and [`example/resolve`](example/resolve) is a command line with everything else: many
names at once, DNS over TLS, cancellation.

## Requirements

* Linux. Sockets are waited on as POSIX descriptors, and nothing else has been tried.
* A C++20 compiler and standard library. clang 23 with libc++ and GCC 14 with libstdc++ are the
  ones the tests run with.
* CMake 3.25 or later.
* [Boost](https://www.boost.org/) 1.88 or later, of which only headers are used. A COROSIO build
  needs no Boost at all: capy and corosio are fetched at pinned commits and built along with the
  library.
* [c-ares](https://c-ares.org/) 1.34 or later. Make that 1.34.7 where lookups get cancelled: up
  to there, `ares_cancel()` can run into
  [GHSA-6wfj-rwm7-3542](https://github.com/c-ares/c-ares/security/advisories/GHSA-6wfj-rwm7-3542),
  and CMake warns about it.
* [OpenSSL](https://www.openssl.org/) 3.2 or later, or [AWS-LC](https://github.com/aws/aws-lc),
  for DNS over TLS.

## Using it

With the library installed, `find_package()` brings the target and everything it depends on:

```cmake
find_package(c_ares_async 0.1 REQUIRED)

target_link_libraries(app PRIVATE c_ares_async::c_ares_async)
```

The target has the same name where the sources are part of the build instead, through
`add_subdirectory()` or `FetchContent`. Tests, examples and documentation are left out then:

```cmake
include(FetchContent)

FetchContent_Declare(
    c_ares_async
    GIT_REPOSITORY https://github.com/pgit/c-ares-async.git
    GIT_TAG <tag or commit>)
FetchContent_MakeAvailable(c_ares_async)

target_link_libraries(app PRIVATE c_ares_async::c_ares_async)
```

Either way the target asks for C++20 and brings the include paths of Boost and c-ares with it,
whose status codes are what a failed lookup is compared against. The TLS library stays out of
sight: no header of the library includes one of its.

| Option | Default | |
| --- | --- | --- |
| `C_ARES_ASYNC_API` | `ASIO` | The I/O runtime to build on, `ASIO` (Boost.Asio) or `COROSIO` (capy/corosio), see [doc/corosio.rst](doc/corosio.rst). |
| `C_ARES_ASYNC_TLS` | `OpenSSL` | The TLS library to build against, `OpenSSL` or `AWS-LC`. `ssl_ROOT` says where the latter is installed. |
| `BUILD_SHARED_LIBS` | `OFF` | Builds a shared library instead of a static one. |
| `C_ARES_ASYNC_BUILD_TESTS` | top-level only | Builds the tests. Also off where `BUILD_TESTING` is. |
| `C_ARES_ASYNC_BUILD_EXAMPLES` | top-level only | Builds the examples. |
| `C_ARES_ASYNC_BUILD_DOCS` | top-level only | Builds the code of the guide, and adds the `docs` target where there is a `doxygen` and a `sphinx-build` to make them with. |
| `C_ARES_ASYNC_INSTALL` | top-level only | Generates the install rules. Off, and refused, in a COROSIO build. |

## Building, testing, installing

The build files set no compiler and no flags of their own. What the repository is developed with
is in [`CMakePresets.json`](CMakePresets.json), for the toolchain that the devcontainer brings
(clang, libc++, Boost, c-ares, OpenSSL, AWS-LC, GoogleTest):

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

| Preset | Directory | |
| --- | --- | --- |
| `dev` | `build` | Debug, AWS-LC. The one to work with. |
| `release` | `build-release` | The same, optimized. |
| `asan` | `build-asan` | With the address and undefined behaviour sanitizers. |
| `tsan` | `build-tsan` | With the thread sanitizer. |
| `openssl` | `build-openssl` | OpenSSL instead of AWS-LC, and the library as a shared one. |
| `gcc` | `build-gcc` | GCC and libstdc++. |
| `corosio` | `build-corosio` | On capy/corosio instead of Boost.Asio. |
| `corosio-asan` | `build-corosio-asan` | The same, with the address and undefined behaviour sanitizers. |

Anywhere else it is a CMake project like any other:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
cmake --install build --prefix /usr/local
```

The tests need no network. They ask a DNS server of their own, which runs in the test on the
loopback interface and answers over UDP, TCP and TLS. [GoogleTest](https://github.com/google/googletest)
is used where it is installed and fetched where it is not. Two of the tests build a project of
their own, [`test/cmake_test`](test/cmake_test), against the installed library and against the
source tree -- which is what keeps the two CMake snippets above true.

## Documentation

`cmake --build build --target docs` makes all of it into one site with
[Sphinx](https://www.sphinx-doc.org), see [doc](doc/README.md):

* [The programmer's guide](doc/programmers-guide.rst): what a program does with the library, step
  by step and with code. This is the place to start.
* [The API reference](doc/reference.rst), made from the comments in
  [`include/c_ares_async`](include/c_ares_async) by [Doxygen](https://www.doxygen.nl).
* [How the integration works](doc/design.rst): c-ares in an event loop.
* [The corosio runtime](doc/corosio.rst): the second runtime, and what porting to it found.
* [HTTPS records](doc/https-records.rst)
* [DNS over TLS](doc/dns-over-tls.rst)

## Good to know

* **Cancelling one lookup cancels all of them.** c-ares has no handle for a single request, so
  what a cancellation can do is `ares_cancel()`, for the whole resolver. Every pending operation
  then completes with `c_ares_async::errors::canceled` -- `operation_aborted` on Boost.Asio,
  `capy::error::canceled` on corosio.
* **A resolver is for one thread at a time.** Where the `io_context` runs on several, make its
  executor a strand and start the lookups from inside it.
* **A lookup is not complete until the context ran its handler**, including where a resolver is
  destroyed with lookups pending: they complete as cancelled, but not from inside the destructor.
* **The library does not log.** What it would have logged goes to the function given to
  `resolver::set_logger()`, which is where the reason for a failed TLS handshake is to be found.
* **There is no promise of a stable interface before 1.0.** The minor version is part of the name
  of the shared library until then, and `find_package(c_ares_async 0.1)` does not accept 0.2.

## License

Distributed under the Boost Software License, Version 1.0. See [LICENSE_1_0.txt](LICENSE_1_0.txt)
or https://www.boost.org/LICENSE_1_0.txt.
