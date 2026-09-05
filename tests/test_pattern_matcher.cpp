/**
 * @file test_pattern_matcher.cpp
 * @brief Модульные тесты сопоставителя шаблонов patternMatches.
 *
 * Проверяется ограниченный синтаксис регулярных выражений из тестового
 * задания: литералы, '.', '*', символьные классы '[...]' с диапазонами
 * и отрицанием, а также граничные случаи.
 */

#include "test_framework.hpp"

#include "pattern_matcher.hpp"

#include <string_view>

using atmodem::patternMatches;

/// @brief Литеральные шаблоны совпадают целиком и без учёта регистра букв.
MINITEST_TEST(matcher_exact_literal)
{
    CHECK(patternMatches("AT", "AT"));
    CHECK(patternMatches("at", "AT"));  // регистр не важен
    CHECK(patternMatches("AT+CPIN?", "at+cpin?"));
    CHECK(!patternMatches("AT", "ATE"));
    CHECK(!patternMatches("AT", "AT0"));
    CHECK(!patternMatches("AT", ""));
}

/// @brief Точка '.' соответствует ровно одному произвольному символу.
MINITEST_TEST(matcher_dot_single_char)
{
    // '.' - ровно один любой символ
    CHECK(patternMatches("A.", "AT"));
    CHECK(patternMatches(".T", "aT"));
    CHECK(patternMatches(".", "x"));
    CHECK(patternMatches("...", "abc"));
    CHECK(!patternMatches("A.", "A"));   // нужен ровно один символ
    CHECK(!patternMatches("A.", "ATE")); // и не больше одного
    CHECK(!patternMatches(".", ""));
}

/// @brief Звёздочка '*' - ноль или больше любых символов.
///
/// Включает примеры из обсуждения в TASK.md: шаблон "A*E" соответствует
/// "AE", "AbE", "AbcdE" и "AbcdxyzE".
MINITEST_TEST(matcher_star_zero_or_more_any)
{
    // '*' - ноль или больше любых символов (аналог '.*')
    CHECK(patternMatches("*", ""));
    CHECK(patternMatches("*", "anything"));
    CHECK(patternMatches("A*", "A"));
    CHECK(patternMatches("A*", "AbCd"));
    CHECK(patternMatches("*T", "T"));
    CHECK(patternMatches("*T", "abT"));
    // из обсуждения в TASK.md: A*E соответствует AbcdE и AbcdxyzE
    CHECK(patternMatches("A*E", "AE"));
    CHECK(patternMatches("A*E", "AbE"));
    CHECK(patternMatches("A*E", "AbcdE"));
    CHECK(patternMatches("A*E", "AbcdxyzE"));
    CHECK(!patternMatches("A*E", "Abcd")); // нет завершающего E
}

/// @brief Несколько '*' в одном шаблоне: откат и совпадение хвоста.
MINITEST_TEST(matcher_star_multiple_and_backtracking)
{
    CHECK(patternMatches("A*T*", "ATzz9"));
    CHECK(patternMatches("A*T*", "AT"));
    CHECK(patternMatches("AB*CD", "ABCD"));
    CHECK(patternMatches("AB*CD", "ABxxCD"));
    CHECK(!patternMatches("AB*CD", "ABxxC"));
    // откат через несколько '*'
    CHECK(patternMatches("*a*b*c*", "a-b-c-d"));
    CHECK(patternMatches("*a*b*c*", "abc"));
    CHECK(!patternMatches("*a*b*c*", "abd"));
    // '*' внутри шаблона поглощает всё, но хвост должен совпасть
    CHECK(patternMatches("AT+COPS.*", "AT+COPS=?"));
    CHECK(patternMatches("AT+COPS.*", "AT+COPS=1,2,\"x\""));
    CHECK(patternMatches("AT+COPS.*", "AT+COPSFOOBAR"));
}

/// @brief Символьные классы: перечисления, диапазоны, отрицание.
///
/// Отдельно проверяется, что '.' внутри класса трактуется как литерал.
MINITEST_TEST(matcher_character_class)
{
    CHECK(patternMatches("ATE[01]", "ATE0"));
    CHECK(patternMatches("ATE[01]", "ate1"));
    CHECK(!patternMatches("ATE[01]", "ATE2"));
    CHECK(!patternMatches("ATE[01]", "ATE"));   // класс требует символ
    CHECK(!patternMatches("ATE[01]", "ATE01")); // и только один

    CHECK(patternMatches("[abc]", "b"));
    CHECK(!patternMatches("[abc]", "d"));

    // диапазоны
    CHECK(patternMatches("AT+CPIN=[0-9][0-9][0-9][0-9]", "AT+CPIN=1234"));
    CHECK(patternMatches("[a-f]", "c"));
    CHECK(!patternMatches("[a-f]", "g"));

    // отрицание
    CHECK(patternMatches("[^0-9]", "x"));
    CHECK(!patternMatches("[^0-9]", "5"));

    // '.' внутри класса - литеральная точка
    CHECK(patternMatches("AT[.]X", "AT.X"));
    CHECK(!patternMatches("AT[.]X", "ATTX"));
}

