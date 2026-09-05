/**
 * @file test_at_server_pty.cpp
 * @brief Интеграционные тесты AtServer поверх реального псевдотерминала.
 *
 * Сервер работает на slave-стороне PTY, тест выступает в роли DTE на
 * master-стороне: отправляет команды и сверяет точные байтовые
 * транскрипты (эхо + ответы в обрамлении CR/LF).
 */

#include "test_framework.hpp"

#include "at_server.hpp"
#include "command_dictionary.hpp"

#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <optional>
#include <string>
#include <string_view>

using namespace atmodem;

namespace {

/// @brief Базовый словарь для интеграционных тестов (обе формы записи правил).
constexpr std::string_view kIntegrationDict =
    "AT=OK\n"
    "ATE[01]=OK\n"
    "ATI=Manufacturer: TestModems\\r\\nModel: TM-1\\r\\nRevision: 9.9\\r\\nOK\n"
    "\"AT+COPS=?\",\"+COPS: (2,\"\"TestNet\"\",\"\"25099\"\",7)\\r\\nOK\"\n"
    "AT+COPS?=+COPS: 0,0,\"TestNet\",7\\r\\nOK\n"
    "AT+CPIN?=+CPIN: READY\\r\\nOK\n"
    "AT+CPIN\\=[0-9][0-9][0-9][0-9]=PIN ACCEPTED\n";

/**
 * @brief Тестовый стенд: пара PTY + работающий сервер на slave-стороне.
 */
struct PtyHarness {
    PtyPair pair;                        ///< Пара дескрипторов псевдотерминала.
    std::optional<AtServer> server;      ///< Сервер поверх slave-стороны.
    int master = -1;                     ///< Сырой дескриптор master-стороны.

    /**
     * @brief Создаёт стенд с базовым словарём и выбранным состоянием эха.
     *
     * @param echo Начальное состояние эха сервера.
     *
     * @return Готовый стенд.
     */
    static PtyHarness make(bool echo = true)
    {
        auto pairResult = SerialPort::openPty();
        CHECK(pairResult.has_value());

        PtyHarness harness{std::move(*pairResult), std::nullopt, -1};
        harness.master = harness.pair.master.get();

        auto dict = CommandDictionary::loadFromText(kIntegrationDict, "test");
        CHECK(dict.has_value());

        AtServerOptions opts{echo, 128, 50};
        harness.server.emplace(std::move(*dict), std::move(harness.pair.slave), opts);
        return harness;
    }

    /**
     * @brief Пишет данные в master-сторону (как это делал бы DTE).
     *
     * @param data Отправляемые байты.
     */
    void send(std::string_view data) const
    {
        const ssize_t n = ::write(master, data.data(), data.size());
        CHECK_EQ(n, static_cast<ssize_t>(data.size()));
    }

    /**
     * @brief Читает доступные байты из master-стороны с ожиданием.
     *
     * @param timeoutMs Сколько миллисекунд ждать готовность данных.
     *
     * @return Прочитанные байты (может быть пусто по тайм-ауту).
     */
    [[nodiscard]] std::string readAvailable(int timeoutMs) const
    {
        std::string out;
        pollfd pfd{.fd = master, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, timeoutMs) > 0) {
            char buf[512] = {};
            const ssize_t n = ::read(master, buf, sizeof(buf));
            if (n > 0) {
                out.append(buf, static_cast<std::size_t>(n));
            }
        }
        return out;
    }
};

/// @brief Монотонное время в миллисекундах от старта процесса.
[[nodiscard]] long long nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/**
 * @brief Отправляет команду и собирает ответ до появления ожидаемого фрагмента.
 *
 * Между ожиданиями прокручивает цикл сервера pollOnce(), имитируя его работу.
 *
 * @param harness      Тестовый стенд.
 * @param command      Полная командная строка с терминатором CR.
 * @param expectNeedle Фрагмент, появление которого означает завершение обмена.
 *
 * @return Всё, что пришло по линии с момента отправки команды (эхо + ответ).
 */
