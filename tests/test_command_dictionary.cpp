/**
 * @file test_command_dictionary.cpp
 * @brief Модульные тесты загрузчика словаря CommandDictionary.
 *
 * Проверяются обе формы записи правил (простая `expect=answer` и CSV
 * с кавычками), escape-последовательности, порядок применения правил,
 * а также диагностика ошибок с указанием номера строки.
 */

#include "test_framework.hpp"

#include "command_dictionary.hpp"
#include "pattern_matcher.hpp"

#include <cstdlib>
#include <unistd.h>

using atmodem::CommandDictionary;

/// @brief Простой формат: комментарии, пустые строки, CRLF и разворачивание
/// escape-последовательностей CR/LF в ответах.
MINITEST_TEST(dictionary_plain_format_and_comments)
{
    const auto dict = CommandDictionary::loadFromText(
        "# комментарий\n"
        "\n" // пустая строка
        "   # комментарий с отступом\n"
        "AT=OK\n"
        "ATE[01]=OK\n"
        "\r\n" // пустая строка в стиле CRLF
        "ATI=Manufacturer: X\\r\\nOK\n");
    CHECK(dict.has_value());
    CHECK_EQ(dict->rules().size(), std::size_t{3});

    const CommandDictionary::Rule* rule = dict->find("AT");
    CHECK(rule != nullptr);
    CHECK_EQ(rule->answer, std::string("OK"));

    CHECK(dict->find("ate1") != nullptr);
    CHECK(dict->find("AT+NOPE") == nullptr);

    const CommandDictionary::Rule* ati = dict->find("ATI");
    CHECK(ati != nullptr);
    CHECK_EQ(ati->answer, std::string("Manufacturer: X\r\nOK")); // \r\n развёрнуты
}

/// @brief Экранированный разделитель '\\=' позволяет включить '=' в ожидание.
///
/// Проверяется правило AT+CPIN\=[0-9]{4}: запрос CPIN? не должен совпасть
/// с шаблоном ввода PIN.
MINITEST_TEST(dictionary_escaped_equals_in_pattern)
{
    const auto dict = CommandDictionary::loadFromText(
        "AT+CPIN?=+CPIN: READY\\r\\nOK\n"
        "AT+CPIN\\=[0-9][0-9][0-9][0-9]=PIN OK\n");

    CHECK(dict.has_value());
    const auto* query = dict->find("AT+CPIN?");
    CHECK(query != nullptr);
    CHECK_EQ(query->answer, std::string("+CPIN: READY\r\nOK"));

    const auto* pin = dict->find("AT+CPIN=1234");
    CHECK(pin != nullptr);
    CHECK_EQ(pin->pattern, std::string("AT+CPIN=[0-9][0-9][0-9][0-9]"));
    CHECK_EQ(pin->answer, std::string("PIN OK"));

    CHECK(dict->find("AT+CPIN") == nullptr);
}

/// @brief CSV-форма "expect","answer": '=' в шаблоне и удвоенные кавычки в ответе.
MINITEST_TEST(dictionary_quoted_csv_format)
{
    const auto dict = CommandDictionary::loadFromText(
        "\"AT+COPS=?\",\"+COPS: (2,\"\"AtMobile\"\",\"\"25099\"\",7)\\r\\nOK\"\n"
        "\"AT\",\"OK\"\n");

    CHECK(dict.has_value());
    const auto* cops = dict->find("AT+COPS=?");
    CHECK(cops != nullptr);
    CHECK_EQ(cops->pattern, std::string("AT+COPS=?"));
    CHECK_EQ(cops->answer, std::string("+COPS: (2,\"AtMobile\",\"25099\",7)\r\nOK"));
}

/// @brief Посторонний текст вне кавычек между полями и после них - ошибка.
///
/// Регрессия: строка "AT",мусор,"OK" раньше принималась, мусор молча
/// пропускался, и правило получало ответ OK.
MINITEST_TEST(dictionary_quoted_csv_rejects_stray_text)
{
    // мусор между полями
    auto bad1 = CommandDictionary::loadFromText("\"AT\",garbage,\"OK\"\n");
    CHECK(!bad1.has_value());
    CHECK(bad1.error().find("вне кавычек") != std::string::npos);

    // мусор после второго поля
    auto bad2 = CommandDictionary::loadFromText("\"AT\",\"OK\"junk\n");
    CHECK(!bad2.has_value());
    CHECK(bad2.error().find("вне кавычек") != std::string::npos);

    // пробелы между полями по-прежнему допустимы
    auto good = CommandDictionary::loadFromText("\"AT\" ,\t\"OK\"\n");
    CHECK(good.has_value());
}

