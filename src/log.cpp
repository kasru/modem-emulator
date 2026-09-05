#include "log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace atmodem {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};

/// @brief Текстовая метка уровня для префикса строки журнала.
[[nodiscard]] const char* levelTag(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Error: return "err ";
    case LogLevel::Info: return "info";
    case LogLevel::Debug: return "dbg ";
    }
    return "???  ";
}
} // namespace

void setLogLevel(LogLevel level) noexcept
{
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel logLevel() noexcept
{
    return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

void logMessage(LogLevel level, std::string_view fmt, ...) noexcept
{
    if (static_cast<int>(level) > g_level.load(std::memory_order_relaxed)) {
        return;
    }

    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm tmBuf{};
    localtime_r(&ts.tv_sec, &tmBuf);

    char stamp[32] = {};
    (void)std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03ld",
                        tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec,
                        static_cast<long>(ts.tv_nsec / 1'000'000));

    // std::string_view не гарантирует NUL-терминатор, поэтому копируем формат
    // в стековый буфер и терминируем вручную: прямая передача fmt.data() в
    // vsnprintf была бы UB для "среза" более длинной строки. Копия в стек,
    // а не в std::string: функция noexcept и не должна падать на bad_alloc.
    char format[256] = {};
    const std::size_t capacity = sizeof(format) - 1;
    const bool truncated = fmt.size() > capacity;
    std::size_t fmtLen = std::min(fmt.size(), capacity);
    fmt.copy(format, fmtLen);
    if (truncated) {
        // Усечение не должно разорвать спецификатор printf ("%ld" -> "%l") —
        // поведение vsnprintf с таким форматом формально не определено.
        // Хвостовой одиночный '%' убираем, на место обрезанного хвоста
        // ставим многоточие.
        if (fmtLen > 0 && format[fmtLen - 1] == '%') {
            --fmtLen;
        }
        const char kEllipsis[] = "...";
        const std::size_t ellipsisLen = sizeof(kEllipsis) - 1;
        if (fmtLen + ellipsisLen <= capacity) {
            std::memcpy(format + fmtLen, kEllipsis, ellipsisLen);
            fmtLen += ellipsisLen;
        }
    }
    format[fmtLen] = '\0';

    char body[512] = {};
    va_list args;
    va_start(args, fmt);
    const int bodyLen = std::vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    if (bodyLen < 0) {
        body[sizeof(body) - 1] = '\0'; // vsnprintf не обязан давать результат
    } else if (static_cast<std::size_t>(bodyLen) >= sizeof(body) - 1) {
        // Сообщение обрезано буфером — помечаем многоточием.
        const char kEllipsis[] = "...";
        const std::size_t ellipsisLen = sizeof(kEllipsis) - 1;
        std::memcpy(body + sizeof(body) - 1 - ellipsisLen, kEllipsis, ellipsisLen);
        body[sizeof(body) - 1] = '\0';
    }

    std::fprintf(stderr, "[%s] %s %s\n", stamp, levelTag(level), body);
    std::fflush(stderr);
}

} // namespace atmodem
