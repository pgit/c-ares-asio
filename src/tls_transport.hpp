//
// DNS over TLS (RFC 7858) for c-ares.
//
// c-ares has no encrypted transport of its own: the dns+tls:// and dns+https:// server URI schemes
// are documented by ares_set_servers_csv(), but the implementations behind them do not exist and
// the parser rejects them with ARES_EBADSTR. What c-ares does have is
// ares_set_socket_functions_ex(), which replaces its socket layer wholesale -- and since DNS over
// TCP is nothing but a length-prefixed byte stream, and c-ares does that framing itself, wrapping
// the stream in TLS is all it takes to turn it into DNS over TLS.
//
// https://c-ares.org/docs/ares_set_socket_functions_ex.html
//
#pragma once

#include <cares_asio/resolver.hpp>

#include <ares.h>

#include <openssl/ssl.h>

#include <map>
#include <memory>

namespace cares_asio::detail
{

class tls_transport
{
public:
   //
   // What the socket is waiting for. 'none' means the TLS session is settled, one way or the
   // other, and readiness on the socket is c-ares' business again.
   //
   enum class interest
   {
      none,
      read,
      write
   };

   //
   // 'logger' is the resolver's, and looked at afresh for every message.
   //
   tls_transport(tls_options options, const log_function& logger);
   ~tls_transport();

   tls_transport(const tls_transport&) = delete;
   tls_transport& operator=(const tls_transport&) = delete;

   //
   // Hands our socket functions to the channel, which must not have opened any sockets yet.
   //
   void install(ares_channel_t* channel);

   //
   // Drives the TCP connect and, after it, the TLS handshake for 'fd'.
   //
   // The handshake is deliberately *not* driven from the socket callbacks: c-ares decides what to
   // wait for based on which queries it has pending, which has nothing to do with what the TLS
   // library needs next, and answering a write-readiness event with EAGAIN because the handshake
   // wants to read would just spin. So the event loop calls this first and only lets c-ares see the socket
   // once there is a TLS session on it.
   //
   interest advance(ares_socket_t fd, bool writable);

private:
   struct tls_session;

   tls_session* find(ares_socket_t fd);
   bool start_handshake(tls_session& session, ares_socket_t fd);

   //
   // The socket functions themselves. c-ares calls the static thunks, which forward here.
   //
   ares_socket_t on_socket(int domain, int type, int protocol);
   int on_close(ares_socket_t fd);
   int on_connect(ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length);
   ares_ssize_t on_recv(ares_socket_t fd, void* buffer, size_t length);
   ares_ssize_t on_send(ares_socket_t fd, const void* buffer, size_t length, int flags);

   static const struct ares_socket_functions_ex& functions();

   tls_options m_options;
   const log_function& m_logger;
   SSL_CTX* m_context = nullptr;
   std::map<ares_socket_t, std::unique_ptr<tls_session>> m_sessions;
};

} // namespace cares_asio::detail
