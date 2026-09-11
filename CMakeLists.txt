cmake_minimum_required(VERSION 3.20)
project(cosy VERSION 0.1.0 LANGUAGES CXX)

option(COSY_BUILD_TESTS "Build Cosy integration tests" ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Cosy supports Linux only.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDW REQUIRED IMPORTED_TARGET libdw)
pkg_check_modules(LIBELF REQUIRED IMPORTED_TARGET libelf)
pkg_check_modules(READLINE REQUIRED IMPORTED_TARGET readline)
find_package(Threads REQUIRED)

add_executable(cosy
    src/main.cpp
    src/architecture.cpp
    src/architecture_x86_64.cpp
    src/architecture_aarch64.cpp
    src/elf_dwarf.cpp
    src/debugger.cpp
)

target_compile_features(cosy PRIVATE cxx_std_20)
target_compile_options(cosy PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_include_directories(cosy PRIVATE include)
target_link_libraries(cosy PRIVATE
    PkgConfig::LIBDW
    PkgConfig::LIBELF
    PkgConfig::READLINE
    Threads::Threads
    dl
)

include(CTest)
if(BUILD_TESTING AND COSY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