std::string exchange(PtyHarness& harness, std::string_view command,
                     std::string_view expectNeedle)
{
    harness.send(command);
    std::string received;
    const long long deadline = nowMs() + 4000;
    while (received.find(expectNeedle) == std::string::npos) {
        CHECK(nowMs() < deadline); // защита от зависания теста
        CHECK(harness.server->pollOnce()); // сервер обязан оставаться живым
        received += harness.readAvailable(5);
    }
    return received;
}

} // namespace

/// @brief adoptFd() принудительно переводит дескриптор в неблокирующий режим.
///
/// Регрессия: readSome()/writeAll() рассчитаны на семантику EAGAIN, но
/// принятый извне блокирующий fd (здесь - обычный pipe) раньше оставался
/// бы блокирующим, и readSome() завис бы навсегда.
MINITEST_TEST(adopt_fd_forces_nonblocking)
{
    int fds[2] = {-1, -1};
    CHECK(::pipe(fds) == 0);

    {
        auto port = SerialPort::adoptFd(fds[0], "pipe-under-test", B0);
        CHECK(port.has_value());

        const int flags = ::fcntl(port->fd(), F_GETFL);
        CHECK(flags >= 0);
        CHECK((flags & O_NONBLOCK) != 0);

        // Пустой канал: неблокирующее чтение обязано вернуть 0 байт,
        // а не заблокировать тестовый поток.
        char buffer[8] = {};
        auto got = port->readSome(buffer);
        CHECK(got.has_value());
        CHECK_EQ(got.value_or(0), std::size_t{0});
    } // порт закрывает дескриптор чтения

    ::close(fds[1]);
}

/// @brief Полный транскрипт обмена при включённом эхе и переключение ATE0/ATE1.
///
/// Проверяется точный порядок байт (эхо команды, затем ответ) и то, что
/// ATE0 отключает эхо для последующего ввода, а ATE1 включает обратно.
MINITEST_TEST(server_pty_echo_on_full_transcript)
{
    PtyHarness h = PtyHarness::make(/*echo=*/true);

    // Эхо включено: клиент получает и отражённую команду, и ответ.
    CHECK_EQ(exchange(h, "AT\r", "\r\nOK\r\n"), std::string("AT\r\r\nOK\r\n"));
    CHECK_EQ(exchange(h, "ATE1\r", "\r\nOK\r\n"), std::string("ATE1\r\r\nOK\r\n"));

    // ATE0 отключает эхо: ответ на команду приходит без отражения ввода.
    CHECK_EQ(exchange(h, "ATE0\r", "\r\nOK\r\n"), std::string("ATE0\r\r\nOK\r\n"));
    CHECK(!h.server->echoEnabled());
    CHECK_EQ(exchange(h, "AT\r", "\r\nOK\r\n"), std::string("\r\nOK\r\n"));

    // ATE1 возвращает эхо, но сам ввод при выключенном эхе не отражается:
    // эффект применяется только к последующим символам.
    CHECK_EQ(exchange(h, "ATE1\r", "\r\nOK\r\n"), std::string("\r\nOK\r\n"));
    CHECK(h.server->echoEnabled());
    CHECK_EQ(exchange(h, "AT\r", "\r\nOK\r\n"), std::string("AT\r\r\nOK\r\n"));
}

/// @brief Команда, отсутствующая в словаре, получает ответ ERROR.
MINITEST_TEST(server_pty_unknown_command_gives_error)
{
    PtyHarness h = PtyHarness::make(/*echo=*/false);
    CHECK_EQ(exchange(h, "AT+BOGUS\r", "\r\nERROR\r\n"), std::string("\r\nERROR\r\n"));
}

/// @brief Команды в нижнем регистре распознаются; ATI отвечает многострочно.
MINITEST_TEST(server_pty_case_insensitive_and_multiline_ati)
{
    PtyHarness h = PtyHarness::make(false);
    const std::string reply = exchange(h, "ati\r", "Manufacturer");
    CHECK(reply.find("Manufacturer: TestModems") != std::string::npos);
    CHECK(reply.find("Model: TM-1") != std::string::npos);
    CHECK(reply.find("Revision: 9.9") != std::string::npos);
    CHECK(reply.find("OK") != std::string::npos);
}

