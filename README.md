# c-ares-asio

Scratch project for experimenting with [c-ares](https://c-ares.org/) integration into
[Boost.Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html).

At the moment, this is just a standalone starting point: `resolve` looks up the host names given on
the command line using ASIO's own `ip::tcp::resolver`, which runs `getaddrinfo()` on an internal
thread. That is the behaviour the c-ares based resolver is supposed to replace.

## Building

The devcontainer brings the whole toolchain (clang, libc++, Boost, spdlog).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Running

```sh
$ build/src/resolve -v localhost www.google.com
13:37:00.000 debug resolving localhost:80...
13:37:00.000 info localhost: ::1
13:37:00.000 info localhost: 127.0.0.1
...
```

`--service` selects the service name or port number to resolve for, `--help` lists all options.
