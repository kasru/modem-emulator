#pragma once

#include <string_view>

namespace atmodem {

/// @brief Уровни детализации журнала сервера.
enum class LogLevel : int {
    Error = 0, ///< Серьёзные ошибки.
    Info  = 1, ///< Основные события (по умолчанию).
    Debug = 2, ///< Подробная трассировка (включается ключом -v).
};

/**
 * @brief Задаёт максимальный уровень сообщений, выводимых в stderr.
 *
 * @param level Новый уровень журналирования.
 */
void setLogLevel(LogLevel level) noexcept;

/**
 * @brief Текущий уровень журналирования.
 *
 * @return Активный уровень.
 */
[[nodiscard]] LogLevel logLevel() noexcept;

/**
 * @brief Выводит сообщение в журнал (stderr) с меткой уровня и временем.
 *
 * @param level Уровень сообщения; сообщения выше текущего уровня отбрасываются.
 * @param fmt   Строка в формате printf. Может быть произвольным
 *              std::string_view (не обязательно NUL-терминированным);
 *              слишком длинный формат усекается до 255 символов.
 * @param args  Аргументы формата.
 */
void logMessage(LogLevel level, std::string_view fmt, ...) noexcept;

/** @brief Макрос удобного вызова журнала с автоматическим уровнем. */
#define LOG_ERROR(...) ::atmodem::logMessage(::atmodem::LogLevel::Error, __VA_ARGS__)
/** @brief Информационное сообщение журнала. */
#define LOG_INFO(...) ::atmodem::logMessage(::atmodem::LogLevel::Info, __VA_ARGS__)
/** @brief Отладочное сообщение журнала (видно только при -v). */
#define LOG_DEBUG(...) ::atmodem::logMessage(::atmodem::LogLevel::Debug, __VA_ARGS__)

} // namespace atmodem