/// @brief Правила проверяются по порядку объявления; побеждает первое совпавшее.
MINITEST_TEST(dictionary_first_match_wins)
{
    const auto dict = CommandDictionary::loadFromText(
        "A*=ПЕРВОЕ\n"
        "AT=ВТОРОЕ\n");
    CHECK(dict.has_value());
    const auto* rule = dict->find("AT"); // оба шаблона подходят, побеждает первый
    CHECK(rule != nullptr);
    CHECK_EQ(rule->answer, std::string("ПЕРВОЕ"));
}

/// @brief Ошибки загрузки сообщают источник и номер проблемной строки.
///
/// Покрываются: строка без '=', пустой шаблон, незакрытая кавычка,
/// словарь без правил, отсутствующий файл.
MINITEST_TEST(dictionary_errors_are_reported)
{
    // нет разделителя '='
    auto bad1 = CommandDictionary::loadFromText("AT=OK\nмусор без равно\n");
    CHECK(!bad1.has_value());
    CHECK(bad1.error().find("<memory>:2:") != std::string::npos); // номер строки

    // пустой шаблон
    auto bad2 = CommandDictionary::loadFromText("=OK\n");
    CHECK(!bad2.has_value());

    // незакрытая кавычка CSV
    auto bad3 = CommandDictionary::loadFromText("\"AT\",\"OK\n");
    CHECK(!bad3.has_value());

    // пустой словарь
    auto bad4 = CommandDictionary::loadFromText("# только комментарии\n");
    CHECK(!bad4.has_value());

    // отсутствующий файл
    auto bad5 = CommandDictionary::loadFromFile("/tmp/opencode/нет_такого_файла.csv");
    CHECK(!bad5.has_value());
}

/// @brief Неизвестная escape-последовательность трактуется как следующий за '\\' символ.
MINITEST_TEST(dictionary_unknown_escape_is_lenient)
{
    const auto dict = CommandDictionary::loadFromText("AT=x\\qy\n");
    CHECK(dict.has_value());
    CHECK_EQ(dict->find("AT")->answer, std::string("xqy"));
}

/// @brief CSV: некорректное число полей — отдельные сообщения об ошибке.
///
/// Подозрительное место: машина состояний parseQuotedCsv() имеет
/// специфичные ветки ошибок (одно поле / три поля / текст вне кавычек).
/// Фиксируем, что каждая ветка реально срабатывает.
MINITEST_TEST(dictionary_csv_field_count_errors)
{
    // Одно поле: кавычка закрыта, разделитель так и не появился.
    auto bad1 = CommandDictionary::loadFromText("\"AT\"\n");
    CHECK(!bad1.has_value());
    CHECK(bad1.error().find("два поля") != std::string::npos);

    // Три поля: второе закрыто запятой, идёт третье.
    auto bad2 = CommandDictionary::loadFromText("\"AT\",\"OK\",\"X\"\n");
    CHECK(!bad2.has_value());
    CHECK(bad2.error().find("лишние поля") != std::string::npos);

    // Текст вне кавычек внутри поля.
    auto bad3 = CommandDictionary::loadFromText("\"AT\",\"OK\"extra\n");
    CHECK(!bad3.has_value());
    CHECK(bad3.error().find("вне кавычек") != std::string::npos);

    // Пустой шаблон (даже если полей ровно два).
    auto bad4 = CommandDictionary::loadFromText("\"\",\"X\"\n");
    CHECK(!bad4.has_value());
    CHECK(bad4.error().find("пустое ожидание") != std::string::npos);
}

/// @brief Кавычка в CSV-поле: и удвоение "", и бэкслэш-экранирование \".
///
/// Исправление: раньше \" внутри CSV-поля не экранировал кавычку — одиночная
/// '"' закрывала поле, и запись "AT","a\"b" была ошибкой формата. Теперь
/// бэкслэш работает и в CSV (единая модель с простой формой и expandEscapes),
/// а классическое удвоение "" продолжает поддерживаться.
MINITEST_TEST(dictionary_csv_quote_doubling_vs_backslash)
{
    // Удвоение кавычки — классический CSV-способ.
    auto good = CommandDictionary::loadFromText("\"AT\",\"a\"\"b\"\n");
    CHECK(good.has_value());
    CHECK_EQ(good->find("AT")->answer, std::string("a\"b"));

    // Бэкслэш-экранирование даёт тот же результат.
    auto escaped = CommandDictionary::loadFromText("\"AT\",\"a\\\"b\"\n");
    CHECK(escaped.has_value());
    CHECK_EQ(escaped->find("AT")->answer, std::string("a\"b"));
}

