// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#include "tls_transport.hpp"

#include "log.hpp"
#include "socket_functions.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509v3.h>

#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace c_ares_async::detail
{

namespace
{

//
// Drains OpenSSL's error queue into something printable.
//
std::string ssl_error()
{
   std::string message;
   while (const auto code = ERR_get_error())
   {
      std::array<char, 256> buffer;
      ERR_error_string_n(code, buffer.data(), buffer.size());
      if (!message.empty())
         message += ", ";
      message += buffer.data();
   }

   return message.empty() ? "no error" : message;
}

//
// Key exchange group of the handshake, e.g. "X25519 (253 bits)" or "X25519MLKEM768". The name is
// known even for groups without an EVP_PKEY, like the post-quantum hybrids, which is also why
// there may be no bit count to go with it.
//
std::string key_exchange(SSL* ssl)
{
#if defined(OPENSSL_IS_AWSLC) || defined(OPENSSL_IS_BORINGSSL)
   const char* group = SSL_get_group_name(SSL_get_group_id(ssl));
#else
   const char* group = SSL_get0_group_name(ssl);
#endif
   std::string name = group ? group : "unknown";

   EVP_PKEY* key = nullptr;
   if (SSL_get_peer_tmp_key(ssl, &key) != 1 || !key)
      return name;

   const auto bits = EVP_PKEY_bits(key);
   EVP_PKEY_free(key);
   return std::format("{} ({} bits)", name, bits);
}

//
// A distinguished name in RFC 2253 form, e.g. "CN=dns.google".
//
std::string distinguished_name(const X509_NAME* name)
{
   if (!name)
      return "none";

   BIO* bio = BIO_new(BIO_s_mem());
   if (!bio)
      return "?";

   std::string result = "?";
   if (X509_NAME_print_ex(bio, name, 0, XN_FLAG_RFC2253) >= 0)
   {
      const char* data = nullptr;
      const auto length = BIO_get_mem_data(bio, &data);
      result.assign(data, static_cast<size_t>(length));
   }

   BIO_free(bio);
   return result;
}

//
// What was negotiated.
//
std::string handshake_info(SSL* ssl)
{
   return std::format("{}, cipher={}, group={}{}", SSL_get_version(ssl), SSL_get_cipher_name(ssl),
                      key_exchange(ssl), SSL_session_reused(ssl) ? ", resumed" : "");
}

//
// Who we negotiated it with.
//
std::string peer_info(SSL* ssl)
{
   const X509* peer = SSL_get0_peer_certificate(ssl);
   if (!peer)
      return "no peer certificate";

   //
   // Where verification is off the chain is not checked at all, so do not pretend it was.
   //
   const bool verified =
      SSL_get_verify_mode(ssl) != SSL_VERIFY_NONE && SSL_get_verify_result(ssl) == X509_V_OK;

   return std::format("subject={}, issuer={}, {}", distinguished_name(X509_get_subject_name(peer)),
                      distinguished_name(X509_get_issuer_name(peer)),
                      verified ? "verified" : "NOT verified");
}

} // namespace

// --------------------------------------------------------------------------------------------------

//
// One TCP connection c-ares has opened, and the TLS session riding on it.
//
struct tls_transport::tls_session
{
   enum class phase
   {
      connecting, // TCP connect in flight
      handshaking, // TLS handshake in flight
      established, // c-ares may read and write
      failed //
   };

   ~tls_session()
   {
      if (ssl)
         SSL_free(ssl);
   }

   phase state = phase::connecting;
   SSL* ssl = nullptr;
};

// --------------------------------------------------------------------------------------------------

tls_transport::tls_transport(tls_options options, const log_function& logger)
   : m_options(std::move(options)), m_logger(logger)
{
   m_context = SSL_CTX_new(TLS_client_method());
   if (!m_context)
      throw std::runtime_error("SSL_CTX_new: " + ssl_error());

   SSL_CTX_set_min_proto_version(m_context, TLS1_2_VERSION);

   //
   // c-ares retries a short write with the same query, but not necessarily from the same buffer,
   // and is happy with a partial write -- which is exactly what these two modes allow.
   //
   SSL_CTX_set_mode(m_context, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

   if (m_options.verify)
   {
      if (SSL_CTX_set_default_verify_paths(m_context) != 1)
      {
         SSL_CTX_free(m_context);
         throw std::runtime_error("SSL_CTX_set_default_verify_paths: " + ssl_error());
      }
      SSL_CTX_set_verify(m_context, SSL_VERIFY_PEER, nullptr);
   }
   else
   {
      SSL_CTX_set_verify(m_context, SSL_VERIFY_NONE, nullptr);
   }
}

tls_transport::~tls_transport()
{
   m_sessions.clear(); // frees the SSL objects while the context is still around
   if (m_context)
      SSL_CTX_free(m_context);
}

void tls_transport::install(ares_channel_t* channel)
{
   if (const auto status = ares_set_socket_functions_ex(channel, &functions(), this);
       status != ARES_SUCCESS)
      throw std::runtime_error(std::string("ares_set_socket_functions_ex: ") +
                               ares_strerror(static_cast<int>(status)));
}

tls_transport::tls_session* tls_transport::find(ares_socket_t fd)
{
   const auto it = m_sessions.find(fd);
   return it == m_sessions.end() ? nullptr : it->second.get();
}

// --------------------------------------------------------------------------------------------------

tls_transport::interest tls_transport::advance(ares_socket_t fd, bool writable)
{
   auto* session = find(fd);
   if (!session)
      return interest::none; // not one of ours, e.g. a UDP socket

   if (session->state == tls_session::phase::connecting)
   {
      if (!writable)
         return interest::write; // the connect is what we are still waiting for

      int error = 0;
      socklen_t length = sizeof(error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0)
         error = errno;

      //
      // SO_ERROR is consume-on-read, and an event loop that reads it in order to report the
      // failure of the socket as the failure of the wait -- corosio's does -- leaves a zero
      // behind. getpeername() answers the same question and keeps answering it: a socket whose
      // connect did not go through has no peer.
      //
      if (error == 0)
      {
         sockaddr_storage peer{};
         socklen_t peer_length = sizeof(peer);
         if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_length) != 0)
            error = errno;
      }

      if (error != 0)
      {
         log_debug(m_logger, "fd={} connect failed: {}", fd, std::strerror(error));
         session->state = tls_session::phase::failed;
         return interest::none; // let c-ares pick the failure up through recv/send
      }

      if (!start_handshake(*session, fd))
      {
         session->state = tls_session::phase::failed;
         return interest::none;
      }

      session->state = tls_session::phase::handshaking;
   }

   if (session->state != tls_session::phase::handshaking)
      return interest::none;

   ERR_clear_error();
   const int result = SSL_connect(session->ssl);
   if (result == 1)
   {
      session->state = tls_session::phase::established;
      log_info(m_logger, "fd={} TLS established: {}", fd, handshake_info(session->ssl));
      log_info(m_logger, "fd={} TLS peer: {}", fd, peer_info(session->ssl));
      return interest::none;
   }

   switch (SSL_get_error(session->ssl, result))
   {
   case SSL_ERROR_WANT_READ:
      return interest::read;

   case SSL_ERROR_WANT_WRITE:
      return interest::write;

   default:
      //
      // A rejected certificate shows up here and nowhere else -- c-ares only ever gets to see a
      // connection that went away, so say what actually happened.
      //
      if (const auto result = SSL_get_verify_result(session->ssl); result != X509_V_OK)
         log_error(m_logger, "TLS handshake failed: {}", X509_verify_cert_error_string(result));
      else
         log_error(m_logger, "TLS handshake failed: {}", ssl_error());

      session->state = tls_session::phase::failed;
      return interest::none;
   }
}

