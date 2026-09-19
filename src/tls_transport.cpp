#include "tls_transport.hpp"

#include <spdlog/spdlog.h>

#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace
{

//
// Drains OpenSSL's error queue into something printable.
//
std::string sslError()
{
   std::string message;
   while (const auto code = ERR_get_error())
   {
      char buffer[256];
      ERR_error_string_n(code, buffer, sizeof(buffer));
      if (!message.empty())
         message += ", ";
      message += buffer;
   }

   return message.empty() ? "no error" : message;
}

} // namespace

// --------------------------------------------------------------------------------------------------

//
// One TCP connection c-ares has opened, and the TLS session riding on it.
//
struct TlsTransport::Session
{
   enum class State
   {
      Connecting, // TCP connect in flight
      Handshaking, // TLS handshake in flight
      Established, // c-ares may read and write
      Failed //
   };

   ~Session()
   {
      if (ssl)
         SSL_free(ssl);
   }

   State state = State::Connecting;
   SSL* ssl = nullptr;
};

// --------------------------------------------------------------------------------------------------

TlsTransport::TlsTransport(TlsOptions options) : m_options(std::move(options))
{
   m_context = SSL_CTX_new(TLS_client_method());
   if (!m_context)
      throw std::runtime_error("SSL_CTX_new: " + sslError());

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
         throw std::runtime_error("SSL_CTX_set_default_verify_paths: " + sslError());
      }
      SSL_CTX_set_verify(m_context, SSL_VERIFY_PEER, nullptr);
   }
   else
   {
      SSL_CTX_set_verify(m_context, SSL_VERIFY_NONE, nullptr);
   }
}

TlsTransport::~TlsTransport()
{
   m_sessions.clear(); // frees the SSL objects while the context is still around
   if (m_context)
      SSL_CTX_free(m_context);
}

void TlsTransport::install(ares_channel_t* channel)
{
   if (const auto status = ares_set_socket_functions_ex(channel, &functions(), this);
       status != ARES_SUCCESS)
      throw std::runtime_error(std::string("ares_set_socket_functions_ex: ") +
                               ares_strerror(static_cast<int>(status)));
}

TlsTransport::Session* TlsTransport::find(ares_socket_t fd)
{
   const auto it = m_sessions.find(fd);
   return it == m_sessions.end() ? nullptr : it->second.get();
}

// --------------------------------------------------------------------------------------------------

TlsTransport::Interest TlsTransport::advance(ares_socket_t fd, bool writable)
{
   auto* session = find(fd);
   if (!session)
      return Interest::None; // not one of ours, e.g. a UDP socket

   if (session->state == Session::State::Connecting)
   {
      if (!writable)
         return Interest::Write; // the connect is what we are still waiting for

      int error = 0;
      socklen_t length = sizeof(error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0)
      {
         spdlog::debug("fd={} connect failed: {}", fd, std::strerror(error ? error : errno));
         session->state = Session::State::Failed;
         return Interest::None; // let c-ares pick the failure up through recv/send
      }

      if (!startHandshake(*session, fd))
      {
         session->state = Session::State::Failed;
         return Interest::None;
      }

      session->state = Session::State::Handshaking;
   }

   if (session->state != Session::State::Handshaking)
      return Interest::None;

   ERR_clear_error();
   const int result = SSL_connect(session->ssl);
   if (result == 1)
   {
      session->state = Session::State::Established;
      spdlog::debug("fd={} TLS established: {} {}", fd, SSL_get_version(session->ssl),
                    SSL_get_cipher(session->ssl));
      return Interest::None;
   }

   switch (SSL_get_error(session->ssl, result))
   {
   case SSL_ERROR_WANT_READ:
      return Interest::Read;

   case SSL_ERROR_WANT_WRITE:
      return Interest::Write;

   default:
      //
      // A rejected certificate shows up here and nowhere else -- c-ares only ever gets to see a
      // connection that went away, so say what actually happened.
      //
      if (const auto result = SSL_get_verify_result(session->ssl); result != X509_V_OK)
         spdlog::error("TLS handshake failed: {}", X509_verify_cert_error_string(result));
      else
         spdlog::error("TLS handshake failed: {}", sslError());

      session->state = Session::State::Failed;
      return Interest::None;
   }
}

bool TlsTransport::startHandshake(Session& session, ares_socket_t fd)
{
   session.ssl = SSL_new(m_context);
   if (!session.ssl)
   {
      spdlog::error("SSL_new: {}", sslError());
      return false;
   }

   SSL_set_connect_state(session.ssl);
   if (SSL_set_fd(session.ssl, fd) != 1)
   {
      spdlog::error("SSL_set_fd: {}", sslError());
      return false;
   }

   //
   // Without a name there is nothing to put in SNI and nothing to check the certificate against,
   // which is why --dot insists on either --tls-hostname or --tls-no-verify.
   //
   if (!m_options.hostname.empty())
   {
      SSL_set_tlsext_host_name(session.ssl, m_options.hostname.c_str());

      if (m_options.verify)
      {
         SSL_set_hostflags(session.ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
         if (SSL_set1_host(session.ssl, m_options.hostname.c_str()) != 1)
         {
            spdlog::error("SSL_set1_host({}): {}", m_options.hostname, sslError());
            return false;
         }
      }
   }

   return true;
}

// --------------------------------------------------------------------------------------------------

ares_socket_t TlsTransport::onSocket(int domain, int type, int protocol)
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
      m_sessions.emplace(fd, std::make_unique<Session>());
   }

   return fd;
}