/// @brief Экранирование в простой форме: \", \\, \= и одиночный хвостовой \.
MINITEST_TEST(dictionary_plain_escape_variants)
{
    auto d = CommandDictionary::loadFromText(
        "AT=a\\\"b\\=c\\\n"
        "AT2=x\\\\y\n");
    CHECK(d.has_value());

    // \" -> кавычка; \= -> равно; хвостовой одиночный \ сохранён.
    CHECK_EQ(d->find("AT")->answer, std::string("a\"b=c\\"));
    // \\ -> один обратный слеш.
    CHECK_EQ(d->find("AT2")->answer, std::string("x\\y"));

    // Неизвестная последовательность \X молча превращается в X.
    auto d2 = CommandDictionary::loadFromText("AT3=q\\w\n");
    CHECK(d2.has_value());
    CHECK_EQ(d2->find("AT3")->answer, std::string("qw"));
}

/// @brief Ответ хранится БУКВАЛЬНО: в отличие от шаблона, НЕ обрезается.
///
/// Подозрительное место: loadFromText() trim'ит целую строку и шаблон,
/// а ответ берётся как есть. Поэтому ведущий пробел в ответе
/// ("AT= OK") сохранится и улетит клиенту. В CSV-форме то же самое:
/// "AT"," OK" -> " OK". Фиксируем асимметрию (паттерн — trim, ответ — нет).
MINITEST_TEST(dictionary_answer_not_trimmed_like_pattern)
{
    auto d = CommandDictionary::loadFromText("AT= OK\n");
    CHECK(d.has_value());
    CHECK_EQ(d->find("AT")->pattern, std::string("AT"));  // шаблон обрезан
    CHECK_EQ(d->find("AT")->answer, std::string(" OK"));  // ответ — нет!

    auto d2 = CommandDictionary::loadFromText("\"AT\",\" OK\"\n");
    CHECK(d2.has_value());
    CHECK_EQ(d2->find("AT")->pattern, std::string("AT"));
    CHECK_EQ(d2->find("AT")->answer, std::string(" OK"));
}

/// @brief '=' внутри ответа не делит запись; реальная переносная в кавычках
/// невозможна (запись читается построчно) — многострочность только через \r\n.
MINITEST_TEST(dictionary_equals_in_answer_and_newline_limit)
{
    // Разделитель — ПЕРВОЕ '='; всё, что после (включая другие '='),
    // идёт в ответ и хранится буквально.
    auto d = CommandDictionary::loadFromText("AT+=1=2\n");
    CHECK(d.has_value());
    CHECK_EQ(d->find("AT+")->pattern, std::string("AT+"));
    CHECK_EQ(d->find("AT+")->answer, std::string("1=2"));

    // Реальный '\n' внутри кавычек рвёт запись: первая строка
    // остаётся с незакрытой кавычкой.
    auto bad = CommandDictionary::loadFromText("\"AT\",\"line1\nline2\"\n");
    CHECK(!bad.has_value());
    CHECK(bad.error().find("незакрытая кавычка") != std::string::npos);

    // Корректный способ записать многострочный ответ — экранированный CRLF.
    auto good = CommandDictionary::loadFromText("\"ATI\",\"line1\\r\\nline2\"\n");
    CHECK(good.has_value());
    CHECK_EQ(good->find("ATI")->answer, std::string("line1\r\nline2"));
}

/// @brief Встроенный демо-словарь покрывает базовый набор команд.
MINITEST_TEST(dictionary_builtin_covers_base_commands)
{
    const auto dict = CommandDictionary::loadBuiltin();
    CHECK(dict.has_value());

    CHECK(dict->find("AT") != nullptr);
    CHECK(dict->find("ATE0") != nullptr);
    CHECK(dict->find("ATE1") != nullptr);
    CHECK(dict->find("ATI") != nullptr);
    CHECK(dict->find("AT+COPS?") != nullptr);
    CHECK(dict->find("AT+CPIN?") != nullptr);
    // Правила вне базового набора не распознаются.
    CHECK(dict->find("AT+CGMI") == nullptr);
}

/// @brief Загрузка из файла (успешный путь; отсутствующий файл уже покрыт).
MINITEST_TEST(dictionary_load_from_file)
{
    char path[] = "/tmp/atmodem_dict_test_XXXXXX";
    const int fd = ::mkstemp(path);
    CHECK(fd >= 0);

    const std::string content = "AT=OK\nATE[01]=OK\n";
    CHECK_EQ(::write(fd, content.data(), content.size()),
             static_cast<ssize_t>(content.size()));
    ::close(fd);

    const auto dict = CommandDictionary::loadFromFile(path);
    ::unlink(path);

    CHECK(dict.has_value());
    CHECK_EQ(dict->rules().size(), std::size_t{2});
    CHECK(dict->find("AT") != nullptr);
    CHECK(dict->find("ATE0") != nullptr);
}
