// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The event loop of corosio, see ../event_loop.hpp. asio/event_loop.hpp has the documentation of
// each of these, and what is said here is what differs.
//
#pragma once

#include <cares_async/runtime.hpp>

#include <boost/capy/cond.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ip_address.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <chrono>
#include <coroutine>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace cares_async::detail
{

//
// Starts 'task' on 'executor', detached, with 'token' to stop it: nobody waits for it, and what
// it throws is dropped. corosio has no co_spawn(), and this is what capy starts a task with.
//
inline void launch(const Executor& executor, std::stop_token token, Task<void> task)
{
   boost::capy::run_async(
      executor, std::move(token), []() noexcept {},
      [](std::exception_ptr) noexcept {})(std::move(task));
}

//
// Lets whatever else is ready to run on the caller's executor run first. capy starts a task by
// dispatching it, which runs it inline where the caller is on its executor already -- so this is
// what a task awaits to be sure it is not on the stack of whoever launched it.
//
struct yield_now
{
   boost::capy::continuation continuation;

   bool await_ready() const noexcept { return false; }
   std::coroutine_handle<> await_suspend(std::coroutine_handle<> h, const boost::capy::io_env* env)
   {
      continuation.h = h;
      env->executor.post(continuation);
      return std::noop_coroutine();
   }
   void await_resume() const noexcept {}
};

enum class wait_type
{
   read,
   write
};

//
// One socket of c-ares', watched but not owned.
//
// corosio has no descriptor type that would take any file descriptor: readiness comes from the
// sockets, which is where assign() adopts one that was made elsewhere -- and the socket has to be
// of the right kind, so which one it is comes from SO_TYPE. Either way nothing but wait() is ever
// called on it, and release() hands the descriptor back to c-ares' ownership.
//
class Descriptor
{
public:
   Descriptor(const Executor& executor, int fd) : executor_(executor)
   {
      int type = 0;
      socklen_t length = sizeof(type);
      if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0)
         throw system_error(make_system_error(errno), "getsockopt(SO_TYPE)");

      auto impl = std::make_shared<state>();
      const auto ec =
         type == SOCK_STREAM
            ? impl->socket.emplace<boost::corosio::tcp_socket>(executor.context()).assign(fd)
            : impl->socket.emplace<boost::corosio::udp_socket>(executor.context()).assign(fd);
      if (ec)
         throw system_error(ec, "assign");

      impl_ = std::move(impl);
   }

   ~Descriptor() { detach(); }

   Descriptor(const Descriptor&) = delete;
   Descriptor& operator=(const Descriptor&) = delete;

   bool is_open() const noexcept { return impl_ != nullptr; }

   //
   // The waits that are still pending hold the socket alive until they have been stopped and
   // their tasks are through, which is why the state lives in a shared_ptr of its own.
   //
   void detach()
   {
      if (!impl_)
         return;

      impl_->stop.request_stop();
      std::visit(
         [](auto& socket) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(socket)>, std::monostate>)
               if (socket.is_open())
                  (void)socket.release();
         },
         impl_->socket);

      impl_.reset();
   }

   //
   // The wait is posted before it is even started: c-ares hands us its sockets from inside
   // ares_process_fds(), through the socket state callback, so a wait armed from there would
   // otherwise be started -- and a readiness that holds already completed, and its callback
   // called -- on the stack of that very call. c-ares does not survive being re-entered like
   // that: it closes its connections underneath itself and comes apart.
   //
   // corosio answers "is this socket ready" with a poll() of its own before it parks anything,
   // which is what makes this the common case rather than a rare one, where Boost.Asio only ever
   // completes a wait from its own event loop.
   //
   template <typename F>
   void wait(wait_type type, F&& on_ready)
   {
      launch(
         executor_, impl_->stop.get_token(),
         [](std::shared_ptr<state> impl, wait_type type, std::decay_t<F> on_ready) -> Task<void> {
            co_await yield_now();
            on_ready(co_await impl->wait(type));
         }(impl_, type, std::forward<F>(on_ready)));
   }

