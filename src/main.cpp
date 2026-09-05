#include "at_server.hpp"
#include "command_dictionary.hpp"
#include "log.hpp"
#include "serial_port.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <termios.h>
#include <unistd.h> // ::readlink() для /proc/self/exe

namespace {

std::atomic_bool g_stopRequested{false}; ///< Флаг остановки, выставляется сигналами.
bool g_startEchoOff = false;             ///< true, если задан ключ --no-echo.

/**
 * @brief Обработчик сигналов завершения: только установка атомарного флага.
 *
 * @param signal Номер сигнала (не используется).
 */
extern "C" void handleStopSignal(int /*signal*/) noexcept
{
    g_stopRequested.store(true);
}

/**
 * @brief Устанавливает обработчики SIGINT/SIGTERM.
 */
void installSignalHandlers()
{
    struct sigaction sa {};
    sa.sa_handler = handleStopSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // без SA_RESTART: poll() должен прерваться для проверки флага
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
}

/// @brief Параметры командной строки.
struct Options {
    std::string devicePath;                    ///< Путь к tty (пусто => создать PTY).
    std::optional<std::string> dictionaryPath; ///< Явный путь к файлу словаря.
    speed_t baud = B115200;                    ///< Скорость порта.
    int baudNumber = 115200;                   ///< Числовая скорость (для журнала).
};

/**
 * @brief Печатает справку по использованию.
 *
 * @param progName Имя исполняемого файла (argv[0]).
 */
void printUsage(const char* progName)
{
    std::fprintf(stderr,
                 "Использование: %s [ПАРАМЕТРЫ]\n"
                 "Сервер, эмулирующий модем с AT-командами на tty-устройстве.\n\n"
                 "  -d, --device ПУТЬ   обслуживать указанное tty-устройство\n"
                 "                      (по умолчанию создаётся псевдотерминал /dev/pts/N)\n"
                 "  -f, --file ПУТЬ     файл словаря 'expect=answer'\n"
                 "                      (по умолчанию ищется рядом с программой)\n"
                 "  -b, --baud ЧИСЛО    скорость порта: 9600|19200|38400|57600|115200|230400\n"
                 "                      (по умолчанию 115200)\n"
                 "      --no-echo       стартовать с выключенным эхом (эквивалент ATE0)\n"
                 "  -v                  подробный журнал\n"
                 "  -h, --help          показать эту справку\n",
                 progName);
}

/**
 * @brief Преобразует числовую скорость в константу termios.
 *
 * @param baud Числовая скорость порта.
 *
 * @return Константа Bxxxx или std::nullopt, если скорость не поддерживается.
 */
std::optional<speed_t> toSpeedT(int baud) noexcept
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default: return std::nullopt;
    }
}

/**
 * @brief Разбирает аргументы командной строки.
 *
 * @param argc Число аргументов.
 * @param argv Массив аргументов.
 * @param[out] opts Заполненные параметры.
 *
 * @return true при успехе; false при ошибке разбора или запросе справки.
 */
bool parseArgs(int argc, char* argv[], Options& opts)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto nextValue = [&]() -> std::optional<std::string> {
            if (i + 1 >= argc) {
                return std::nullopt;
            }
            return argv[++i];
        };

        if (arg == "-d" || arg == "--device") {
            if (auto value = nextValue()) {
                opts.devicePath = *value;
            } else {
                LOG_ERROR("после %s ожидается путь к устройству", arg.c_str());
                return false;
            }
        } else if (arg == "-f" || arg == "--file") {
            if (auto value = nextValue()) {
                opts.dictionaryPath = *value;
            } else {
                LOG_ERROR("после %s ожидается путь к файлу словаря", arg.c_str());
                return false;
            }
        } else if (arg == "-b" || arg == "--baud") {
            auto value = nextValue();
            if (!value) {
                LOG_ERROR("после %s ожидается число", arg.c_str());
                return false;
            }
            // from_chars с проверкой полного потребления отклоняет мусор
            // вроде "115200abc" или пустую строку (чего atoi бы не заметил).
            int baud = 0;
            const auto res = std::from_chars(value->data(), value->data() + value->size(), baud);
            if (res.ec != std::errc() || res.ptr != value->data() + value->size()) {
                LOG_ERROR("некорректная скорость: %s", value->c_str());
                return false;
            }
            if (const auto speed = toSpeedT(baud)) {
                opts.baud = *speed;
                opts.baudNumber = baud;
            } else {
                LOG_ERROR("неподдерживаемая скорость: %d", baud);
                return false;
            }
        } else if (arg == "--no-echo") {
            g_startEchoOff = true;
        } else if (arg == "-v") {
            atmodem::setLogLevel(atmodem::LogLevel::Debug);
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return false;
        } else {
            LOG_ERROR("неизвестный параметр: %s", arg.c_str());
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

/**
 * @brief Возвращает путь к исполняемому файлу текущего процесса.
 *
 * Надёжный источник на Linux - символьная ссылка /proc/self/exe: её
 * разрешает ядро, поэтому результат не зависит ни от текущего каталога,
 * ни от способа запуска (относительный путь, поиск по $PATH, симлинк).
 *
 * @return Путь к исполняемому файлу; std::nullopt, если ссылку прочитать
 *         не удалось (например, /proc не смонтирован).
 */
std::optional<std::filesystem::path> executablePath()
{
    char buffer[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        return std::nullopt;
    }
    return std::filesystem::path(buffer, buffer + n);
}

/**
 * @brief Ищет файл словаря в стандартных местах.
 *
 * Сначала пробуется каталог рядом с исполняемым файлом (см. executablePath,
 * при недоступности - argv[0]), затем текущий каталог и системные пути.
 *
 * @param exeName Имя исполняемого файла (argv[0]) - запасной вариант поиска.
 *
 * @return Первый существующий путь или std::nullopt.
 */
std::optional<std::string> locateDefaultDictionary(const char* exeName)
{
    namespace fs = std::filesystem;

    fs::path exeDir;
    if (const auto self = executablePath()) {
        exeDir = self->parent_path();
    } else if (exeName != nullptr) {
        exeDir = fs::absolute(exeName).parent_path();
    }

    std::vector<fs::path> candidates;
    if (!exeDir.empty()) {
        candidates.push_back(exeDir / "../config/modem_commands.csv");
        candidates.push_back(exeDir / "../share/atmodem/modem_commands.csv");
    }
    candidates.push_back("config/modem_commands.csv");
    candidates.push_back("modem_commands.csv");
    candidates.push_back("/etc/atmodem/modem_commands.csv");

    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            return candidate.string();
        }
    }
    return std::nullopt;
}