int TlsTransport::onClose(ares_socket_t fd)
{
   if (auto* session = find(fd); session && session->state == Session::State::Established)
      SSL_shutdown(session->ssl); // best effort close_notify, the answer is already in

   m_sessions.erase(fd);
   return ::close(fd);
}

int TlsTransport::onConnect(ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length)
{
   if (::connect(fd, address, length) == 0)
      return 0;

   return -1; // errno is set, and for a non-blocking socket it is almost always EINPROGRESS
}

ares_ssize_t TlsTransport::onRecv(ares_socket_t fd, void* buffer, size_t length)
{
   auto* session = find(fd);
   if (!session)
      return ::recv(fd, buffer, length, 0);

   switch (session->state)
   {
   case Session::State::Connecting:
   case Session::State::Handshaking:
      errno = EAGAIN; // advance() is still working on it
      return -1;

   case Session::State::Failed:
      errno = ECONNRESET;
      return -1;

   case Session::State::Established:
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
      spdlog::debug("fd={} SSL_read: {}", fd, sslError());
      session->state = Session::State::Failed;
      errno = ECONNRESET;
      return -1;
   }
}

ares_ssize_t TlsTransport::onSend(ares_socket_t fd, const void* buffer, size_t length, int flags)
{
   auto* session = find(fd);
   if (!session)
      return ::send(fd, buffer, length, flags);

   switch (session->state)
   {
   case Session::State::Connecting:
   case Session::State::Handshaking:
      errno = EAGAIN;
      return -1;

   case Session::State::Failed:
      errno = ECONNRESET;
      return -1;

   case Session::State::Established:
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
      spdlog::debug("fd={} SSL_write: {}", fd, sslError());
      session->state = Session::State::Failed;
      errno = ECONNRESET;
      return -1;
   }
}

// --------------------------------------------------------------------------------------------------

//
// The C side of it. Everything c-ares needs but we have no reason to intercept is forwarded to the
// system call it would have made itself.
//
const struct ares_socket_functions_ex& TlsTransport::functions()
{
   static const struct ares_socket_functions_ex table{
      .version = 1,
      .flags = ARES_SOCKFUNC_FLAG_NONBLOCKING,

      .asocket = [](int domain, int type, int protocol, void* data) -> ares_socket_t
   { return static_cast<TlsTransport*>(data)->onSocket(domain, type, protocol); },

      .aclose = [](ares_socket_t fd, void* data) -> int
   { return static_cast<TlsTransport*>(data)->onClose(fd); },

      .asetsockopt = [](ares_socket_t fd, ares_socket_opt_t opt, const void* value,
                        ares_socklen_t size, void*) -> int
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
         // Fast open would defer the connect to the first write, and the first write is the
         // one we need a finished handshake for. ENOSYS tells c-ares we mean it.
         //
         errno = ENOSYS;
         return -1;
      }

      errno = ENOSYS;
      return -1;
   },

      .aconnect = [](ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length,
                     unsigned int, void* data) -> int
   { return static_cast<TlsTransport*>(data)->onConnect(fd, address, length); },

      .arecvfrom = [](ares_socket_t fd, void* buffer, size_t length, int, struct sockaddr* address,
                      ares_socklen_t* address_len, void* data) -> ares_ssize_t
   {
      //
      // Only ever called with an address for unconnected UDP sockets, which we do not have.
      //
      if (address && address_len)
         *address_len = 0;
      return static_cast<TlsTransport*>(data)->onRecv(fd, buffer, length);
   },

      .asendto = [](ares_socket_t fd, const void* buffer, size_t length, int flags,
                    const struct sockaddr*, ares_socklen_t, void* data) -> ares_ssize_t
   { return static_cast<TlsTransport*>(data)->onSend(fd, buffer, length, flags); },

      .agetsockname = [](ares_socket_t fd, struct sockaddr* address, ares_socklen_t* length,
                         void*) -> int { return ::getsockname(fd, address, length); },

      .abind = [](ares_socket_t fd, unsigned int, const struct sockaddr* address, socklen_t length,
                  void*) -> int { return ::bind(fd, address, length); },

      .aif_nametoindex = [](const char* name, void*) -> unsigned int
   { return ::if_nametoindex(name); },

      .aif_indextoname = [](unsigned int index, char* buffer, size_t size, void*) -> const char*
   {
      if (size < IF_NAMESIZE)
         return nullptr;
      return ::if_indextoname(index, buffer);
   },
   };

   return table;
}
