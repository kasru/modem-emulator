/**
 * @file test_line_assembler.cpp
 * @brief Модульные тесты сборщика командных строк LineAssembler.
 *
 * Проверяется поведение "как у типичного модема": терминатор CR, игнор
 * LF/NUL, редактирование BS/DEL с эхом, отмена строки по CAN, лимит
 * длины строки и отбрасывание управляющих символов.
 */

#include "test_framework.hpp"

#include "line_assembler.hpp"

using atmodem::FeedResult;
using atmodem::LineAssembler;

namespace {

/// @brief Прогоняет строку байтов через сборщик, возвращая накопленное эхо.
std::string feedAll(LineAssembler& asmr, std::string_view input)
{
    std::string echo;
    for (const char c : input) {
        const FeedResult fr = asmr.feed(c);
        if (fr.echo) {
            echo.append(fr.echoSeq);
        }
    }
    return echo;
}

} // namespace

/// @brief Строка собирается до CR; каждый символ, включая терминатор, даёт эхо.
MINITEST_TEST(assembler_basic_line)
{
    LineAssembler asmr(128);
    const std::string echo = feedAll(asmr, "AT\r");
    CHECK_EQ(echo, std::string("AT\r")); // посимвольное эхо + эхо CR

    std::string line;
    CHECK(asmr.takeLine(line));
    CHECK_EQ(line, std::string("AT"));
    CHECK(!asmr.takeLine(line)); // вторая выборка невозможна
}

/// @brief LF и NUL игнорируются прозрачно (совместимость с клиентами CRLF).
MINITEST_TEST(assembler_ignores_lf_and_nul)
{
    LineAssembler asmr(128);
    feedAll(asmr, "AT\n"); // LF игнорируется (клиенты с CRLF)
    CHECK_EQ(asmr.pending(), std::string_view("AT"));

    feedAll(asmr, "\0\0");
    CHECK_EQ(asmr.pending(), std::string_view("AT"));

    feedAll(asmr, "\r");
    std::string line;
    CHECK(asmr.takeLine(line));
    CHECK_EQ(line, std::string("AT"));
}

/// @brief BS/DEL стирают последний символ и возвращают эхо "\\b \\b".
MINITEST_TEST(assembler_backspace_editing)
{
    LineAssembler asmr(128);
    CHECK_EQ(feedAll(asmr, "ATX"), std::string("ATX"));
    // стирание даёт эхо "\b \b"
    CHECK_EQ(feedAll(asmr, "\bT"), std::string("\b \bT"));
    std::string line;
    feedAll(asmr, "\r");
    CHECK(asmr.takeLine(line));
    CHECK_EQ(line, std::string("ATT")); // X стёрт, T дописан
}

/// @brief BS на пустой строке не меняет буфер и не порождает эха.
MINITEST_TEST(assembler_backspace_on_empty_is_silent)
{
    LineAssembler asmr(128);
    const FeedResult fr = asmr.feed('\b');
    CHECK(!fr.echo);           // на пустой строке BS молча
    CHECK(fr.echoSeq.empty());
}

/// @brief CAN (0x18) отменяет текущую строку целиком.
MINITEST_TEST(assembler_cancel_line)
{
    LineAssembler asmr(128);
    feedAll(asmr, "AT+COPS");
    const FeedResult fr = asmr.feed(0x18); // CAN отменяет строку
    CHECK(fr.echo);
    CHECK_EQ(fr.echoSeq, std::string_view("\r\n"));
    CHECK_EQ(asmr.pending(), std::string_view(""));
    feedAll(asmr, "\r");
    std::string line;
    CHECK(asmr.takeLine(line));
    CHECK_EQ(line, std::string("")); // пустая команда после отмены
}

/// @brief Превышение лимита длины отмечается флагом overflow.
///
/// Лишние символы не накапливаются (строка обрезается до лимита), но всё
/// равно эхо-ятся, как у реального модема. Завершённая строка извлекается
/// с флагом overflow — сервер обязан ответить на неё ERROR.
MINITEST_TEST(assembler_overflow_flag)
{
    LineAssembler asmr(4);
    FeedResult last{};
    for (int i = 0; i < 10; ++i) {
        last = asmr.feed('x');
    }
    CHECK(last.overflowed);                // лимит 4 символа превышен
    CHECK(last.echo);                      // переполненный символ всё же эхо-ится
    CHECK_EQ(last.echoSeq, std::string_view("x"));

    feedAll(asmr, "\r");
    std::string line;
    CHECK(asmr.takeLine(line));  // строка всё же завершена...
    CHECK_EQ(line.size(), std::size_t{4});
    // ...и сервер обязан ответить ERROR (см. интеграционный тест)
}

