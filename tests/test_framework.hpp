#pragma once

#include <cstdio>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

/**
 * @file test_framework.hpp
 * @brief Минималистичный каркас модульных тестов (без внешних зависимостей).
 *
 * Тест объявляется макросом MINITEST_TEST(имя) { ... } и регистрируется
 * автоматически. Проверки выполняются макросами CHECK(cond) и CHECK_EQ(a, b);
 * при невыполнении проверки тест завершается ошибкой с указанием места.
 */

namespace minibest {

/// @brief Сообщение о провале последней проверки внутри активного теста.
inline std::stringstream& failureStream()
{
    static std::stringstream stream;
    return stream;
}

/// @brief Исключение, выбрасываемое при провале проверки.
struct CheckError : std::exception {
    /// @brief Принимает готовый текст ошибки.
    explicit CheckError(std::string message) : message_(std::move(message)) {}

    /// @brief Текст ошибки для отчёта.
    [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_; ///< Полный текст сообщения об ошибке.
};

/// @brief Тип одной зарегистрированной тестовой функции.
using TestFn = void (*)();

/// @brief Единый реестр тестов (наполняется статическими регистраторами).
[[nodiscard]] inline std::vector<std::pair<std::string, TestFn>>& registry()
{
    static std::vector<std::pair<std::string, TestFn>> tests;
    return tests;
}

/// @brief Автоматический регистратор теста в реестре.
struct Registrar {
    /**
     * @brief Добавляет тест в общий реестр.
     *
     * @param name Имя теста.
     * @param fn   Тестируемая функция.
     */
    Registrar(const char* name, TestFn fn) { registry().emplace_back(name, fn); }
};

/// @brief Запускает все зарегистрированные тесты и печатает сводку.
/**
 * @return Количество провалившихся тестов.
 */
inline int runAll()
{
    int failed = 0;
    for (const auto& [name, fn] : registry()) {
        failureStream().str({});
        failureStream().clear();
        try {
            fn();
            std::printf("[ OK ] %s\n", name.c_str());
        } catch (const CheckError& e) {
            ++failed;
            std::printf("[FAIL] %s\n       %s\n", name.c_str(), e.what());
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[FAIL] %s\n       непредвиденное исключение: %s\n",
                        name.c_str(), e.what());
        }
    }
    const std::size_t total = registry().size();
    std::printf("----\n%zu тестов, провалено: %d\n", total, failed);
    return failed;
}

} // namespace minibest

/** @cond internal */
#define MINITEST_CHECK_IMPL(condText, cond)                                                    \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::ostringstream mt_oss;                                                         \
            mt_oss << __FILE__ << ':' << __LINE__ << ": проверка " << condText << " не выполнена"; \
            throw minibest::CheckError(mt_oss.str());                                          \
        }                                                                                      \
    } while (false)

#define MINITEST_EQ_IMPL(aText, bText, a, b)                                                   \
    do {                                                                                       \
        decltype(auto) mt_a = (a);                                                             \
        decltype(auto) mt_b = (b);                                                             \
        if (!(mt_a == mt_b)) {                                                                 \
            std::ostringstream mt_oss;                                                         \
            mt_oss << __FILE__ << ':' << __LINE__ << ": ожидалось " << aText << " == "         \
                   << bText << "; фактические значения: '" << mt_a << "' и '" << mt_b << "'";  \
            throw minibest::CheckError(mt_oss.str());                                          \
        }                                                                                      \
    } while (false)

#define MINITEST_CONCAT_(a, b) a##b
#define MINITEST_CONCAT(a, b) MINITEST_CONCAT_(a, b)
/** @endcond */

/**
 * @def CHECK(cond)
 * @brief Проверяет условие; при невыполнении прерывает тест с сообщением.
 *
 * @param cond Проверяемое условие (текст условия попадает в отчёт).
 */
#define CHECK(cond) MINITEST_CHECK_IMPL(#cond, (cond))

/**
 * @def CHECK_EQ(a, b)
 * @brief Проверяет равенство значений; печатает фактические значения при провале.
 *
 * Типы значений должны поддерживать operator== и вывод в std::ostream.
 *
 * @param a Фактическое значение.
 * @param b Ожидаемое значение.
 */
#define CHECK_EQ(a, b) MINITEST_EQ_IMPL(#a, #b, (a), (b))

/**
 * @def MINITEST_TEST(name)
 * @brief Объявляет тест с уникальным именем и автоматически регистрирует его.
 *
 * Тест - это функция без параметров и результата. Регистрация выполняется
 * статическим объектом до входа в main(), поэтому тесты запускаются
 * minibest::runAll() в произвольном порядке.
 *
 * @param name Уникальный в пределах исполняемого файла идентификатор теста
 *             (также используется как имя в отчёте прогона).
 *
 * @par Пример
 * @code
 * /// @brief Краткое описание проверяемого поведения.
 * MINITEST_TEST(my_feature_works)
 * {
 *     CHECK(42 == answer());
 *     CHECK_EQ(make_greeting(), std::string("hello"));
 * }
 * @endcode
 *
 * @note Чтобы doxygen показывал документированные таким образом тесты как
 *       функции, добавьте в Doxyfile:
 * @code
 * MACRO_EXPANSION    = YES
 * EXPAND_ONLY_PREDEF = YES
 * PREDEFINED         = "MINITEST_TEST(name)=void name()"
 * @endcode
 */
#define MINITEST_TEST(name)                                                              \
    static void MINITEST_CONCAT(mt_test_, name)();                                       \
    static const minibest::Registrar MINITEST_CONCAT(mt_reg_, name){#name,               \
                                                                    &MINITEST_CONCAT(mt_test_, name)}; \
    static void MINITEST_CONCAT(mt_test_, name)()
