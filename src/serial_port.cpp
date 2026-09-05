#include "serial_port.hpp"

#include <fcntl.h>   // ::open() и флаги O_RDWR, O_NOCTTY, O_NONBLOCK
#include <poll.h>    // ::poll() для ожидания готовности дескриптора
#include <pty.h>     // ::posix_openpt(), ::grantpt(), ::unlockpt(), ::ptsname_r()

#include <cerrno>    // errno и коды ошибок EAGAIN, EINTR, ETIMEDOUT...
#include <utility>   // std::move

namespace atmodem {

int SerialPort::configureRaw(int fd, speed_t baud) noexcept
{
    // Структура с настройками терминала; нулевая инициализация {} обнуляет
    // все поля, чтобы дальше заполнять их осознанно.
    termios tty{};
    // Читаем ТЕКУЩИЕ настройки порта в структуру: менять настройки принято
    // поверх считанных, а не собранных "с нуля".
    if (::tcgetattr(fd, &tty) != 0) {
        return errno; // дескриптор не является терминалом или ошибка ввода-вывода
    }

    // Включаем "сырой" режим одним вызовом: без преобразований перевода
    // строки (CR/LF), без эха драйвера, без сигналов по Ctrl-C и т.п.
    // Эхом и разбором строк управляет сам AT-сервер, а не ядро.
    ::cfmakeraw(&tty);
    // Включаем приёмник, игнорируем состояние управляющих линий модема,
    // чтобы сервер работал даже с "висящей в воздухе" консолью.
    tty.c_cflag |= (CLOCAL | CREAD);
    // 8 бит данных, без чётности.
    tty.c_cflag &= ~static_cast<tcflag_t>(PARENB); // отключаем бит чётности (PARENB)
    tty.c_cflag &= ~static_cast<tcflag_t>(CSTOPB); // один стоповый бит (CSTOPB включил бы два)
    tty.c_cflag &= ~static_cast<tcflag_t>(CSIZE);  // сбрасываем маску размера символа...
    tty.c_cflag |= CS8;                            // ...и задаём 8 бит данных (8N1)
    // Без аппаратного управления потоком.
    tty.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS); // линии RTS/CTS не используем
    // Неблокирующее поведение чтения: read() возвращает всё, что уже пришло,
    // не дожидаясь полного блока данных.
    tty.c_cc[VMIN] = 0;   // минимум байт для возврата из read(): ноль
    tty.c_cc[VTIME] = 0;  // тайм-аут ожидания в децисекундах: ноль (не ждём вовсе)

    // Задаём скорость обмена: отдельно для входящего и исходящего потоков.
    if (::cfsetispeed(&tty, baud) != 0 || ::cfsetospeed(&tty, baud) != 0) {
        return errno;
    }
    // Применяем подготовленные настройки немедленно (TCSANOW), не дожидаясь,
    // пока опустеет очередь передачи.
    if (::tcsetattr(fd, TCSANOW, &tty) != 0) {
        return errno;
    }
    // Сбрасываем накопившиеся входные и выходные данные: после перенастройки
    // в очередях может лежать "мусор" от предыдущего владельца порта.
    ::tcflush(fd, TCIOFLUSH);
    return 0; // успех
}

std::expected<SerialPort, int>
SerialPort::openDevice(const std::string& path, speed_t baud) noexcept
{
    // Открываем устройство на чтение и запись:
    //   O_RDWR     - двунаправленный обмен (команды принимаем, ответы шлём);
    //   O_NOCTTY   - порт НЕ становится управляющим терминалом процесса
    //                (иначе сигналами терминала можно случайно убить сервер);
    //   O_NONBLOCK - неблокирующий режим: open() и последующий read()
    //                возвращаются сразу, даже если данных нет.
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return std::unexpected(errno); // устройства нет / занято / нет прав
    }
    SerialPort port(fd, path); // оборачиваем дескриптор (RAII закроет при ошибке)
    // Настраиваем сырой режим с указанной скоростью.
    if (const int rc = configureRaw(port.fd_.get(), baud); rc != 0) {
        return std::unexpected(rc);
    }
    return port;
}

