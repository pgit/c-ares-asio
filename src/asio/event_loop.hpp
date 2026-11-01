// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The event loop of Asio, see ../event_loop.hpp.
//
#pragma once

#include <c_ares_async/runtime.hpp>

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace c_ares_async::detail
{

//
// corosio/event_loop.hpp has two more of these, launch() and yield_now(), which have nothing to
// do here: a wait of Asio's only ever completes from the event loop, so nothing has to be put
// there by hand to keep it off the stack of whoever armed it.
//

//
// What readiness to wait for. c-ares asks for one or the other, or for both at a time.
//
enum class wait_type
{
   read,
   write
};

//
// One socket of c-ares', watched but not owned: the event loop is used to wait for readiness on
// it and for nothing else, which is why the descriptor is released again instead of being closed.
//
class Descriptor
{
public:
   //
   // Throws system_error where 'fd' cannot be watched, as Asio's own constructor does.
   //
   Descriptor(const Executor& executor, int fd) : descriptor_(executor, fd) {}
   ~Descriptor() { detach(); }

   Descriptor(const Descriptor&) = delete;
   Descriptor& operator=(const Descriptor&) = delete;

   bool is_open() const noexcept { return descriptor_.is_open(); }

   //
   // Cancels what is pending and gives the file descriptor back, without closing it. The
   // callbacks of the cancelled waits still run, with errors::canceled.
   //
   void detach()
   {
      if (descriptor_.is_open())
         descriptor_.release();
   }

   //
   // Calls 'on_ready(error_code)' once, when the descriptor is ready for 'type' -- or with an
   // error, errors::canceled among them, where it never gets there.
   //
   template <typename F>
   void wait(wait_type type, F&& on_ready)
   {
      descriptor_.async_wait(type == wait_type::read
                                ? boost::asio::posix::stream_descriptor::wait_read
                                : boost::asio::posix::stream_descriptor::wait_write,
                             std::forward<F>(on_ready));
   }

private:
   boost::asio::posix::stream_descriptor descriptor_;
};

//
// A timer that calls back when it expires. Every arm() replaces the wait before it.
//
// Cancelling a timer does nothing for a wait that has expired already and only has its handler
// left on the queue -- and by the time that one runs, the timer may have been re-armed, or be
// gone along with whoever wanted to hear from it. So every wait carries a stop token of its own,
// which arm() and cancel() trigger, and a handler that finds it triggered keeps quiet.
//
class Timer
{
public:
   explicit Timer(const Executor& executor) : timer_(executor) {}
   ~Timer() { cancel(); }

   Timer(const Timer&) = delete;
   Timer& operator=(const Timer&) = delete;

   template <typename F>
   void arm(std::chrono::steady_clock::duration delay, F&& on_expiry)
   {
      cancel();
      stop_.emplace();
      timer_.expires_after(delay);
      timer_.async_wait([on_expiry = std::forward<F>(on_expiry),
                         token = stop_->get_token()](const error_code& ec) mutable {
         if (!ec && !token.stop_requested())
            on_expiry();
      });
   }

   void cancel()
   {
      if (stop_)
         stop_->request_stop();

      stop_.reset();
      timer_.cancel();
   }

private:
   boost::asio::steady_timer timer_;
   std::optional<std::stop_source> stop_;
};

//
// The bytes of an IPv4 or IPv6 address, as the text of it. What c-ares hands over in the
// "ipv4hint" and "ipv6hint" parameters of an HTTPS record is the wire format, not an endpoint.
//
inline std::string address_text(const std::array<unsigned char, 4>& bytes)
{
   return boost::asio::ip::address_v4(bytes).to_string();
}

inline std::string address_text(const std::array<unsigned char, 16>& bytes)
{
   return boost::asio::ip::address_v6(bytes).to_string();
}

//
// 'address' as an Endpoint of the runtime's own, or nothing where it is of a family that the
// runtime has no endpoint for.
//
inline std::optional<Endpoint> make_endpoint(const sockaddr* address, std::size_t length)
{
   if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in))
   {
      sockaddr_in sa{};
      std::memcpy(&sa, address, sizeof(sa));

      boost::asio::ip::address_v4::bytes_type bytes;
      std::memcpy(bytes.data(), &sa.sin_addr, bytes.size());
      return Endpoint(boost::asio::ip::address_v4(bytes), ntohs(sa.sin_port));
   }

   if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6))
   {
      sockaddr_in6 sa{};
      std::memcpy(&sa, address, sizeof(sa));

      boost::asio::ip::address_v6::bytes_type bytes;
      std::memcpy(bytes.data(), &sa.sin6_addr, bytes.size());
      return Endpoint(boost::asio::ip::address_v6(bytes, sa.sin6_scope_id), ntohs(sa.sin6_port));
   }

   return std::nullopt;
}

} // namespace c_ares_async::detail
