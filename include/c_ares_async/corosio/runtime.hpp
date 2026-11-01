// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The runtime layer for capy/corosio: coroutines only, capy::task, capy::any_executor, and
// std::stop_token where ASIO has cancellation slots. See c_ares_async/runtime.hpp for what this is;
// asio/runtime.hpp defines the same names in the same order and has their documentation, and what
// is said here is what differs.
//
#pragma once

#include <boost/capy/continuation.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/ex/await_context.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ip_address.hpp>

#include <coroutine>
#include <functional>
#include <optional>
#include <stop_token>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace c_ares_async
{

/** A coroutine, as the runtime runs it: `boost::capy::task<T>`.

    What @ref resolver::resolve and @ref resolver::resolve_https return,
    and the one spelling of the operations that both runtimes have.
 */
template <typename T = void>
using Task = boost::capy::task<T>;

/** The type of the executor a @ref resolver runs on:
    `boost::capy::any_executor`.
 */
using Executor = boost::capy::any_executor;

/** An IP address, as the runtime has it: `boost::corosio::ip_address`. */
using IpAddress = boost::corosio::ip_address;

/** An address and a port, as the runtime has it:
    `boost::corosio::endpoint`, which serves TCP and UDP alike. What a
    lookup comes back with.
 */
using Endpoint = boost::corosio::endpoint;

/** What went wrong, as the runtime reports it: `std::error_code`. */
using error_code = std::error_code;

/** The base of an error category of the runtime's own:
    `std::error_category`, which @ref ares_category derives from.
 */
using error_category = std::error_category;

/** What the runtime throws an @ref error_code as: `std::system_error`. */
using system_error = std::system_error;

/** The error codes of the API contract that the two runtimes spell
    differently.
 */
namespace errors
{

/** An operation that was cancelled, or whose resolver went away:
    `boost::capy::error::canceled`.
 */
inline const error_code canceled = boost::capy::error::canceled;

} // namespace errors

/** Makes an error code of the system category of the runtime.

    What a call to the operating system failed with, `errno` and the like.
    The values of @ref ares_category overlap with those of `errno`, which is
    what the categories are there to tell apart.

    @param value The value, as `errno` has it.

    @return `value` in the system category, which is `std::system_category()`
    here.
 */
inline error_code make_system_error(int value) noexcept { return {value, std::system_category()}; }

// --------------------------------------------------------------------------------------------------

namespace detail
{

//
// Where a parked operation leaves its result, and how its caller gets resumed. It is part of the
// awaitable that initiate() returns, which lives in the caller's coroutine frame for as long as
// the operation is in progress.
//
template <typename... Args>
struct completion_state
{
   boost::capy::continuation continuation;
   const boost::capy::await_context* env = nullptr;
   std::optional<std::tuple<error_code, Args...>> result;
   std::optional<std::stop_callback<std::function<void()>>> on_stop;
   bool starting = false; // the operation is being started, see initiation::await_suspend()

   //
   // Never resumes the caller inline: it is posted to the caller's executor, which is what ASIO's
   // post() does for the handler there. A c-ares callback runs from inside ares_getaddrinfo() or
   // ares_process_fds(), and resuming the application in there would let it call back into
   // c-ares from inside it.
   //
   void complete(error_code ec, Args... args)
   {
      result.emplace(ec, std::move(args)...);
      on_stop.reset();
      if (!starting)
         env->executor.post(continuation);
   }
};

template <typename Signature>
struct completion_traits;

template <typename... Args>
struct completion_traits<void(error_code, Args...)>
{
   using state = completion_state<Args...>;
   using result = std::tuple<error_code, Args...>;
};

template <typename Signature>
class Completion;

//
// An operation that has been started and not completed yet. Here it refers to the state of the
// awaitable its caller waits on, and invoking it posts the caller's resumption.
//
// One that is destroyed without having been invoked completes as cancelled, so that nobody waits
// for it forever -- which is what a c-ares request whose channel is destroyed comes to.
//
template <typename... Args>
class Completion<void(error_code, Args...)>
{
public:
   using state_type = completion_state<Args...>;

   Completion() noexcept = default;
   explicit Completion(state_type* state) noexcept : state_(state) {}
   Completion(Completion&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}
   Completion& operator=(Completion&& other) noexcept
   {
      if (this != &other)
      {
         abandon();
         state_ = std::exchange(other.state_, nullptr);
      }
      return *this;
   }
   ~Completion() { abandon(); }

   Completion(const Completion&) = delete;
   Completion& operator=(const Completion&) = delete;

   explicit operator bool() const noexcept { return state_ != nullptr; }

   void operator()(error_code ec, Args... args) &&
   {
      std::exchange(state_, nullptr)->complete(ec, std::move(args)...);
   }

   state_type* state() const noexcept { return state_; }

private:
   void abandon() noexcept
   {
      if (state_)
         std::exchange(state_, nullptr)->complete(errors::canceled, Args{}...);
   }

   state_type* state_ = nullptr;
};

//
// Invoking the completion posts by itself, so there is nothing for the executor to do here.
//
template <typename Handler, typename... Args>
inline void complete_later(Handler&& handler, const Executor&, Args&&... args)
{
   if (handler)
      std::move(handler)(std::forward<Args>(args)...);
}

//
// Calls 'on_cancel' when the stop token of whoever waits for the operation is triggered. A stop
// that was requested before the operation started never gets here: initiate() completes such an
// operation as cancelled right away.
//
template <typename Signature, typename F>
inline void on_cancel(Completion<Signature>& handler, F&& on_cancel)
{
   auto* state = handler.state();
   if (!state || !state->env->stop_token.stop_possible())
      return;

   state->on_stop.emplace(state->env->stop_token,
                          std::function<void()>(std::forward<F>(on_cancel)));
}

//
// Completing the operation drops the stop callback, so this is only here to be called in both
// runtimes.
//
template <typename Signature>
inline void clear_on_cancel(Completion<Signature>&)
{
}

template <typename Signature, typename Init>
class initiation
{
   using traits = completion_traits<Signature>;

public:
   explicit initiation(Init init) : init_(std::move(init)) {}

   //
   // capy awaits nothing it cannot move, and moving one of these is only ever done before it is
   // awaited, while its state is still empty: the stop callback in there does not move.
   //
   initiation(initiation&& other) noexcept(std::is_nothrow_move_constructible_v<Init>)
      : init_(std::move(other.init_))
   {
   }

   initiation& operator=(initiation&&) = delete;

   bool await_ready() const noexcept { return false; }

   std::coroutine_handle<> await_suspend(std::coroutine_handle<> h,
                                         const boost::capy::await_context* env)
   {
      state_.continuation.h = h;
      state_.continuation.owner = env->owner;
      state_.env = env;
      if (env->stop_token.stop_requested())
      {
         canceled(std::type_identity<typename traits::result>{});
         return h;
      }

      //
      // An operation that completes while it is being started resumes its caller right away, once
      // the initiation has returned: by then the stack is the caller's own again, with no c-ares
      // call on it. That is what ASIO's awaitables do, too.
      //
      state_.starting = true;
      init_(Completion<Signature>(&state_));
      state_.starting = false;

      return state_.result ? h : std::noop_coroutine();
   }

   typename traits::result await_resume() { return std::move(*state_.result); }

private:
   template <typename... Args>
   void canceled(std::type_identity<std::tuple<error_code, Args...>>)
   {
      state_.result.emplace(errors::canceled, Args{}...);
   }

   Init init_;
   typename traits::state state_;
};

//
// Starts an operation that completes a Completion<Signature>, and waits for it.
//
template <typename Signature, typename Init>
auto initiate(Init&& init)
{
   return initiation<Signature, std::decay_t<Init>>(std::forward<Init>(init));
}

} // namespace detail

} // namespace c_ares_async
