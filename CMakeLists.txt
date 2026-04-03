cmake_minimum_required(VERSION 3.16)
project(hft_bookkeeping CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Compiler flags ────────────────────────────────────────────────────────────

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-function)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native -funroll-loops -DNDEBUG)
else()
    add_compile_options(-O0 -g -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────

find_package(GTest REQUIRED)
include(GoogleTest)

# Both library and headers pinned to /usr/local — the from-source build location.
# This avoids the apt vs source mismatch that causes ABI link errors.
find_library(QUICKFIX_LIBRARY REQUIRED
        NAMES quickfix
        PATHS /usr/local/lib
        NO_DEFAULT_PATH
)
find_path(QUICKFIX_INCLUDE_DIR REQUIRED
        NAMES quickfix/Application.h
        PATHS /usr/local/include
        NO_DEFAULT_PATH
)
add_library(quickfix UNKNOWN IMPORTED)
set_target_properties(quickfix PROPERTIES
        IMPORTED_LOCATION             "${QUICKFIX_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${QUICKFIX_INCLUDE_DIR}"
)

# ── Tests: bookkeeping layer (no QuickFIX dependency) ────────────────────────

set(BOOKKEEPING_TESTS
        tests/test_OrderEvent.cpp
        tests/test_EventLog.cpp
        tests/test_PositionTracker.cpp
        tests/test_OrderManager.cpp
        tests/test_RiskChecker.cpp
)

foreach(src ${BOOKKEEPING_TESTS})
    get_filename_component(target ${src} NAME_WE)
    add_executable(${target} ${src})
    target_include_directories(${target} PRIVATE include)
    target_link_libraries(${target} PRIVATE
            GTest::gtest GTest::gtest_main)
    target_compile_options(${target} PRIVATE
            -O0 -g -fsanitize=address,undefined)
    target_link_options(${target} PRIVATE
            -fsanitize=address,undefined)
    gtest_discover_tests(${target})
endforeach()

# ── Tests: gateway layer ──────────────────────────────────────────────────────

set(GATEWAY_TESTS
        tests/test_MessageTranslator.cpp
        tests/test_FIXGateway.cpp
        tests/test_Killswitch.cpp
)

foreach(src ${GATEWAY_TESTS})
    get_filename_component(target ${src} NAME_WE)
    add_executable(${target} ${src} gateway/FIXGateway.cpp)
    target_include_directories(${target} PRIVATE
            include gateway tests/fixtures)
    target_link_libraries(${target} PRIVATE
            GTest::gtest GTest::gtest_main quickfix)
    # C++14 required: QuickFIX headers use throw() specs removed in C++17
    # set_target_properties(${target} PROPERTIES CXX_STANDARD 14)
    target_compile_options(${target} PRIVATE
            -O0 -g -fsanitize=address,undefined)
    target_link_options(${target} PRIVATE
            -fsanitize=address,undefined)
    gtest_discover_tests(${target})
endforeach()

# ── Gateway executable ────────────────────────────────────────────────────────

add_executable(run_gateway
        src/main.cpp
        gateway/FIXGateway.cpp
)
target_include_directories(run_gateway PRIVATE include gateway)
target_link_libraries(run_gateway PRIVATE quickfix)
# C++14 required: QuickFIX headers use throw() specs removed in C++17
# set_target_properties(run_gateway PROPERTIES CXX_STANDARD 14)
target_compile_options(run_gateway PRIVATE -O2 -g)

# ── Benchmark ─────────────────────────────────────────────────────────────────

add_executable(run_bench benchmarks/bench_main.cpp)
target_include_directories(run_bench PRIVATE include benchmarks)
target_compile_options(run_bench PRIVATE
        -O3 -march=native -funroll-loops -DNDEBUG
        -fno-sanitize=address,undefined)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(run_bench PRIVATE -fno-sanitize=address,undefined)
endif()