/// @brief CSV-правило с '=' в шаблоне: различаются AT+COPS=? и AT+COPS?.
MINITEST_TEST(server_pty_cops_quoted_rule_with_equals_pattern)
{
    PtyHarness h = PtyHarness::make(false);

    // Правило записано в CSV-форме: "AT+COPS=?","+COPS: ..."
    const std::string scan = exchange(h, "AT+COPS=?\r", "+COPS:");
    CHECK(scan.find("(2,\"TestNet\",\"25099\",7)") != std::string::npos);
    CHECK(scan.ends_with("\r\nOK\r\n"));

    // Обычный запрос текущего оператора.
    const std::string current = exchange(h, "AT+COPS?\r", "+COPS: 0");
    CHECK(current.find("\"TestNet\"") != std::string::npos);
}

/// @brief Запрос состояния PIN и ввод PIN по шаблону из четырёх цифр.
///
/// Многострочный ответ содержит внутренний разделитель CRLF и внешнее
/// обрамление CRLF; неверная длина PIN не совпадает с шаблоном -> ERROR.
MINITEST_TEST(server_pty_cpin_query_and_pin_entry)
{
    PtyHarness h = PtyHarness::make(false);
    // Многострочный ответ: внутренний разделитель CRLF, обрамление CRLF по краям.
    CHECK_EQ(exchange(h, "AT+CPIN?\r", "\r\nOK\r\n"),
             std::string("\r\n+CPIN: READY\r\nOK\r\n"));
    CHECK_EQ(exchange(h, "AT+CPIN=1234\r", "PIN ACCEPTED"),
             std::string("\r\nPIN ACCEPTED\r\n"));
    // Неверная длина PIN не соответствует шаблону из четырёх цифр.
    CHECK_EQ(exchange(h, "AT+CPIN=12\r", "\r\nERROR\r\n"), std::string("\r\nERROR\r\n"));
}

/// @brief Пустая командная строка (одиночный CR) игнорируется молча.
MINITEST_TEST(server_pty_empty_line_is_silently_ignored)
{
    PtyHarness h = PtyHarness::make(false);
    h.send("\r");
    std::string received;
    for (int i = 0; i < 6; ++i) {
        CHECK(h.server->pollOnce());
        received += h.readAvailable(10);
    }
    CHECK(received.empty()); // ни эха (выключен), ни ответа
}

/// @brief Backspace от клиента корректирует команду до её отправки модему.
///
/// Клиент печатает "ATZ", стирает Z терминатором строки: сервер видит "AT".
MINITEST_TEST(server_pty_backspace_edits_command_before_send)
{
    PtyHarness h = PtyHarness::make(true);
    const std::string reply = exchange(h, "ATZ\b\r", "\r\nOK\r\n");
    CHECK_EQ(reply, std::string("ATZ\b \b\r\r\nOK\r\n"));
}

/// @brief Командная строка длиннее лимита завершается ответом ERROR.
MINITEST_TEST(server_pty_line_overflow_answers_error)
{
    PtyHarness h = PtyHarness::make(false);
    h.send(std::string(300, 'x') + "\r"); // длиннее maxLineLength (по умолчанию 128)
    std::string received;
    const long long deadline = nowMs() + 4000;
    while (received.find("\r\nERROR\r\n") == std::string::npos) {
        CHECK(nowMs() < deadline);
        CHECK(h.server->pollOnce());
        received += h.readAvailable(5);
    }
}

/// @brief CAN (0x18) отменяет текущую командную строку.
///
/// Подозрительное место: CAN очищает буфер сборщика, но строка остаётся
/// "завершённой" — сервер забирает пустую строку и молча её игнорирует.
/// Клиент при этом видит только эхо (команда, "\r\n" от CAN и CR).
MINITEST_TEST(server_pty_can_cancels_command)
{
    PtyHarness h = PtyHarness::make(/*echo=*/true);

    h.send("ATX\x18\r");
    std::string received;
    for (int i = 0; i < 8; ++i) {
        CHECK(h.server->pollOnce());
        received += h.readAvailable(10);
    }
    // Эхо: 'A' 'T' 'X', затем "\r\n" (эхо CAN), затем "\r" (эхо CR).
    // Ни OK, ни ERROR — строка была пуста.
    CHECK_EQ(received, std::string("ATX\r\n\r"));
}

