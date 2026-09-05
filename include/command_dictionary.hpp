#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atmodem {

/**
 * @brief Словарь "ожидание = ответ" для AT-сервера.
 *
 * Загружается из текстового файла (или строки), где каждая значащая строка
 * описывает одно правило в одном из двух форматов.
 *
 * @par Формат 1 — простой: @c expect=answer
 * Правило разделяется по ПЕРВОМУ неэкранированному символу `=`. Если символ
 * `=` нужен внутри ожидания, он экранируется как `\\=`:
 * @code
 * AT=OK
 * ATE[01]=OK
 * AT+CPIN\=[0-9][0-9][0-9][0-9]=OK
 * @endcode
 *
 * @par Формат 2 — CSV с кавычками: @c "expect","answer"
 * Полезно, когда и ожидание, и ответ содержат `=`, запятые или кавычки
 * (кавычки внутри поля удваиваются):
 * @code
 * "AT+COPS=?","+COPS: (2,""OpenMobile"",""25099"",7)"
 * @endcode
 *
 * Дополнительно:
 *   - пустые строки и строки-комментарии, начинающиеся с `#`, игнорируются;
 *   - в обеих частях поддерживаются escape-последовательности
 *     `\\r`, `\\n`, `\\t`, `\\\\`, `\\=`, `\\"` (неизвестная последовательность
 *     трактуется как сам следующий за `\` символ);
 *   - правила проверяются в порядке объявления, побеждает первое совпавшее;
 *   - сравнение шаблонов выполняется функцией patternMatches без std::regex.
 */
class CommandDictionary {
public:
    /// @brief Одно правило словаря: шаблон команды и текст ответа.
    struct Rule {
        std::string pattern; ///< Ожидание (шаблон ограниченного синтаксиса).
        std::string answer;  ///< Ответ модема (без обрамления CR/LF).
    };

    /**
     * @brief Загружает словарь из файла.
     *
     * @param path Путь к текстовому/CSV файлу правил.
     *
     * @return Готовый словарь либо текст ошибки с номером проблемной строки.
     */
    [[nodiscard]] static std::expected<CommandDictionary, std::string>
    loadFromFile(const std::string& path);

    /**
     * @brief Загружает встроенный минимальный словарь (демо-режим).
     *
     * Запасной вариант, когда файл словаря недоступен. Покрывает базовый
     * набор: AT, ATE[01], ATI, AT+COPS?, AT+CPIN?.
     *
     * @return Готовый словарь; встроенный набор считается корректным.
     */
    [[nodiscard]] static std::expected<CommandDictionary, std::string> loadBuiltin();

    /**
     * @brief Загружает словарь из строки в памяти (удобно для тестов).
     *
     * @param text       Содержимое словаря; переводы строк LF или CRLF.
     * @param sourceName Имя источника для сообщений об ошибках.
     *
     * @return Готовый словарь либо текст ошибки с номером проблемной строки.
     */
    [[nodiscard]] static std::expected<CommandDictionary, std::string>
    loadFromText(std::string_view text, const std::string& sourceName = "<memory>");

    /**
     * @brief Ищет первое правило, шаблон которого полностью соответствует команде.
     *
     * @param command Командная строка без терминатора CR (регистр не важен).
     *
     * @return Указатель на найденное правило (живёт, пока жив словарь)
     *         или nullptr, если ничего не подошло.
     */
    [[nodiscard]] const Rule* find(std::string_view command) const noexcept;

    /**
     * @brief Список всех правил в порядке объявления.
     *
     * @return Константная ссылка на внутренний вектор правил.
     */
    [[nodiscard]] const std::vector<Rule>& rules() const noexcept { return rules_; }

    /**
     * @brief Разворачивает escape-последовательности в строке правила.
     *
     * Поддерживаются `\\r`, `\\n`, `\\t`, `\\\\`, `\\=`, `\\"`;
     * неизвестная последовательность даёт сам символ после `\`.
     *
     * @param raw Строка с возможными escape-последовательностями.
     *
     * @return Строка после разворачивания escape-последовательностей.
     */
    [[nodiscard]] static std::string expandEscapes(std::string_view raw);

private:
    /// @brief Добавляет правило в конец списка (порядок = приоритет поиска).
    void addRule(std::string pattern, std::string answer);

    std::vector<Rule> rules_; ///< Упорядоченный набор правил.
};

} // namespace atmodem
