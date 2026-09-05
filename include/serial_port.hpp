#pragma once

#include <termios.h>

#include <atomic>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "unique_fd.hpp"

namespace atmodem {

/// @brief Пара дескрипторов псевдотерминала (объявление ниже, после SerialPort).
struct PtyPair;

/**
 * @brief RAII-обёртка над tty-устройством с настройкой терминала.
 *
 * При открытии устройство переводится в "сырой" режим (аналог того, как
 * работает драйвер модема): 8N1, заданная скорость, без программного и
 * аппаратного управления потоком, эхо и преобразования символов отключены
 * (эхом управляет сам AT-сервер). Чтение/запись выполняются в неблокирующем
 * режиме; ожидание готовности делается через poll().
 */
class SerialPort {
public:
    /// @brief Деструктор по умолчанию: UniqueFd закрывает дескриптор.
    ~SerialPort() = default;

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    /// @brief Перемещающий конструктор.
    SerialPort(SerialPort&&) noexcept = default;
    /// @brief Перемещающее присваивание.
    SerialPort& operator=(SerialPort&&) noexcept = default;

    /**
     * @brief Открывает существующее tty-устройство и настраивает его.
     *
     * @param path Путь к устройству (`/dev/ttyUSB0`, `/dev/ttyS0`, `/dev/pts/N`...).
     * @param baud Скорость порта (константа Bxxxx из <termios.h>).
     *
     * @return Готовый порт или errno при ошибке открытия/настройки.
     */
    [[nodiscard]] static std::expected<SerialPort, int>
    openDevice(const std::string& path, speed_t baud = B115200) noexcept;

    /**
     * @brief Создаёт пару псевдотерминалов и настраивает обе стороны.
     *
     * Сервер обслуживает master-сторону (тогда данные клиентов, открывших
     * устройство @p slavePath, приходят ему на вход), а slave-устройство
     * остаётся точкой подключения внешних программ (screen, socat, тесты).
     *
     * @param baud Скорость порта для настройки терминала.
     *
     * @return Пара master/slave или errno при ошибке.
     */
    [[nodiscard]] static std::expected<PtyPair, int> openPty(speed_t baud = B115200) noexcept;

    /**
     * @brief Принимает готовый файловый дескриптор под управление SerialPort.
     *
     * Дескриптор принудительно переводится в неблокирующий режим
     * (O_NONBLOCK): readSome()/writeAll() рассчитаны на семантику EAGAIN.
     *
     * @param fd        Уже открытый дескриптор tty (владение переходит объекту).
     * @param name      Имя устройства для журналов.
     * @param baud      Скорость для настройки сырого режима; B0 - не настраивать.
     *
     * @return Готовый порт или errno при ошибке настройки.
     */
    [[nodiscard]] static std::expected<SerialPort, int>
    adoptFd(int fd, const std::string& name, speed_t baud = B0) noexcept;

    /**
     * @brief Сырой файловый дескриптор устройства.
     *
     * @return Дескриптор (>= 0), пригодный для poll()/read()/write().
     */
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

    /**
     * @brief Имя устройства (для журналов и сообщений пользователю).
     *
     * @return Путь, по которому устройство было открыто.
     */
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /**
     * @brief Неблокирующее чтение доступных данных.
     *
     * @param buffer Буфер для приёма байтов.
     *
     * @return Количество прочитанных байт (0 — данных нет);
     *         errno — при ошибке ввода-вывода.
     */
    [[nodiscard]] std::expected<std::size_t, int>
    readSome(std::span<char> buffer) const noexcept;

    /**
     * @brief Записывает данные целиком, при необходимости ожидая готовности
     *        через poll(POLLOUT) и досылая остаток.
     *
     * Ожидание готовности выполняется короткими срезами и прерывается, если
     * установлен @p cancel (например, флаг остановки сервера): так запись
     * не сможет застопорить штатное завершение по Ctrl-C.
     *
     * @param data   Записываемые байты.
     * @param cancel Атомарный флаг отмены (может быть nullptr).
     *
     * @return Пустой результат при успехе; errno — при ошибке записи;
     *         ECANCELED — если запись прервана по флагу @p cancel.
     */
    [[nodiscard]] std::expected<void, int> writeAll(std::string_view data,
                                                    const std::atomic_bool* cancel = nullptr) const noexcept;

private:
    /**
     * @brief Приватный конструктор: объекты создаются фабриками openDevice/openPty.
     *
     * @param fd   Уже открытый дескриптор устройства.
     * @param name Имя устройства для журналов.
     */
    SerialPort(int fd, std::string name) noexcept : fd_(fd), name_(std::move(name)) {}

    /**
     * @brief Настраивает терминал в сырой режим с заданной скоростью.
     *
     * @param fd   Дескриптор tty.
     * @param baud Требуемая скорость (константа Bxxxx).
     *
     * @return 0 при успехе, иначе errno.
     */
    [[nodiscard]] static int configureRaw(int fd, speed_t baud) noexcept;

    UniqueFd fd_;      ///< Владелец файлового дескриптора.
    std::string name_; ///< Имя устройства.
};

/**
 * @brief Пара дескрипторов псевдотерминала (master/slave).
 *
 * Используется, когда реального tty-устройства нет: сервер обслуживает
 * master-сторону, а клиенты подключаются к устройству, путь к которому
 * возвращает поле @p slavePath (например, `/dev/pts/4`).
 */
struct PtyPair {
    UniqueFd master;       ///< Управляющая сторона PTY — её обслуживает сервер.
    SerialPort slave;      ///< Подчинённая сторона PTY (для клиентов и тестов).
    std::string slavePath; ///< Путь к устройству slave-стороны.
};

} // namespace atmodem
