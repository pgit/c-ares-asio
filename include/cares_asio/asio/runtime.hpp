// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The runtime layer for Boost.Asio: completion tokens, asio::awaitable, any_io_executor. See
// cares_asio/runtime.hpp for what this is, and corosio/runtime.hpp for the other runtime, which
// defines the same names in the same order.
//
#pragma once

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/append.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/error_category.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <utility>

namespace cares_asio
{

/** A coroutine, as the runtime runs it: `boost::asio::awaitable<T>`.

    What @ref resolver::resolve and @ref resolver::resolve_https return,
    and the one spelling of the operations that both runtimes have.
 */
template <typename T = void>
using Task = boost::asio::awaitable<T>;

/** The type of the executor a @ref resolver runs on:
    `boost::asio::any_io_executor`.
 */
using Executor = boost::asio::any_io_executor;

/** An IP address, as the runtime has it: `boost::asio::ip::address`. */
using IpAddress = boost::asio::ip::address;

/** An address and a port, as the runtime has it:
    `boost::asio::ip::tcp::endpoint`. What a lookup comes back with.
 */
using Endpoint = boost::asio::ip::tcp::endpoint;

/** What went wrong, as the runtime reports it:
    `boost::system::error_code`.
 */
using error_code = boost::system::error_code;

/** The base of an error category of the runtime's own:
    `boost::system::error_category`, which @ref ares_category derives
    from.
 */
using error_category = boost::system::error_category;

/** What the runtime throws an @ref error_code as:
    `boost::system::system_error`.
 */
using system_error = boost::system::system_error;

/** The error codes of the API contract that the two runtimes spell
    differently.
 */
namespace errors
{

/** An operation that was cancelled, or whose resolver went away:
    `boost::asio::error::operation_aborted`.
 */
inline const error_code canceled = boost::asio::error::operation_aborted;

} // namespace errors

/** Makes an error code of the system category of the runtime.

    What a call to the operating system failed with, `errno` and the like.
    The values of @ref ares_category overlap with those of `errno`, which is
    what the categories are there to tell apart.

    @param value The value, as `errno` has it.

    @return `value` in the system category, which is
    `boost::system::system_category()` here.
 */
inline error_code make_system_error(int value) noexcept
{
   return {value, boost::system::system_category()};
}

// --------------------------------------------------------------------------------------------------

namespace detail
{

//
// An operation that has been started and not completed yet, waiting to be completed with a
// 'Signature' of void(error_code, ...). The resolver parks one of these per lookup, in the
// request it hands to c-ares, and completes it from the c-ares callback.
//
template <typename Signature>
using Completion = boost::asio::any_completion_handler<Signature>;

//
// Completes 'handler' from 'executor's queue, after whatever is running now has returned. That is
// how a c-ares callback completes a lookup: c-ares calls it from inside ares_getaddrinfo() or
// ares_process_fds(), and resuming the application in there would let it call back into c-ares
// from inside it.
//
template <typename Handler, typename... Args>
inline void complete_later(Handler&& handler, const Executor& executor, Args&&... args)
{
   boost::asio::post(
      executor, boost::asio::append(std::forward<Handler>(handler), std::forward<Args>(args)...));
}

//
// Calls 'on_cancel' when whoever started the operation that 'handler' stands for cancels it.
// Does nothing where the caller has no way of cancelling.
//
template <typename Signature, typename F>
inline void on_cancel(Completion<Signature>& handler, F&& on_cancel)
{
   auto slot = boost::asio::get_associated_cancellation_slot(handler);
   if (slot.is_connected())
      slot.assign([on_cancel = std::forward<F>(on_cancel)](
                     boost::asio::cancellation_type_t) mutable { on_cancel(); });
}

//
// Takes that back, for an operation that is about to complete: a cancellation arriving afterwards
// has nothing left to cancel, and would take the lookups that are still pending with it.
//
template <typename Signature>
inline void clear_on_cancel(Completion<Signature>& handler)
{
   if (auto slot = boost::asio::get_associated_cancellation_slot(handler); slot.is_connected())
      slot.clear();
}

//
// Starts an operation that completes a Completion<Signature>, and waits for it, yielding the
// completion's arguments as a tuple:
//
//    auto [ec, endpoints] = co_await initiate<resolve_signature>(
//       [&](Completion<resolve_signature> handler) { start_resolve(..., std::move(handler)); });
//
// The initiating function runs when the operation is awaited, so it may capture by reference.
//
template <typename Signature, typename Init>
auto initiate(Init&& init)
{
   return boost::asio::async_initiate<const boost::asio::as_tuple_t<boost::asio::deferred_t>&,
                                      Signature>(
      [init = std::forward<Init>(init)](Completion<Signature> handler) mutable {
         init(std::move(handler));
      },
      boost::asio::as_tuple(boost::asio::deferred));
}

} // namespace detail

} // namespace cares_asio