/// @brief reset() очищает незавершённый буфер и все флаги.
MINITEST_TEST(assembler_reset_clears_state)
{
    LineAssembler asmr(8);
    feedAll(asmr, "AT");
    for (int i = 0; i < 20; ++i) {
        (void)asmr.feed('x');     // добиваем до лимита и переполняем
    }
    CHECK_EQ(asmr.pending().size(), std::size_t{8}); // лимит достигнут
    CHECK(asmr.overflow());
    (void)asmr.feed('\r');        // строка завершена, но ещё не забрана

    asmr.reset();                 // сброс без takeLine()
    CHECK(!asmr.overflow());
    CHECK(asmr.pending().empty());
    std::string line;
    CHECK(!asmr.takeLine(line));  // завершённость тоже сброшена

    // После сброса сборщик продолжает работать как новый.
    feedAll(asmr, "ATE1\r");
    CHECK(asmr.takeLine(line));
    CHECK_EQ(line, std::string("ATE1"));
}

/// @brief Регрессия: после takeLine() флаг переполнения сброшен.
///
/// Раньше overflow_ "залипал": одна слишком длинная строка делала все
/// последующие команды ERROR без обращения к словарю.
MINITEST_TEST(assembler_overflow_resets_after_take_line)
{
    LineAssembler asmr(4);
    for (int i = 0; i < 10; ++i) {
        (void)asmr.feed('x');   // переполняем лимит
    }
    feedAll(asmr, "\r");
    std::string line;
    CHECK(asmr.takeLine(line));
    CHECK(!asmr.overflow());    // буфер очищен вместе с признаком переполнения

    // Следующая короткая строка не должна наследовать переполнение.
    const FeedResult fr = asmr.feed('a');
    CHECK(!fr.overflowed);
    feedAll(asmr, "\r");
    CHECK(asmr.takeLine(line));
    CHECK_EQ(line, std::string("a"));
}

/// @brief Одиночные управляющие символы (кроме обработанных выше) отбрасываются.
MINITEST_TEST(assembler_control_chars_dropped)
{
    LineAssembler asmr(128);
    feedAll(asmr, "AT\x01\x02");
    CHECK_EQ(asmr.pending(), std::string_view("AT"));
}

/// @brief BS из состояния переполнения возвращает строку в рабочий вид.
///
/// Подозрительное место: флаг overflow "липнет", пока не стёрнут
/// хотя бы один символ; после стирания строка снова может завершиться
/// без ERROR. Фиксируем эту логику восстановления.
MINITEST_TEST(assembler_backspace_recovers_from_overflow)
{
    LineAssembler asmr(4);
    for (int i = 0; i < 5; ++i) {
        (void)asmr.feed('x');
    }
    CHECK(asmr.overflow());
    CHECK_EQ(asmr.pending(), std::string_view("xxxx"));

    const FeedResult fr = asmr.feed('\b'); // стираем один лишний символ
    CHECK(fr.echo);
    CHECK_EQ(fr.echoSeq, std::string_view("\b \b"));
    CHECK(!asmr.overflow());               // снова в пределах лимита
    CHECK_EQ(asmr.pending(), std::string_view("xxx"));

    (void)asmr.feed('\r');
    std::string line;
    CHECK(asmr.takeLine(line));
    CHECK_EQ(line, std::string("xxx")); // сервер ответит OK, а не ERROR
}

/// @brief DEL (0x7F) стирает символ точно так же, как BS (0x08).
MINITEST_TEST(assembler_del_erases_like_backspace)
{
    LineAssembler asmr(16);
    feedAll(asmr, "AT");

    const FeedResult fr = asmr.feed(0x7F);
    CHECK(fr.echo);
    CHECK_EQ(fr.echoSeq, std::string_view("\b \b")); // эхо единообразно
    CHECK_EQ(asmr.pending(), std::string_view("A"));

    const FeedResult fr2 = asmr.feed('\b');
    CHECK(fr2.echo);
    CHECK(asmr.pending().empty());
}

