# The corosio runtime, next to Boost.Asio

One source tree that builds either as it always did, on Boost.Asio with completion tokens, or on
[capy](https://github.com/cppalliance/capy)/[corosio](https://github.com/cppalliance/corosio) with
a coroutine-only API. The choice is made at configure time:

```sh
cmake --preset corosio          # -DCARES_ASYNC_API=COROSIO, in build-corosio/
cmake --build --preset corosio
ctest --preset corosio
```

`CARES_ASYNC_API` is `ASIO` or `COROSIO` and nothing else is needed: capy and corosio are fetched
at the commits [`CMakeLists.txt`](../CMakeLists.txt) pins and built as part of the build. A
COROSIO build needs no Boost at all -- which is also why it has no install rules, see
[*Constraints*](#constraints).

## What the two runtimes are

| | ASIO | COROSIO |
| --- | --- | --- |
| Operations | `async_resolve()`, `async_resolve_https()`, any completion token, plus the coroutine spelling | `resolve()`, `resolve_https()` -- coroutines, and nothing else |
| `Task<T>` | `boost::asio::awaitable<T>` | `boost::capy::task<T>` |
| `Executor` | `boost::asio::any_io_executor` | `boost::capy::any_executor` |
| `Endpoint` | `boost::asio::ip::tcp::endpoint` | `boost::corosio::endpoint` |
| `error_code` | `boost::system::error_code` | `std::error_code` |
| `errors::canceled` | `boost::asio::error::operation_aborted` | `boost::capy::error::canceled` |
| Cancelling a lookup | a cancellation slot, `cancel_after()` | the `std::stop_token` of the awaiting coroutine, `corosio::timeout()` |

Both have the same coroutine spelling, which yields the error and the result as a tuple:

```cpp
auto [ec, endpoints] = co_await resolver.resolve("example.com", "443");
```

That is what the shared tests are written in. On Boost.Asio it is `async_resolve()` with
`as_tuple(use_awaitable)` behind it; on corosio it is a `capy::io_task`, since
`capy::io_result<T>` *is* `std::tuple<std::error_code, T>` -- so a lookup goes into
`corosio::timeout()` and `capy::when_all()` as it is, with nothing in between.

## Where the code is

Only three places know which runtime they are in, and they are the ones that say `CARES_ASYNC_ASIO`
or `CARES_ASYNC_COROSIO` (exactly one of which is 1, from the generated `cares_async/config.hpp`):

| | |
| --- | --- |
| [`include/cares_async/runtime.hpp`](../include/cares_async/runtime.hpp) | the vocabulary of the interface, which picks `asio/runtime.hpp` or `corosio/runtime.hpp` |
| [`src/event_loop.hpp`](../src/event_loop.hpp) | the event loop the implementation drives: `Descriptor`, `Timer`, `make_endpoint()`, again one file per runtime |
| [`include/cares_async/resolver.hpp`](../include/cares_async/resolver.hpp) | the front end: the completion-token operations are ASIO-only |

The `resolve` example has a small layer of the same kind for what a program needs beyond the
library -- the event loop itself, the signal handling, the workers of `--concurrency`, the
resolver of the runtime that `--native` compares against:
[`example/resolve/runtime.hpp`](../example/resolve/runtime.hpp), again one file per runtime, with
one `main.cpp` on top of either. The minimal example, which is there to be read rather than used,
has a version per runtime instead: [`example/asio/minimal.cpp`](../example/asio/minimal.cpp) and
[`example/corosio/minimal.cpp`](../example/corosio/minimal.cpp).

Everything else -- the lookups, the HTTPS records, the server lists, the DNS over TLS transport --
is written once, against those names. `asio/runtime.hpp` documents each of them and
`corosio/runtime.hpp` has the same definitions in the same order, so the pair can be read side by
side; the same goes for `src/asio/event_loop.hpp` and `src/corosio/event_loop.hpp`, and for the
test fixtures in `test/asio/` and `test/corosio/`.

The tests follow the same rule: what is about protocol behaviour is written once, in the coroutine
spelling, and runs on either runtime. What is about one runtime only has a file of its own --
[`test/asio/async_test.cpp`](../test/asio/async_test.cpp) for completion tokens, associated
executors, cancellation slots and strands, [`test/corosio/stop_test.cpp`](../test/corosio/stop_test.cpp)
for stop tokens and `corosio::timeout()`.

The fake DNS server stays on Boost.Asio in both: it serves UDP, TCP and TLS, and a second
implementation of it would only be a second thing to keep right. The COROSIO fixture steps its
`asio::io_context` and the resolver's `corosio::io_context` from the one thread the test runs on,
so the tests stay as deterministic as they are on ASIO.

## What the port found

Three of these are bugs that were in the ASIO build all along, where they are races rather than
certainties -- corosio answers "is this socket ready" with a `poll()` of its own before it parks
anything, so what ASIO gets to one turn of the event loop later happens here right away.

* **c-ares must not be re-entered.** It hands its sockets over from inside `ares_process_fds()`,
  through the socket state callback, so a wait armed from there was started -- and, for a
  readiness that held already, completed -- on the stack of that very call, which called
  `ares_process_fds()` again. c-ares closed its connections underneath itself and came apart in a
  use-after-free. The COROSIO `Descriptor` posts every wait before it starts it.
* **TCP Fast Open makes a socket writable before its connection is up.** c-ares enables it on
  every TCP connection and asks to be told when the socket becomes writable, which
  `ares_set_socket_functions_ex()` documents to mean "the connect has finished" -- and an event
  loop that answers "is this socket writable" cannot tell the two apart. Told so anyway, c-ares
  writes the query a second time and drops the connection, which costs a lookup every retry it
  has. [`src/socket_functions.hpp`](../src/socket_functions.hpp) turns fast open down, as the TLS
  transport already did.
* **`SO_ERROR` is consume-on-read.** corosio's reactor reads it to report the error of a socket as
  the error of the wait, which left the TLS transport's connect check with a zero and a handshake
  started on a dead socket -- all the way to a `SIGPIPE`. It confirms the connect with
  `getpeername()` now, which answers the same question and keeps answering it.
* **A socket that has failed is ready**, as far as a readiness wait is concerned: whoever owns the
  socket is the one to find the failure out, by reading or writing. Boost.Asio reports such a wait
  as ready; corosio reports the error of the socket, and the COROSIO `Descriptor` maps that back.
* **A timer that has expired may still be queued** when it is re-armed or cancelled, in both
  runtimes. Each wait carries a stop token of its own, and a callback that finds it triggered
  keeps quiet -- which is also what keeps a timeout that was due when the resolver went away from
  looking for it.

## Constraints

* **A capy context has to run dry.** Destroying a `corosio::io_context` with a coroutine still
  suspended on it leaks that coroutine's frame -- unlike ASIO, capy cannot unwind it. Destroying a
  resolver hands its cancelled waits and its timer back to the context, so the context has to run
  once more, which is what the tests do (`build-corosio-asan/` is where LeakSanitizer says so).
  That is the same rule as for the completions: a lookup is not finished until the context ran.
* **`corosio::io_context::poll()` does not ask the reactor.** It runs what is ready already; a
  socket that became readable in the meantime is not noticed. Stepping such a context by hand
  takes `run_one_for()`, see [`test/corosio/event_loop_fixture.hpp`](../test/corosio/event_loop_fixture.hpp).
* **`std::optional<cares_async::resolver>` does not compile.** capy's `any_executor` has a
  converting constructor that is constrained on `std::copy_constructible` only, so asking whether
  a resolver converts to an executor -- which `std::optional` does, through
  `is_trivially_move_constructible` -- instantiates it with the resolver and fails inside. A
  `std::unique_ptr` works, and so does holding the resolver by value. To be filed upstream.
* **corosio has no output operator for an endpoint.** `endpoint.address()` and `endpoint.port()`
  are there to be printed; the tests teach GoogleTest the same (`PrintTo`).
* **No install rules.** capy and corosio are built from pinned sources here and not installed, so
  an exported target would point at libraries that are not there. `CARES_ASYNC_INSTALL` is off by
  default in a COROSIO build and refuses to be turned on, which also means the two CMake
  consumer tests are ASIO-only.
* **No `docs` target.** The reference is made from an ASIO build; the comments are the same in
  both, and `asio/runtime.hpp` documents the vocabulary for either.

## Not done yet

* Threads. A resolver is for one thread at a time in either runtime, and where the ASIO build has
  a strand test (`async_test.cpp`), the COROSIO one has none: `capy::strand` exists and corosio
  resumes a coroutine through the executor of its `io_env`, so a resolver on a strand should stay
  on it, but nothing here runs it that way yet.
* Moving the pins. Both libraries are pre-release and change their API on `develop` without
  notice, so moving a pin is a commit of its own.
