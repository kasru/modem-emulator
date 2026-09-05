cmake_minimum_required(VERSION 3.20)

project(atmodem
    VERSION 1.0.0
    DESCRIPTION "AT-командный сервер: эмулятор модема на tty-устройстве"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)

# ---------------------------------------------------------------------------
# Библиотека с основной логикой (используется и сервером, и тестами)
# ---------------------------------------------------------------------------
add_library(atmodem_core STATIC
    src/pattern_matcher.cpp
    src/command_dictionary.cpp
    src/line_assembler.cpp
    src/serial_port.cpp
    src/at_server.cpp
    src/log.cpp
)

target_include_directories(atmodem_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Требуем C++23 явно на уровне цели: флаг достанется и серверу, и тестам,
# и любому другому проекту, подключившему atmodem_core.
target_compile_features(atmodem_core PUBLIC cxx_std_23)

find_package(Threads REQUIRED)
target_link_libraries(atmodem_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Исполняемый файл сервера
# ---------------------------------------------------------------------------
add_executable(atmodem src/main.cpp)
target_link_libraries(atmodem PRIVATE atmodem_core)

# ---------------------------------------------------------------------------
# Тесты
# ---------------------------------------------------------------------------
enable_testing()

add_executable(atmodem_tests
    tests/test_main.cpp
    tests/test_pattern_matcher.cpp
    tests/test_command_dictionary.cpp
    tests/test_line_assembler.cpp
    tests/test_log.cpp
    tests/test_at_server_pty.cpp
)
target_link_libraries(atmodem_tests PRIVATE atmodem_core)

add_test(NAME atmodem_tests COMMAND atmodem_tests)

# ---------------------------------------------------------------------------
# Установка
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS atmodem RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/modem_commands.csv DESTINATION ${CMAKE_INSTALL_DATADIR}/atmodem)
