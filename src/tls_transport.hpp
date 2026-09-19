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

#include <ares.h>

#include <openssl/ssl.h>

#include <map>
#include <memory>
#include <string>

struct TlsOptions
{
   bool enabled = false;
   std::string hostname; // for SNI and certificate verification, empty if none was given
   bool verify = true;
};

// --------------------------------------------------------------------------------------------------

class TlsTransport
{
public:
   //
   // What the socket is waiting for. 'None' means the TLS session is settled, one way or the
   // other, and readiness on the socket is c-ares' business again.
   //
   enum class Interest
   {
      None,
      Read,
      Write
   };

   explicit TlsTransport(TlsOptions options);
   ~TlsTransport();

   TlsTransport(const TlsTransport&) = delete;
   TlsTransport& operator=(const TlsTransport&) = delete;

   //
   // Hands our socket functions to the channel, which must not have opened any sockets yet.
   //
   void install(ares_channel_t* channel);

   //
   // Drives the TCP connect and, after it, the TLS handshake for 'fd'.
   //
   // The handshake is deliberately *not* driven from the socket callbacks: c-ares decides what to
   // wait for based on which queries it has pending, which has nothing to do with what OpenSSL
   // needs next, and answering a write-readiness event with EAGAIN because the handshake wants to
   // read would just spin. So the event loop calls this first and only lets c-ares see the socket
   // once there is a TLS session on it.
   //
   Interest advance(ares_socket_t fd, bool writable);

private:
   struct Session;

   Session* find(ares_socket_t fd);
   bool startHandshake(Session& session, ares_socket_t fd);

   //
   // The socket functions themselves. c-ares calls the static thunks, which forward here.
   //
   ares_socket_t onSocket(int domain, int type, int protocol);
   int onClose(ares_socket_t fd);
   int onConnect(ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length);
   ares_ssize_t onRecv(ares_socket_t fd, void* buffer, size_t length);
   ares_ssize_t onSend(ares_socket_t fd, const void* buffer, size_t length, int flags);

   static const struct ares_socket_functions_ex& functions();

   TlsOptions m_options;
   SSL_CTX* m_context = nullptr;
   std::map<ares_socket_t, std::unique_ptr<Session>> m_sessions;
};
