#pragma once

#include <atomic>
#include <string>
#include <string_view>

#include "command_dictionary.hpp"
#include "line_assembler.hpp"
#include "serial_port.hpp"

namespace atmodem {

/**
 * @brief Параметры AT-сервера.
 */
struct AtServerOptions {
    const bool localEcho = true;          ///< Начальное состояние эха (как ATE1 у модема).
    const std::size_t maxLineLength = 128; ///< Максимальная длина командной строки.
    const int pollTimeoutMs = 200;        ///< Тайм-аут ожидания данных в одном цикле poll().
};

/**
 * @brief Сервер, эмулирующий типичный модем с AT-командами на tty-устройстве.
 *
 * Цикл работы:
 *  1. poll() ждёт данные от клиента (с тайм-аутом, чтобы проверять флаг
 *     остановки);
 *  2. прочитанные байты скармливаются LineAssembler'у: он возвращает эхо,
 *     обрабатывает редактирование (BS/DEL/CAN) и выделяет командные строки;
 *  3. завершённая строка ищется в CommandDictionary (первое совпадение по
 *     ограниченному синтаксису регулярных выражений);
 *  4. ответ отправляется клиенту в формате V.250:
 *     `CR LF <ответ> CR LF`; при отсутствии совпадения — `ERROR`.
 *
 * Особые команды, меняющие состояние модема, обрабатываются сервером
 * дополнительно к словарю: `ATE0` выключает эхо, `ATE1` — включает
 * (регистр не важен).
 */
class AtServer {
public:
    /**
     * @brief Создаёт сервер поверх открытого tty-устройства.
     *
     * @param dictionary Словарь "ожидание = ответ".
     * @param port       Открытое устройство (владение перемещается в сервер).
     * @param options    Параметры поведения (эхо, лимиты, тайм-ауты).
     */
    AtServer(CommandDictionary dictionary, SerialPort port,
             const AtServerOptions& options = {});

    /**
     * @brief Главный цикл сервера.
     *
     * Работает до установки флага остановки, отключения клиента или ошибки
     * ввода-вывода на порту.
     *
     * @param stopFlag Атомарный флаг: true означает запрос на остановку
     *                 (обычно выставляется обработчиком SIGINT/SIGTERM).
     *
     * @return 0 при штатном завершении (сигнал или отключение клиента),
     *         1 при ошибке ввода-вывода.
     */
    int run(const std::atomic_bool& stopFlag);

    /**
     * @brief Выполняет один цикл poll + обработка данных.
     *
     * Публичный метод нужен для управляемого тестирования без процессов:
     * тест сам вызывает циклы и проверяет ответы на другой стороне PTY.
     *
     * Политика при нехватке памяти (std::bad_alloc из накопителя строки):
     * текущая командная строка сбрасывается, сервер продолжает работу.
     *
     * @return true — можно продолжать работу; false — ошибка порта
     *         (клиент закрылся, ошибка чтения/записи).
     */
    [[nodiscard]] bool pollOnce() noexcept;

    /**
     * @brief Доступ к порту сервера (для тестов и журналирования).
     *
     * @return Константная ссылка на объект порта.
     */
    [[nodiscard]] const SerialPort& port() const noexcept { return port_; }

    /**
     * @brief Текущее состояние эха.
     *
     * @return true, если эхо включено (эквивалент ATE1).
     */
    [[nodiscard]] bool echoEnabled() const noexcept { return echoEnabled_; }

private:
    /**
     * @brief Обрабатывает одну завершённую командную строку.
     *
     * @param line Команда без терминатора CR.
     */
    void processLine(std::string_view line) noexcept;

    /**
     * @brief Отправляет ответ в обрамлении `CR LF ... CR LF`.
     *
     * @param body Текст ответа (например, `OK`, многострочный текст ATI).
     */
    bool sendResponse(std::string_view body) noexcept;

    /**
     * @brief Обрабатывает побочные эффекты известных команд (ATE0/ATE1).
     *
     * @param command Командная строка (регистр не важен).
     */
    void applySideEffects(std::string_view command) noexcept;

    CommandDictionary dictionary_; ///< Словарь ожиданий и ответов.
    SerialPort port_;              ///< Обслуживаемое устройство.
    AtServerOptions options_;      ///< Неизменяемая конфигурация сервера.
    bool echoEnabled_ = false;     ///< Текущее состояние эха (меняется ATE0/ATE1).
    LineAssembler assembler_;      ///< Сборщик командных строк.
    std::string workBuffer_;       ///< Буфер переиспользуемой памяти для строк.
    // Указатель на флаг остановки: выставляется в run(); передаётся в
    // writeAll(), чтобы запись не блокировала завершение по сигналу.
    const std::atomic_bool* stopFlag_ = nullptr;
    bool ioError_ = false;         ///< true, если остановка вызвана ошибкой ввода-вывода.
};

} // namespace atmodem
