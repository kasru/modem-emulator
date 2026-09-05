#include "pattern_matcher.hpp"

namespace atmodem {

namespace {

/// @brief Приводит ASCII-букву к нижнему регистру (прочие символы не меняет).
[[nodiscard]] char fold(char c) noexcept
{
    const auto uc = static_cast<unsigned char>(c);
    if (uc >= 'A' && uc <= 'Z') {
        return static_cast<char>(uc - 'A' + 'a');
    }
    return c;
}

/**
 * @brief Описание символьного класса `[...]`, разобранное из шаблона.
 */
struct ClassInfo {
    bool valid;   ///< true, если класс синтаксически корректен (есть закрывающая `]`).
    bool negated; ///< true для формы `[^...]`.
    std::string_view body; ///< Содержимое между скобками (без учёта `^`).
};

/**
 * @brief Разбирает символьный класс, начиная с позиции открывающей скобки.
 *
 * @param pattern Шаблон целиком.
 * @param openPos Позиция символа `[` в шаблоне.
 *
 * @return Разобранное описание класса; при отсутствии закрывающей `]`
 *         возвращается info с valid == false.
 */
[[nodiscard]] ClassInfo parseClass(std::string_view pattern, std::size_t openPos) noexcept
{
    const std::size_t bodyStart = openPos + 1;
    std::size_t i = bodyStart;
    bool negated = false;
    if (i < pattern.size() && pattern[i] == '^') {
        negated = true;
        ++i;
    }
    // Ищем закрывающую скобку.
    while (i < pattern.size() && pattern[i] != ']') {
        ++i;
    }
    if (i >= pattern.size()) {
        return {false, false, {}};
    }
    std::string_view body = pattern.substr(bodyStart, i - bodyStart);
    if (negated && !body.empty()) {
        body.remove_prefix(1); // убираем '^' из тела
    }
    return {true, negated, body};
}

/**
 * @brief Проверяет принадлежность символа телу символьного класса.
 *
 * Поддерживает перечисления (`[abc]`) и диапазоны (`[a-z0-9]`).
 * Символ `-` в начале или в конце тела считается литералом.
 *
 * @param body Тело класса (содержимое между скобками).
 * @param c    Уже приведённый к нижнему регистру символ.
 *
 * @return true, если символ входит в класс.
 */
[[nodiscard]] bool classContains(std::string_view body, char c) noexcept
{
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char lo = fold(body[i]);
        // Диапазон "x-y": '-' не первый и не последний символ тела.
        if (i + 2 < body.size() && body[i + 1] == '-') {
            const char hi = fold(body[i + 2]);
            if (static_cast<unsigned char>(lo) <= static_cast<unsigned char>(c)
                && static_cast<unsigned char>(c) <= static_cast<unsigned char>(hi)) {
                return true;
            }
            // Обратный диапазон (lo > hi) пуст как диапазон, поэтому его
            // концы сравниваются как литералы: "[z-a]" совпадает с 'z' и 'a'.
            if (static_cast<unsigned char>(lo) > static_cast<unsigned char>(hi)
                && (lo == c || hi == c)) {
                return true;
            }
            i += 2;
            continue;
        }
        if (lo == c) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Пытается сопоставить один элемент шаблона с одним символом текста.
 *
 * @param[in]     pattern Шаблон.
 * @param[in,out] p       Позиция в шаблоне; при успехе сдвигается за элемент.
 * @param         c       Приведённый к нижнему регистру символ текста.
 *
 * @return true, если элемент шаблона соответствует символу @p c.
 */
[[nodiscard]] bool elementMatches(std::string_view pattern, std::size_t& p, char c) noexcept
{
    const char pc = pattern[p];
    if (pc == '.') { // любой одиночный символ
        ++p;
        return true;
    }
    if (pc == '[') { // символьный класс
        const ClassInfo cls = parseClass(pattern, p);
        if (!cls.valid) { // незакрытая '[' — обычный литерал
            ++p;
            return c == '[';
        }
        p += (cls.negated ? 2 : 1) + cls.body.size() + 1; // прыжок за ']'
        return classContains(cls.body, c) != cls.negated;
    }
    ++p;
    return fold(pc) == c; // литерал сравнивается без учёта регистра
}

} // namespace

bool patternMatches(std::string_view pattern, std::string_view text) noexcept
{
    std::size_t p = 0;             // позиция в шаблоне
    std::size_t s = 0;             // позиция в тексте
    std::size_t starPos = std::string_view::npos; // позиция последнего '*'
    std::size_t starMatch = 0;     // позиция текста на момент встречи '*'

    while (s < text.size()) {
        const char c = fold(text[s]);
        if (p < pattern.size() && pattern[p] == '*') {
            // Запоминаем '*' и пробуем минимальное покрытие (ноль символов).
            starPos = p++;
            starMatch = s;
        } else if (p < pattern.size() && elementMatches(pattern, p, c)) {
            ++s;
        } else if (starPos != std::string_view::npos) {
            // Откат: расширяем покрытие '*' на один символ и продолжаем.
            p = starPos + 1;
            s = ++starMatch;
        } else {
            return false;
        }
    }

    // Хвост шаблона должен состоять только из '*', чтобы покрыть конец текста.
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

} // namespace atmodem