std::expected<PtyPair, int> SerialPort::openPty(speed_t baud) noexcept
{
    // Создаём псевдотерминал: получаем дескриптор master-стороны (/dev/ptmx).
    int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (master < 0) {
        return std::unexpected(errno);
    }
    // Страховка: если что-то ниже упадёт, guard закроет master в своём
    // деструкторе и дескриптор не "утечёт".
    UniqueFd masterGuard(master);

    // grantpt меняет права на slave-устройство (даёт доступ его будущему
    // владельцу), unlockpt разрешает открывать slave-сторону.
    if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        return std::unexpected(errno);
    }
    // Узнаём путь к slave-устройству вида "/dev/pts/N" - именно его мы
    // сообщаем клиентам для подключения.
    char pathBuf[128] = {};
    if (::ptsname_r(master, pathBuf, sizeof(pathBuf)) != 0) {
        return std::unexpected(errno);
    }
    const std::string slavePath(pathBuf);

    // Открываем slave-сторону (второй конец пары).
    const int slaveFd = ::open(slavePath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (slaveFd < 0) {
        return std::unexpected(errno);
    }
    UniqueFd slaveGuard(slaveFd); // страховка: закроет дескриптор при ошибке ниже
    // Сырой режим на slave: данные клиентов проходят без искажений.
    if (const int rc = configureRaw(slaveFd, baud); rc != 0) {
        return std::unexpected(rc);
    }
    SerialPort slave(slaveFd, slavePath);
    (void)slaveGuard.release(); // владение дескриптором перешло объекту SerialPort

    // Сырой режим и на master: сервер читает клиента как есть.
    if (const int rc = configureRaw(master, baud); rc != 0) {
        return std::unexpected(rc);
    }
    // Отдаём пару наружу: master уходит под контроль вызывающего кода
    // (сервер будет обслуживать именно его), slave - клиентам и тестам.
    return PtyPair{std::move(masterGuard), std::move(slave), slavePath};
}

std::expected<SerialPort, int>
SerialPort::adoptFd(int fd, const std::string& name, speed_t baud) noexcept
{
    // readSome()/writeAll() рассчитаны на неблокирующий режим (обработка
    // EAGAIN), однако принятый извне дескриптор может быть блокирующим:
    // выставляем O_NONBLOCK сами и не полагаемся на вызывающий код.
    UniqueFd guard(fd); // страховка: закроет дескриптор при любой ошибке ниже
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        return std::unexpected(errno);
    }
    if ((flags & O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return std::unexpected(errno);
    }
    if (baud != B0) { // B0 означает "уже настроен, не трогать"
        if (const int rc = configureRaw(fd, baud); rc != 0) {
            return std::unexpected(rc);
        }
    }
    (void)guard.release(); // дескриптор настроен: владение переходит объекту SerialPort
    return SerialPort(fd, name);
}

std::expected<std::size_t, int>
SerialPort::readSome(std::span<char> buffer) const noexcept
{
    // Одна попытка неблокирующего чтения: забираем столько байт, сколько
    // уже доступно (максимум - размер буфера).
    const ssize_t n = ::read(fd_.get(), buffer.data(), buffer.size());
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::size_t{0}; // данных пока нет — это не ошибка
        }
        if (errno == EINTR) {
            return std::size_t{0}; // прервано сигналом: данных нет, вернёмся позже
        }
        return std::unexpected(errno); // реальная ошибка ввода-вывода
    }
    // n >= 0: число реально прочитанных байт (0 - очередь была пуста).
    return static_cast<std::size_t>(n);
}

std::expected<void, int> SerialPort::writeAll(std::string_view data,
                                              const std::atomic_bool* cancel) const noexcept
{
    // Цикл до полной записи: за один write() может уйти лишь часть данных,
    // поэтому после каждой успешной записи сдвигаем начало остатка.
    while (!data.empty()) {
        const ssize_t n = ::write(fd_.get(), data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue; // запись прервана сигналом - просто повторяем
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Буфер ядра переполнен: подписываемся на POLLOUT и ждём,
                // когда дескриптор снова примет данные. Ждём короткими
                // срезами и проверяем флаг отмены, чтобы запись не могла
                // застопорить остановку сервера (Ctrl-C при застрявшем клиенте).
                for (;;) {
                    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                        return std::unexpected(ECANCELED);
                    }
                    pollfd pfd{.fd = fd_.get(), .events = POLLOUT, .revents = 0};
                    const int ready = ::poll(&pfd, 1, 100);
                    if (ready > 0) {
                        break; // дескриптор готов — пробуем записать остаток
                    }
                    if (ready < 0 && errno == EINTR) {
                        continue; // прерваны сигналом — проверим флаг отмены
                    }
                    if (ready < 0) {
                        return std::unexpected(errno); // реальная ошибка poll
                    }
                    // ready == 0: тайм-аут среза — возвращаемся к проверке флага
                }
                continue;
            }
            return std::unexpected(errno); // прочие ошибки (EIO, EBADF...)
        }
        if (n == 0) {
            return std::unexpected(EIO);
        }
        // n > 0: убираем успешно записанное начало и досылаем хвост.
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return {}; // всё записано
}

} // namespace atmodem
