#pragma once

#include <unistd.h>

#include <string>
#include <utility>

namespace atmodem {

/**
 * @brief RAII-обёртка над файловым дескриптором POSIX.
 *
 * Владеет дескриптором единолично: закрытие выполняется в деструкторе.
 * Копирование запрещено, перемещение разрешено (дескриптор переезжает,
 * у источника остаётся -1).
 */
class UniqueFd {
public:
    /// @brief Создаёт пустую обёртку (дескриптор -1).
    UniqueFd() noexcept = default;

    /**
     * @brief Принимает владение существующим дескриптором.
     *
     * @param fd Файловый дескриптор или -1 для "пустой" обёртки.
     */
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    /// @brief Закрывает принадлежащий дескриптор (если открыт).
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    /// @brief Перемещающий конструктор: забирает дескриптор источника.
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    /// @brief Перемещающее присваивание: закрывает свой дескриптор и забирает чужой.
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }    /**
     * @brief Доступ к сырому дескриптору.
     *
     * @return Значение дескриптора или -1, если он не открыт.
     */
    [[nodiscard]] int get() const noexcept { return fd_; }

    /**
     * @brief Проверяет, открыт ли дескриптор.
     *
     * @return true, если дескриптор валиден (>= 0).
     */
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    /**
     * @brief Закрывает дескриптор (повторный вызов безопасен).
     */
    void reset() noexcept
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    /**
     * @brief Отпускает дескриптор без закрытия (возврат владения вызывающему).
     *
     * @return Значение освобождённого дескриптора (-1, если был пуст).
     */
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

private:
    int fd_ = -1; ///< Принадлежащий обёртке файловый дескриптор.
};

} // namespace atmodem
