cmake_minimum_required(VERSION 3.16)
project(HFT_Order_Book CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release build: max optimisation, no debug symbols
# Debug build:   sanitizers, assertions
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native -funroll-loops -DNDEBUG)
else()
    add_compile_options(-O0 -g -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

include_directories(include)

add_executable(run_tests tests/test_main.cpp)
target_include_directories(run_tests PRIVATE include)

add_executable(run_simulation tests/test_simulation.cpp
        OrderManager.h)
target_include_directories(run_simulation PRIVATE include)


# Benchmark target (add later)
# add_executable(bench bench/bench_main.cpp)
# target_include_directories(bench PRIVATE include)

