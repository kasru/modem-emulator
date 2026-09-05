/**
 * @file test_log.cpp
 * @brief Модульные тесты журналирования (log.hpp / log.cpp).
 */

#include "test_framework.hpp"

#include "log.hpp"

#include <string>

namespace {

/// @brief Формат в виде std::string_view без NUL-терминатора не должен
///        приводить к UB: logMessage обязан скопировать его в свой буфер.
MINITEST_TEST(log_non_terminated_format_view)
{
    // Точный срез кучи: если реализация передаст fmt.data() прямо в
    // vsnprintf, ASan поймает чтение за границей аллокации.
    std::string heap = "heap format %d";
    const std::string_view view{heap.data(), heap.size()};

    atmodem::setLogLevel(atmodem::LogLevel::Debug);
    atmodem::logMessage(atmodem::LogLevel::Error, view, 42);
}

/// @brief Слишком длинный формат усекается и не портит стек.
MINITEST_TEST(log_long_format_truncated)
{
    std::string longFmt(1000, 'x');
    atmodem::setLogLevel(atmodem::LogLevel::Error);
    atmodem::logMessage(atmodem::LogLevel::Error, longFmt);
}

} // namespace
