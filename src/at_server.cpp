#include "at_server.hpp"

#include "log.hpp"
#include "pattern_matcher.hpp"

#include <cerrno>
#include <cstring>
#include <new>
#include <poll.h>

namespace {

/// @brief true, если код ошибки означает, что клиент закрыл соединение (штатная ситуация).
[[nodiscard]] bool isDisconnectError(int error) noexcept
{
    return error == EIO || error == EPIPE || error == ENXIO || error == ENODEV;
}

} // namespace

namespace atmodem {

AtServer::AtServer(CommandDictionary dictionary, SerialPort port,
                   const AtServerOptions& options)
    : dictionary_(std::move(dictionary)),
      port_(std::move(port)),
      options_(options),
      echoEnabled_(options.localEcho),
      assembler_(options.maxLineLength)
{
}

bool AtServer::sendResponse(std::string_view body) noexcept
{
    // Формат ответа по V.250: <CR><LF><текст><CR><LF>
    workBuffer_.clear();
    workBuffer_.reserve(body.size() + 4);
    workBuffer_.push_back('\r');
    workBuffer_.push_back('\n');
    workBuffer_.append(body);
    workBuffer_.push_back('\r');
    workBuffer_.push_back('\n');

    const auto written = port_.writeAll(workBuffer_, stopFlag_);
    if (!written) {
        if (isDisconnectError(written.error())) {
            LOG_INFO("клиент отключился (%s)", port_.name().c_str());
        } else if (written.error() == ECANCELED) {
            // Запись прервана флагом остановки — это не ошибка: run()
            // увидит флаг и завершится штатно.
        } else {
            LOG_ERROR("ошибка записи в %s: %s", port_.name().c_str(), strerror(written.error()));
            ioError_ = true;
        }
        return false;
    }
    return true;
}

void AtServer::applySideEffects(std::string_view command) noexcept
{
    if (patternMatches("ate[01]", command)) {
        echoEnabled_ = (command.back() == '1');
        LOG_DEBUG("эхо переключено: %s", echoEnabled_ ? "вкл" : "выкл");
    }
}

void AtServer::processLine(std::string_view line) noexcept
{
    // Отбрасываем хвостовые NUL и пробелы, которыми клиенты дополняют команды:
    // реальные модемы игнорируют хвостовые пробелы, иначе словарь и побочные
    // эффекты разъехались бы на "ATE1 " и "ATE1".
    while (!line.empty() && (line.back() == '\0' || line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }
    if (line.empty()) {
        return; // "пустая команда" — молча игнорируем
    }

    LOG_DEBUG("команда '%.*s'", static_cast<int>(line.size()), line.data());

    const CommandDictionary::Rule* rule = dictionary_.find(line);
    if (rule != nullptr) {
        // Побочные эффекты применяются только к распознанным командам: иначе
        // незнакомая команда возвращала бы ERROR, но всё равно меняла бы эхо.
        applySideEffects(line);
    } else {
        LOG_INFO("неизвестная команда '%.*s' -> ERROR",
                 static_cast<int>(line.size()), line.data());
    }
    const std::string_view answer = (rule != nullptr) ? rule->answer : std::string_view("ERROR");
    sendResponse(answer);
}

bool AtServer::pollOnce() noexcept
{
    pollfd pfd{.fd = port_.fd(), .events = POLLIN, .revents = 0};
    const int ready = ::poll(&pfd, 1, options_.pollTimeoutMs);
    if (ready < 0) {
        if (errno == EINTR) {
            return true; // прерваны сигналом — проверим флаг остановки снаружи
        }
        LOG_ERROR("poll(%s): %s", port_.name().c_str(), strerror(errno));
        ioError_ = true;
        return false;
    }
    if (ready == 0) {
        return true; // тайм-аут без данных
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (pfd.revents & POLLIN) == 0) {
        LOG_INFO("клиент отключился (%s)", port_.name().c_str());
        return false;
    }

    char chunk[256] = {};
    // Политика при исчерпании памяти: аллокации здесь ограничены
    // maxLineLength байтами, а в установившемся режиме не происходят вовсе
    // (ёмкости уже выделены). Если память всё же кончилась - не роняем сервер,
    // а сбрасываем накопитель строки (частичные данные теряются) и продолжаем
    // обслуживать следующие команды.
    try {
        for (;;) { // вычитываем всё доступное на данный момент
            const auto got = port_.readSome(chunk);
            if (!got) {
                if (isDisconnectError(got.error())) {
                    LOG_INFO("клиент отключился (%s)", port_.name().c_str());
                } else {
                    LOG_ERROR("read(%s): %s", port_.name().c_str(), strerror(got.error()));
                    ioError_ = true;
                }
                return false;
            }
            if (*got == 0) {
                break;
            }

            for (std::size_t i = 0; i < *got; ++i) {
                const FeedResult fr = assembler_.feed(chunk[i]);
                if (fr.echo && echoEnabled_) {
                    const auto echoed = port_.writeAll(fr.echoSeq, stopFlag_);
                    if (!echoed) {
                        if (isDisconnectError(echoed.error())) {
                            LOG_INFO("клиент отключился (%s)", port_.name().c_str());
                        } else if (echoed.error() != ECANCELED) {
                            LOG_ERROR("ошибка записи в %s: %s",
                                      port_.name().c_str(), strerror(echoed.error()));
                            ioError_ = true;
                        }
                        return false; // ECANCELED — штатный останов, не ошибка
                    }
                }
                if (fr.lineCompleted) {
                    const bool overflowed = fr.overflowed;
                    std::string line;
                    if (assembler_.takeLine(line)) {
                        if (overflowed) {
                            LOG_INFO("слишком длинная командная строка -> ERROR");
                            if (!sendResponse("ERROR")) {
                                return false; // ioError_ уже выставлен
                            }
                        } else {
                            processLine(line); // ошибки записи внутри фиксируют ioError_
                        }
                    }
                }
            }
        }
    } catch (const std::bad_alloc&) {
        LOG_ERROR("исчерпана память: текущая командная строка отброшена");
        assembler_.reset();
    }
    return true;
}

int AtServer::run(const std::atomic_bool& stopFlag)
{
    stopFlag_ = &stopFlag; // чтобы writeAll мог прервать запись по флагу
    LOG_INFO("обслуживаю %s, правил словаря: %zu",
             port_.name().c_str(), dictionary_.rules().size());
    while (!stopFlag.load(std::memory_order_relaxed)) {
        if (!pollOnce()) {
            // Отключение клиента (HUP) - штатная ситуация, ошибка - нет.
            return ioError_ ? 1 : 0;
        }
    }
    LOG_INFO("остановка по сигналу");
    return 0;
}

} // namespace atmodem