/**
 * @brief Загружает словарь согласно опциям запуска.
 *
 * @param options Параметры командной строки.
 * @param exeName Имя исполняемого файла.
 *
 * @return Загруженный словарь; при невозможности загрузить — программа
 *         завершается внутри функции.
 */
atmodem::CommandDictionary loadDictionary(const Options& options, const char* exeName)
{
    using atmodem::CommandDictionary;

    std::string path = options.dictionaryPath.value_or("");
    if (!options.dictionaryPath) {
        if (auto found = locateDefaultDictionary(exeName)) {
            path = *found;
        }
    }

    if (!path.empty()) {
        auto loaded = CommandDictionary::loadFromFile(path);
        if (loaded.has_value()) {
            LOG_INFO("словарь: %s (%zu правил)", path.c_str(), loaded->rules().size());
            return std::move(*loaded);
        }
        LOG_ERROR("%s", loaded.error().c_str());
        if (options.dictionaryPath.has_value()) {
            // Явно указанный файл обязан загрузиться.
            std::exit(1);
        }
    }

    LOG_ERROR("перехожу на встроенный минимальный словарь");
    auto fallback = CommandDictionary::loadBuiltin();
    if (!fallback.has_value()) { // корректность встроенного набора покрыта тестами
        LOG_ERROR("%s", fallback.error().c_str());
        std::exit(1);
    }
    return std::move(*fallback);
}

} // namespace

int main(int argc, char* argv[])
{
    using namespace atmodem;

    Options options;
    if (!parseArgs(argc, argv, options)) {
        return 2;
    }
    installSignalHandlers();

    CommandDictionary dictionary = loadDictionary(options, argv[0]);

    // ----- устройство и запуск сервера -----
    std::optional<SerialPort> device;
    std::optional<std::string> deviceName;
    // В PTY-режиме сервер обслуживает master, а slave держим открытым
    // до конца работы процесса: пока жив хотя бы один дескриптор slave,
    // клиент не получает ложный HUP на master. Следствие: в этом режиме
    // POLLHUP на master недостижим, и путь "клиент отключился -> exit 0"
    // срабатывает только в режиме -d (реальное устройство).
    std::optional<SerialPort> slaveHold;

    if (options.devicePath.empty()) {
        auto pair = SerialPort::openPty(options.baud);
        if (!pair) {
            LOG_ERROR("posix_openpt: %s", strerror(pair.error()));
            return 1;
        }
        deviceName = pair->slavePath;
        LOG_INFO("слушаю псевдотерминал %s (%d 8N1)",
                 pair->slavePath.c_str(), options.baudNumber);
        LOG_INFO("подключитесь: screen %s либо socat - %s",
                 pair->slavePath.c_str(), pair->slavePath.c_str());
        // Сервер обслуживает master-сторону: тогда данные клиентов,
        // открывших slave-устройство, попадают прямо к нам.
        auto master = SerialPort::adoptFd(pair->master.release(), *deviceName, options.baud);
        if (!master) {
            LOG_ERROR("настройка master: %s", strerror(master.error()));
            return 1;
        }
        device = std::move(*master);
        slaveHold = std::move(pair->slave); // держим slave открытым (см. выше)
    } else {
        auto port = SerialPort::openDevice(options.devicePath, options.baud);
        if (!port) {
            LOG_ERROR("открытие %s: %s", options.devicePath.c_str(), strerror(port.error()));
            return 1;
        }
        deviceName = port->name();
        device = std::move(*port);
        LOG_INFO("слушаю устройство %s (%d 8N1)", deviceName->c_str(), options.baudNumber);
    }

    AtServerOptions serverOptions{!g_startEchoOff};
    AtServer server(std::move(dictionary), std::move(*device), serverOptions);

    const int rc = server.run(g_stopRequested);

    LOG_INFO("сервер завершил работу (код %d)", rc);
    return rc;
}