/// @brief Ввод после CR игнорируется до takeLine()/reset().
///
/// Исправление: раньше completed_ не блокировал приём новых символов,
/// и "AT\rZ\r" давал строку "ATZ" вместо "AT" — ловушка для прямого
/// пользователя сборщика. Теперь после CR сборщик "заперт": последующие
/// символы не накапливаются и не эхо-ятся, пока строка не забрана.
MINITEST_TEST(assembler_chars_after_cr_before_take)
{
    LineAssembler asmr(16);
    feedAll(asmr, "AT\rZ\r");

    std::string line;
    CHECK(asmr.takeLine(line));
    CHECK_EQ(line, std::string("AT")); // "Z" после CR отброшен
    CHECK(!asmr.takeLine(line));       // вторая выборка невозможна
}

/// @brief CAN после CR не действует: строка уже завершена и не отменяется.
///
/// Исправление: раньше CAN очищал буфер, но оставлял completed_ взведённым,
/// и takeLine() возвращал ПУСТУЮ строку. Теперь CAN (как и любой другой
/// ввод) после CR игнорируется, а takeLine() отдаёт завершённую строку.
MINITEST_TEST(assembler_can_after_cr_is_ignored)
{
    LineAssembler asmr(16);
    feedAll(asmr, "AT\r");
    (void)asmr.feed(0x18); // CAN — без эффекта после CR

    std::string line;
    CHECK(asmr.takeLine(line));
    CHECK_EQ(line, std::string("AT")); // строка завершена как была
    CHECK(!asmr.takeLine(line));
}

/// @brief Лимит длины нулевой: любой символ — переполнение.
///
/// Убедимся, что нулевой лимит не вызывает UB (деление, пустые срезы)
/// и что строка корректно завершается пустым содержимым.
MINITEST_TEST(assembler_zero_limit)
{
    LineAssembler asmr(0);

    const FeedResult fr = asmr.feed('A');
    CHECK(fr.overflowed);
    CHECK(asmr.pending().empty()); // символ не накопился

    (void)asmr.feed('\r');
    std::string line;
    CHECK(asmr.takeLine(line));
    CHECK(line.empty());
    CHECK(!asmr.overflow());
}

/// @brief Эхо-последовательности стабильны для разных типов ввода.
///
/// Замечание (подозрительная асимметрия): LF и NUL игнорируются БЕЗ эха,
/// тогда как CAN/CR/BS/DEL эхо-ятся. Клиент, отправляющий CRLF, увидит
/// эхо только CR — это осознанное решение (см. kCrEcho), но асимметрию
/// с LF фиксируем здесь явно.
MINITEST_TEST(assembler_echo_sequences)
{
    LineAssembler asmr(16);

    CHECK_EQ(asmr.feed('A').echoSeq, std::string_view("A"));
    CHECK_EQ(asmr.feed('\b').echoSeq, std::string_view("\b \b"));

    // LF и NUL — без эха вовсе.
    const FeedResult lf = asmr.feed('\n');
    CHECK(!lf.echo);
    CHECK(lf.echoSeq.empty());
    const FeedResult nul = asmr.feed('\0');
    CHECK(!nul.echo);
    CHECK(nul.echoSeq.empty());

    CHECK_EQ(asmr.feed(0x18).echoSeq, std::string_view("\r\n")); // CAN
    CHECK_EQ(asmr.feed('\r').echoSeq, std::string_view("\r"));
}

/// @brief NAK (0x15) эквивалентен CAN: очищает буфер И флаг переполнения.
MINITEST_TEST(assembler_nak_clears_overflow)
{
    LineAssembler asmr(4);
    for (int i = 0; i < 5; ++i) {
        (void)asmr.feed('x');
    }
    CHECK(asmr.overflow());

    const FeedResult fr = asmr.feed(0x15); // NAK
    CHECK(fr.echo);
    CHECK_EQ(fr.echoSeq, std::string_view("\r\n")); // как у CAN
    CHECK(!asmr.overflow());
    CHECK(asmr.pending().empty());

    // Строка после NAK собирается заново и завершается штатно.
    feedAll(asmr, "AT\r");
    std::string line;
    CHECK(asmr.takeLine(line));
    CHECK_EQ(line, std::string("AT"));
}
