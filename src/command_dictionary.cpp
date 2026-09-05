#include "command_dictionary.hpp"

#include "pattern_matcher.hpp"

#include <algorithm>
#include <fstream>
#include <ranges>
#include <sstream>

namespace atmodem {

namespace {

/// @brief Удаляет пробельные символы с обоих концов строки.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept
{
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    };
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

/**
 * @brief Разбирает одну строку формата `"expect","answer"` (CSV с кавычками).
 *
 * @param line Строка целиком (начинается с `"`).
 * @param[out] pattern Сюда помещается разобранное ожидание.
 * @param[out] answer  Сюда помещается разобранный ответ.
 *
 * @return Пустая строка при успехе, иначе текст ошибки.
 */
std::string parseQuotedCsv(std::string_view line,
                           std::string& pattern,
                           std::string& answer)
{
    enum class State { Outside, InQuote, QuoteClosed } state = State::Outside;
    std::size_t field = 0;
    std::string current;

    const auto finishField = [&]() -> std::string {
        if (field == 0) {
            pattern = CommandDictionary::expandEscapes(trim(current));
        } else {
            return "лишние поля после второго";
        }
        ++field;
        return {};
    };

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        switch (state) {
        case State::Outside:
            if (c == '"') { // открывающая кавычка очередного поля
                state = State::InQuote;
                current.clear();
            } else if (c == ',') {
                ++field; // допускаем пропуск пустых позиций
            } else if (c != ' ' && c != '\t') { // пробелы между полями допустимы
                // Посторонний текст вне кавычек (например, "AT",мусор,"OK")
                // - ошибка формата, а не молчаливо пропущенный символ.
                return "текст вне кавычек между полями";
            }
            break;
        case State::InQuote:
            if (c == '\\' && i + 1 < line.size()) {
                // Бэкслэш-экранирование (единая модель с простой формой):
                // следующий символ не закрывает кавычку; expandEscapes()
                // позже развернёт \" в литеральную кавычку.
                current.push_back(c);
                current.push_back(line[++i]); // вот здесь нужен индекс: ++i съедает экранированный символ
            } else if (c == '"') {
                state = State::QuoteClosed; // возможно закрытие или удвоение
            } else {
                current.push_back(c);
            }
            break;
        case State::QuoteClosed:
            if (c == '"') { // удвоенная кавычка -> литерал
                current.push_back('"');
                state = State::InQuote;
            } else if (c == ',') {
                if (std::string err = finishField(); !err.empty()) {
                    return err;
                }
                state = State::Outside;
            } else if (c != ' ' && c != '\t') { // пробелы между полями допустимы
                return "текст вне кавычек внутри поля";
            }
            break;
        }
    }

    if (state == State::InQuote) {
        return "незакрытая кавычка";
    }
    // Корректная запись: поле-ожидание закрыто запятой (field стал >= 1),
    // затем идёт поле-ответ, завершённое кавычкой (состояние QuoteClosed).
    if (state != State::QuoteClosed || field == 0) {
        return "ожидается два поля в кавычках";
    }
    answer = CommandDictionary::expandEscapes(current);
    return {};
}

} // namespace

std::string CommandDictionary::expandEscapes(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\' || i + 1 >= raw.size()) {
            out.push_back(raw[i]);
            continue;
        }
        switch (raw[++i]) {
        case 'r': out.push_back('\r'); break;
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case '\\': out.push_back('\\'); break;
        case '=': out.push_back('='); break;
        case '"': out.push_back('"'); break;
        default: out.push_back(raw[i]); break; // неизвестный escape: берём символ как есть
        }
    }
    return out;
}

void CommandDictionary::addRule(std::string pattern, std::string answer)
{
    rules_.push_back(Rule{std::move(pattern), std::move(answer)});
}

std::expected<CommandDictionary, std::string>
CommandDictionary::loadFromText(std::string_view text, const std::string& sourceName)
{
    CommandDictionary dict;
    std::size_t lineNo = 0;

    for (const auto&& piece : std::views::split(text, '\n')) {
        ++lineNo;
        std::string_view line{piece.begin(), piece.end()};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue; // пустая строка или комментарий
        }

        const auto fail = [&](std::string_view reason) {
            return std::unexpected(sourceName + ':' + std::to_string(lineNo)
                                   + ": " + std::string(reason));
        };

        Rule rule;
        if (line.front() == '"') {
            if (std::string err = parseQuotedCsv(line, rule.pattern, rule.answer); !err.empty()) {
                return fail(err);
            }
        } else {
            // Простой формат: до первого неэкранированного '='.
            std::string rawPattern;
            std::size_t separator = std::string_view::npos;
            for (std::size_t i = 0; i < line.size(); ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    rawPattern.push_back(line[i]);
                    rawPattern.push_back(line[++i]);
                    continue;
                }
                if (line[i] == '=') {
                    separator = i;
                    break;
                }
                rawPattern.push_back(line[i]);
            }
            if (separator == std::string_view::npos) {
                return fail("нет разделителя '='");
            }
            rule.pattern = expandEscapes(trim(rawPattern));
            rule.answer = expandEscapes(line.substr(separator + 1));
        }

        if (rule.pattern.empty()) {
            return fail("пустое ожидание (шаблон)");
        }
        dict.addRule(rule.pattern, rule.answer);
    }

    if (dict.rules_.empty()) {
        return std::unexpected(sourceName + ": словарь не содержит ни одного правила");
    }
    return dict;
}

std::expected<CommandDictionary, std::string>
CommandDictionary::loadFromFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected("не удалось открыть файл: " + path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return loadFromText(buffer.str(), path);
}

std::expected<CommandDictionary, std::string> CommandDictionary::loadBuiltin()
{
    // Демо-словарь на случай отсутствия файла (см. main.cpp).
    constexpr std::string_view kBuiltin =
        "AT=OK\n"
        "ATE[01]=OK\n"
        "ATI=Manufacturer: AtModem\\r\\nModel: AT-MODEM-1\\r\\nOK\n"
        "AT+COPS?=+COPS: 0,0,\"BuiltinNet\",7\\r\\nOK\n"
        "AT+CPIN?=+CPIN: READY\\r\\nOK\n";
    return loadFromText(kBuiltin, "<builtin>");
}

const CommandDictionary::Rule*
CommandDictionary::find(std::string_view command) const noexcept
{
    for (const Rule& rule : rules_) {
        if (patternMatches(rule.pattern, command)) {
            return &rule;
        }
    }
    return nullptr;
}

} // namespace atmodem