bool tls_transport::start_handshake(tls_session& session, ares_socket_t fd)
{
   session.ssl = SSL_new(m_context);
   if (!session.ssl)
   {
      log_error(m_logger, "SSL_new: {}", ssl_error());
      return false;
   }

   SSL_set_connect_state(session.ssl);
   if (SSL_set_fd(session.ssl, fd) != 1)
   {
      log_error(m_logger, "SSL_set_fd: {}", ssl_error());
      return false;
   }

   //
   // Without a name there is nothing to put in SNI and nothing to check the certificate against,
   // which leaves the chain: the certificate is then one to trust, though not known to be the
   // server's. That is the caller's decision, see tls_options::hostname.
   //
   if (!m_options.hostname.empty())
   {
      SSL_set_tlsext_host_name(session.ssl, m_options.hostname.c_str());

      if (m_options.verify)
      {
         SSL_set_hostflags(session.ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
         if (SSL_set1_host(session.ssl, m_options.hostname.c_str()) != 1)
         {
            log_error(m_logger, "SSL_set1_host({}): {}", m_options.hostname, ssl_error());
            return false;
         }
      }
   }

   return true;
}

// --------------------------------------------------------------------------------------------------

ares_socket_t tls_transport::on_socket(int domain, int type, int protocol)
{
   const int fd = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
   if (fd < 0)
      return ARES_SOCKET_BAD;

   //
   // Only the TCP sockets carry a TLS session. UDP ones are passed through untouched -- with
   // ARES_FLAG_USETCP there should not be any, but c-ares owns that decision, not us.
   //
   if (type == SOCK_STREAM)
   {
      const int on = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
      m_sessions.emplace(fd, std::make_unique<tls_session>());
   }

   return fd;
}

int tls_transport::on_close(ares_socket_t fd)
{
   if (auto* session = find(fd); session && session->state == tls_session::phase::established)
      SSL_shutdown(session->ssl); // best effort close_notify, the answer is already in

   m_sessions.erase(fd);
   return ::close(fd);
}

int tls_transport::on_connect(ares_socket_t fd, const struct sockaddr* address,
                              ares_socklen_t length)
{
   if (::connect(fd, address, length) == 0)
      return 0;

   return -1; // errno is set, and for a non-blocking socket it is almost always EINPROGRESS
}

ares_ssize_t tls_transport::on_recv(ares_socket_t fd, void* buffer, size_t length)
{
   auto* session = find(fd);
   if (!session)
      return ::recv(fd, buffer, length, 0);

   switch (session->state)
   {
   case tls_session::phase::connecting:
   case tls_session::phase::handshaking:
      errno = EAGAIN; // advance() is still working on it
      return -1;

   case tls_session::phase::failed:
      errno = ECONNRESET;
      return -1;

   case tls_session::phase::established:
      break;
   }

   const int capacity =
      static_cast<int>(std::min(length, static_cast<size_t>(std::numeric_limits<int>::max())));

   ERR_clear_error();
   const int count = SSL_read(session->ssl, buffer, capacity);
   if (count > 0)
      return count;

   switch (SSL_get_error(session->ssl, count))
   {
   case SSL_ERROR_WANT_READ:
   case SSL_ERROR_WANT_WRITE:
      errno = EAGAIN;
      return -1;

   case SSL_ERROR_ZERO_RETURN:
      return 0; // clean shutdown, which is end of file as far as c-ares is concerned

   default:
      log_debug(m_logger, "fd={} SSL_read: {}", fd, ssl_error());
      session->state = tls_session::phase::failed;
      errno = ECONNRESET;
      return -1;
   }
}

ares_ssize_t tls_transport::on_send(ares_socket_t fd, const void* buffer, size_t length, int flags)
{
   auto* session = find(fd);
   if (!session)
      return ::send(fd, buffer, length, flags);

   switch (session->state)
   {
   case tls_session::phase::connecting:
   case tls_session::phase::handshaking:
      errno = EAGAIN;
      return -1;

   case tls_session::phase::failed:
      errno = ECONNRESET;
      return -1;

   case tls_session::phase::established:
      break;
   }

   const int count =
      static_cast<int>(std::min(length, static_cast<size_t>(std::numeric_limits<int>::max())));

   ERR_clear_error();
   const int written = SSL_write(session->ssl, buffer, count);
   if (written > 0)
      return written;

   switch (SSL_get_error(session->ssl, written))
   {
   case SSL_ERROR_WANT_READ:
   case SSL_ERROR_WANT_WRITE:
      errno = EAGAIN;
      return -1;

   default:
      log_debug(m_logger, "fd={} SSL_write: {}", fd, ssl_error());
      session->state = tls_session::phase::failed;
      errno = ECONNRESET;
      return -1;
   }
}

// --------------------------------------------------------------------------------------------------

//
// The C side of it. Everything c-ares needs but we have no reason to intercept is forwarded to the
// system call it would have made itself.
//
const struct ares_socket_functions_ex& tls_transport::functions()
{
   static const struct ares_socket_functions_ex table{
      .version = 1,
      .flags = ARES_SOCKFUNC_FLAG_NONBLOCKING,

      .asocket = [](int domain, int type, int protocol, void* data) -> ares_socket_t {
         return static_cast<tls_transport*>(data)->on_socket(domain, type, protocol);
      },

      .aclose = [](ares_socket_t fd, void* data) -> int {
         return static_cast<tls_transport*>(data)->on_close(fd);
      },

      //
      // The same as the plain socket functions answer, fast open turned down and all: the
      // handshake needs a connection before the first write just as much, see
      // socket_functions.hpp.
      //
      .asetsockopt = [](ares_socket_t fd, ares_socket_opt_t opt, const void* value,
                        ares_socklen_t size, void*) -> int { //
         return set_socket_option(fd, opt, value, size);
      },

      .aconnect = [](ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length,
                     unsigned int, void* data) -> int {
         return static_cast<tls_transport*>(data)->on_connect(fd, address, length);
      },

      .arecvfrom = [](ares_socket_t fd, void* buffer, size_t length, int, struct sockaddr* address,
                      ares_socklen_t* address_len, void* data) -> ares_ssize_t {
         //
         // Only ever called with an address for unconnected UDP sockets, which we do not have.
         //
         if (address && address_len)
            *address_len = 0;
         return static_cast<tls_transport*>(data)->on_recv(fd, buffer, length);
      },

      .asendto = [](ares_socket_t fd, const void* buffer, size_t length, int flags,
                    const struct sockaddr*, ares_socklen_t, void* data) -> ares_ssize_t {
         return static_cast<tls_transport*>(data)->on_send(fd, buffer, length, flags);
      },

      .agetsockname = [](ares_socket_t fd, struct sockaddr* address, ares_socklen_t* length,
                         void*) -> int { //
         return ::getsockname(fd, address, length);
      },

      .abind = [](ares_socket_t fd, unsigned int, const struct sockaddr* address, socklen_t length,
                  void*) -> int { //
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

} // namespace c_ares_async::detail
