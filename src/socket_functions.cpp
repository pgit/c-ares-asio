// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#include "socket_functions.hpp"

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace cares_asio::detail
{

int set_socket_option(ares_socket_t fd, ares_socket_opt_t opt, const void* value,
                      ares_socklen_t size)
{
   switch (opt)
   {
   case ARES_SOCKET_OPT_SENDBUF_SIZE:
      return ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, value, size);

   case ARES_SOCKET_OPT_RECVBUF_SIZE:
      return ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, value, size);

   case ARES_SOCKET_OPT_BIND_DEVICE:
      return ::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, value, size);

   case ARES_SOCKET_OPT_TCP_FASTOPEN:
      //
      // See socket_functions.hpp: a socket that fast open has made writable is not one whose
      // connection is up, and that is the difference c-ares goes by.
      //
      errno = ENOSYS;
      return -1;
   }

   errno = ENOSYS;
   return -1;
}

namespace
{

//
// Everything else is what c-ares would do itself. The sockets are non-blocking and close on exec
// from the start, which is what ARES_SOCKFUNC_FLAG_NONBLOCKING promises the library.
//
const struct ares_socket_functions_ex& functions()
{
   static const struct ares_socket_functions_ex table{
      .version = 1,
      .flags = ARES_SOCKFUNC_FLAG_NONBLOCKING,

      .asocket = [](int domain, int type, int protocol, void*) -> ares_socket_t {
         const int fd = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
         if (fd < 0)
            return ARES_SOCKET_BAD;

         //
         // DNS over TCP sends the length of a message and the message itself, and the second
         // would wait for the first to be acknowledged.
         //
         if (type == SOCK_STREAM)
         {
            const int on = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
         }

         return fd;
      },

      .aclose = [](ares_socket_t fd, void*) -> int { return ::close(fd); },

      .asetsockopt = [](ares_socket_t fd, ares_socket_opt_t opt, const void* value,
                        ares_socklen_t size, void*) -> int { //
         return set_socket_option(fd, opt, value, size);
      },

      //
      // Fast open is refused above, so there is nothing in 'flags' left to act on: a connect is
      // a connect, and for a non-blocking socket it is almost always still in progress when it
      // comes back.
      //
      .aconnect = [](ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length,
                     unsigned int, void*) -> int { //
         return ::connect(fd, address, length);
      },

      .arecvfrom = [](ares_socket_t fd, void* buffer, size_t length, int flags,
                      struct sockaddr* address, ares_socklen_t* address_len,
                      void*) -> ares_ssize_t { //
         return ::recvfrom(fd, buffer, length, flags, address, address_len);
      },

      .asendto = [](ares_socket_t fd, const void* buffer, size_t length, int flags,
                    const struct sockaddr* address, ares_socklen_t address_len,
                    void*) -> ares_ssize_t { //
         return ::sendto(fd, buffer, length, flags, address, address_len);
      },

      .agetsockname = [](ares_socket_t fd, struct sockaddr* address, ares_socklen_t* length,
                         void*) -> int { //
         return ::getsockname(fd, address, length);
      },

      .abind = [](ares_socket_t fd, unsigned int, const struct sockaddr* address,
                  ares_socklen_t length, void*) -> int { //
         return ::bind(fd, address, length);
      },

      .aif_nametoindex = [](const char* name, void*) -> unsigned int {
         return ::if_nametoindex(name);
      },

      .aif_indextoname = [](unsigned int index, char* buffer, size_t size, void*) -> const char* {
         if (size < IF_NAMESIZE)
            return nullptr;
         return ::if_indextoname(index, buffer);
      },
   };

   return table;
}

} // namespace

ares_status_t install_socket_functions(ares_channel_t* channel)
{
   return ares_set_socket_functions_ex(channel, &functions(), nullptr);
}

} // namespace cares_asio::detail
