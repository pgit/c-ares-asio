// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <chrono>
#include <cstddef>

#if !defined(GITHUB_ACTIONS) && !defined(NDEBUG)
#include <print>
#endif

// =================================================================================================

//
// Similar to the run() of the event loop, and returns the same count of completions run, but
// prints a horizontal ruler per completion, highlighting the ones that took more than 10ms. The
// time measured spans the completion's own execution as well as the idle wait before it.
//
// Under NDEBUG this is plain run() and prints nothing.
//
// A template only because the event loop is the one of whichever runtime this was built on, and
// runtime.hpp is what knows which: it includes this, not the other way round. Both have run_one().
//
template <typename Context>
std::size_t run(Context& context)
{
#if defined(GITHUB_ACTIONS) || defined(NDEBUG)
   return context.run();
#else
   using namespace std::chrono;

   std::size_t i = 0;
   auto t0 = steady_clock::now();
   for (i = 0; context.run_one(); ++i)
   {
      auto t1 = steady_clock::now();
      auto dt = duration_cast<milliseconds>(t1 - t0);
      t0 = t1;
      // clang-format off
      if (dt < 10ms)
         std::println("--- {} ------------------------------------------------------------------------", i);
      else
         std::println("\x1b[1;31m--- {} ({}) ----------------------------------------------------------------\x1b[0m", i, dt);
      // clang-format on
   }

   return i;
#endif
}

// =================================================================================================
