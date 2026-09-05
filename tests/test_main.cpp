/**
 * @file test_main.cpp
 * @brief Точка входа набора тестов atmodem.
 */

#include "test_framework.hpp"

/**
 * @brief Запускает все зарегистрированные тесты.
 *
 * @return 0 при успехе, 1 при наличии проваленных тестов.
 */
int main()
{
    return minibest::runAll() == 0 ? 0 : 1;
}