private:
   struct state
   {
      //
      // The two socket types have awaitables of their own, so this is a coroutine picking one of
      // them rather than a visit, which would have to come back with a common type.
      //
      Task<error_code> wait(wait_type type)
      {
         const auto what = type == wait_type::read ? boost::corosio::wait_type::read
                                                   : boost::corosio::wait_type::write;

         error_code ec;
         if (auto* tcp = std::get_if<boost::corosio::tcp_socket>(&socket))
            std::tie(ec) = co_await tcp->wait(what);
         else if (auto* udp = std::get_if<boost::corosio::udp_socket>(&socket))
            std::tie(ec) = co_await udp->wait(what);
         else
            co_return errors::canceled; // released underneath us

         //
         // A socket that has failed is ready as far as a wait is concerned: whoever owns it --
         // c-ares, or the TLS session underneath it -- is the one to find the failure out, by
         // reading or writing, and to fail or retry its query over it. corosio reports the error
         // of the socket as the error of the wait, where Boost.Asio reports such a wait as ready,
         // so this is where the two are made to say the same thing.
         //
         if (ec && ec != boost::capy::cond::canceled && ec != std::errc::bad_file_descriptor)
            ec = {};

         co_return ec;
      }

      std::variant<std::monostate, boost::corosio::tcp_socket, boost::corosio::udp_socket> socket;
      std::stop_source stop;
   };

   Executor executor_;
   std::shared_ptr<state> impl_;
};

//
// corosio has a re-armable timer in its detail namespace only; what it offers is a delay, so
// every arm() is a task of its own that the next arm() or cancel() stops.
//
// A delay that has expired may still be queued behind the executor when it is stopped, so the
// task looks at its stop token again once it has resumed: a callback that has been replaced or
// cancelled is never called.
//
class Timer
{
public:
   explicit Timer(const Executor& executor) : executor_(executor) {}
   ~Timer() { cancel(); }

   Timer(const Timer&) = delete;
   Timer& operator=(const Timer&) = delete;

   template <typename F>
   void arm(std::chrono::steady_clock::duration delay, F&& on_expiry)
   {
      cancel();
      stop_.emplace();
      launch(executor_, stop_->get_token(),
             [](std::chrono::steady_clock::duration delay,
                std::function<void()> on_expiry) -> Task<void> {
                auto [ec] = co_await boost::corosio::delay(delay);
                if (!ec && !(co_await boost::capy::this_coro::stop_token).stop_requested())
                   on_expiry();
             }(delay, std::function<void()>(std::forward<F>(on_expiry))));
   }

   void cancel()
   {
      if (stop_)
         stop_->request_stop();

      stop_.reset();
   }

private:
   Executor executor_;
   std::optional<std::stop_source> stop_;
};

inline std::string address_text(const std::array<unsigned char, 4>& bytes)
{
   return boost::corosio::ipv4_address(bytes).to_string();
}

inline std::string address_text(const std::array<unsigned char, 16>& bytes)
{
   return boost::corosio::ipv6_address(bytes).to_string();
}

inline std::optional<Endpoint> make_endpoint(const sockaddr* address, std::size_t length)
{
   if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in))
   {
      sockaddr_in sa{};
      std::memcpy(&sa, address, sizeof(sa));

      boost::corosio::ipv4_address::bytes_type bytes;
      std::memcpy(bytes.data(), &sa.sin_addr, bytes.size());
      return Endpoint(boost::corosio::ipv4_address(bytes), ntohs(sa.sin_port));
   }

   if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6))
   {
      sockaddr_in6 sa{};
      std::memcpy(&sa, address, sizeof(sa));

      boost::corosio::ipv6_address::bytes_type bytes;
      std::memcpy(bytes.data(), &sa.sin6_addr, bytes.size());
      return Endpoint(boost::corosio::ipv6_address(bytes, sa.sin6_scope_id), ntohs(sa.sin6_port));
   }

   return std::nullopt;
}

} // namespace cares_async::detail
