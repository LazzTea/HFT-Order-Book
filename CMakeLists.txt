cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0135 NEW)
project(HFT_Order_Book CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release build: max optimisation, no debug symbols
# Debug build:   sanitizers, assertions

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -DNDEBUG)
else()
    if(NOT MSVC)
        add_compile_options(-O0 -g -fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# For Google Testing
include(FetchContent)

FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/heads/main.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(googletest)
include(GoogleTest)

add_executable(run_tests
        tests/test_EventLog.cpp
        tests/test_OrderEvent.cpp
        tests/test_OrderManager.cpp
        tests/test_PositionTracker.cpp
        tests/test_RiskChecker.cpp
)

target_include_directories(run_tests PRIVATE include)

target_link_libraries(run_tests
        GTest::gtest
        GTest::gtest_main
)

enable_testing()

include(GoogleTest)

set_target_properties(run_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

gtest_discover_tests(run_tests
        DISCOVERY_MODE PRE_TEST
)


# Benchmark target (add later)
# add_executable(bench bench/bench_main.cpp)
# target_include_directories(bench PRIVATE include)