/// @brief Граничные случаи: пустые строки, незакрытая '[' и '-' на границах класса.
MINITEST_TEST(matcher_edge_cases)
{
    // пустой шаблон соответствует только пустому тексту
    CHECK(patternMatches("", ""));
    CHECK(!patternMatches("", "x"));
    // незакрытая '[' трактуется как литерал
    CHECK(patternMatches("A[", "A["));
    CHECK(!patternMatches("A[", "Ax"));
    // '-' на границах класса - литерал
    CHECK(patternMatches("[-a]", "-"));
    CHECK(patternMatches("[a-]", "-"));
}

/// @brief Вырожденные символьные классы: "[]", "[^]", "[]a]", "[*]".
///
/// Зафиксируем текущее поведение:
///  - "[]" - синтаксически корректный, но пустой класс: ничего не совпадает;
///  - "[^]" - отрицание пустого класса: любой одиночный символ;
///  - ПЕРВОЕ "]" закрывает класс, поэтому "[]a]" = "[]" + литерал "a"
///    (в отличие от PCRE, где это класс {']','a'});
///  - "*" внутри класса - обычный литерал.
MINITEST_TEST(matcher_degenerate_classes)
{
    CHECK(!patternMatches("[]", "x"));
    CHECK(!patternMatches("[]", "]"));
    CHECK(!patternMatches("A[]", "A"));

    CHECK(patternMatches("[^]", "x"));
    CHECK(patternMatches("[^]", "]"));
    CHECK(patternMatches("A[^]B", "AzB"));
    CHECK(!patternMatches("A[^]B", "AB"));    // класс требует символ
    CHECK(!patternMatches("A[^]B", "ZzB"));   // и только один

    // Первое "]" закрывает класс: литералы после него не в классе.
    CHECK(!patternMatches("[]a]", "]"));
    CHECK(!patternMatches("[]a]", "a"));

    CHECK(patternMatches("[*]", "*"));
    CHECK(!patternMatches("[*]", "a"));
    CHECK(patternMatches("A[*]B", "A*B"));
}

/// @brief Обратные (lo > hi) диапазоны: концы трактуются как литералы.
///
/// Исправление: раньше "[z-a]" был "мёртвым" классом — classContains()
/// безвозвратно пропускал обе позиции (i += 2), поэтому класс не совпадал
/// ни с чем, даже с концами z/a. Теперь концы обратного диапазона
/// сравниваются как литералы, что интуитивно понятнее.
MINITEST_TEST(matcher_reversed_range_endpoints_are_literals)
{
    CHECK(!patternMatches("[z-a]", "m")); // диапазон пуст...
    CHECK(patternMatches("[z-a]", "z"));  // ...но концы — литералы
    CHECK(patternMatches("[z-a]", "a"));

    // Внутренний '-' связывается с предыдущим: тело "a--z" = {a, '-', z}.
    CHECK(patternMatches("[a--z]", "z"));
    CHECK(patternMatches("[a--z]", "a"));
    CHECK(patternMatches("[a--z]", "-"));
}

/// @brief Диапазоны без учёта регистра; вырожденные "*" и сложный откат.
MINITEST_TEST(matcher_case_insensitive_ranges_and_star_edges)
{
    // Диапазон в верхнем регистре ловит нижний и наоборот.
    CHECK(patternMatches("[A-F]", "c"));
    CHECK(patternMatches("at+[0-9a-f][0-9a-f]", "AT+ab"));
    CHECK(!patternMatches("at+[0-9a-f][0-9a-f]", "AT+g0"));

    // Соседние звёздочки эквивалентны одной.
    CHECK(patternMatches("a**b", "ab"));
    CHECK(patternMatches("a**b", "aXb"));
    CHECK(!patternMatches("a**b", "aXc"));
    CHECK(patternMatches("**", ""));
    CHECK(patternMatches("**", "anything"));

    // Откат через несколько '*' с обязательным совпадением хвоста.
    CHECK(patternMatches("*a*ba", "aaba"));
    CHECK(!patternMatches("*ab*ba", "ab"));  // хвост "*ba" не покрыт
    CHECK(patternMatches("*a*ab", "aab"));
    CHECK(!patternMatches("*a*ab", "aa"));
    CHECK(patternMatches("*a*a*b*", "aXaYb")); // a, a, b в порядке
    CHECK(!patternMatches("*a*a*b*", "a-b-c")); // только одна 'a'
    CHECK(!patternMatches("*a*a*b*", "ab")); // нужен 'b' после второй 'a'
}
