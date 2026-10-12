//
// The library does not log anywhere by itself: whatever it has to say goes to the function that
// resolver::set_logger() was given, and nowhere if there is none.
//
#pragma once

#include <cares_asio/resolver.hpp>

#include <format>
#include <utility>

namespace cares_asio::detail
{

//
// Formats only where somebody is listening.
//
template <typename... Args>
void log(const log_function& logger, log_level level, std::format_string<Args...> format,
         Args&&... args)
{
   if (logger)
      logger(level, std::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void log_debug(const log_function& logger, std::format_string<Args...> format, Args&&... args)
{
   log(logger, log_level::debug, format, std::forward<Args>(args)...);
}

template <typename... Args>
void log_info(const log_function& logger, std::format_string<Args...> format, Args&&... args)
{
   log(logger, log_level::info, format, std::forward<Args>(args)...);
}

template <typename... Args>
void log_error(const log_function& logger, std::format_string<Args...> format, Args&&... args)
{
   log(logger, log_level::error, format, std::forward<Args>(args)...);
}

} // namespace cares_asio::detail