/// @brief ATE0/ATE1 работают без учёта регистра (в т.ч. смешанный).
MINITEST_TEST(server_pty_ate_case_insensitive)
{
    PtyHarness h = PtyHarness::make(/*echo=*/true);

    CHECK_EQ(exchange(h, "ate0\r", "\r\nOK\r\n"), std::string("ate0\r\r\nOK\r\n"));
    CHECK(!h.server->echoEnabled());

    // Состояние сохранилось: последующий ввод не отражается.
    CHECK_EQ(exchange(h, "AT\r", "\r\nOK\r\n"), std::string("\r\nOK\r\n"));

    CHECK_EQ(exchange(h, "aTe1\r", "\r\nOK\r\n"), std::string("\r\nOK\r\n"));
    CHECK(h.server->echoEnabled());
    CHECK_EQ(exchange(h, "AT\r", "\r\nOK\r\n"), std::string("AT\r\r\nOK\r\n"));
}

/// @brief Ответ словаря передаётся клиенту БУКВАЛЬНО: пробелы не трогаются.
///
/// Фиксирует сквозное поведение: ответ "AT = OK" (ведущий пробел) уходит
/// на линию с этим пробелом — сервер не обрезает ответы словаря.
MINITEST_TEST(server_pty_answer_whitespace_verbatim)
{
    auto pairResult = SerialPort::openPty();
    CHECK(pairResult.has_value());

    auto dict = CommandDictionary::loadFromText("AT= OK\n");
    CHECK(dict.has_value());

    AtServerOptions opts{false}; // эхо выключено, остальное — по умолчанию
    AtServer server(std::move(*dict), std::move(pairResult->slave), opts);

    const int master = pairResult->master.get();
    const std::string cmd = "AT\r";
    CHECK_EQ(::write(master, cmd.data(), cmd.size()), static_cast<ssize_t>(cmd.size()));

    std::string received;
    const long long deadline = nowMs() + 4000;
    while (received.find("\r\n") == std::string::npos) {
        CHECK(nowMs() < deadline);
        CHECK(server.pollOnce());
        received += [&] {
            pollfd pfd{.fd = master, .events = POLLIN, .revents = 0};
            std::string chunk;
            if (::poll(&pfd, 1, 5) > 0) {
                char buf[64] = {};
                const ssize_t n = ::read(master, buf, sizeof(buf));
                if (n > 0) {
                    chunk.assign(buf, static_cast<std::size_t>(n));
                }
            }
            return chunk;
        }();
    }
    // Эхо выключено, поэтому приходит только ответ: CRLF + " OK" + CRLF.
    CHECK_EQ(received, std::string("\r\n OK\r\n"));
}

/// @brief Штатное завершение по установленному флагу остановки.
///
/// run() проверяет флаг в начале каждого цикла: если он уже выставлен,
/// сервер завершается с кодом 0 без ожидания данных.
MINITEST_TEST(server_stops_on_stop_flag)
{
    PtyHarness h = PtyHarness::make(false);

    std::atomic_bool stop{false};
    stop.store(true, std::memory_order_relaxed);

    CHECK_EQ(h.server->run(stop), 0);
}

/// @brief openDevice() открывает реальный tty-путь (slave-сторону PTY).
///
/// Покрывает фабрику openDevice: пару master/slave мы уже умеем создавать,
/// а открытие существующего устройства по имени — нет.
MINITEST_TEST(server_open_device_on_pty_slave)
{
    auto pairResult = SerialPort::openPty();
    CHECK(pairResult.has_value());

    const std::string slavePath = pairResult->slavePath;
    auto port = SerialPort::openDevice(slavePath);
    CHECK(port.has_value());
    CHECK_EQ(port->name(), slavePath);
    CHECK(port->fd() >= 0);
}
