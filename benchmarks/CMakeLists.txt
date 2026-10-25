include(FetchContent)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.4
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

# Liquibook (header-only matching engine, BSD-style license). Pinned commit.
FetchContent_Declare(
    liquibook
    GIT_REPOSITORY https://github.com/objectcomputing/liquibook.git
    GIT_TAG 2427613b32f1667abae68a01df6af9ba8270f8e7
)
FetchContent_MakeAvailable(liquibook)

add_executable(flatbook_bench
    src/BenchCppTrader.cpp
    src/BenchOrderBook.cpp
)
target_link_libraries(flatbook_bench PRIVATE flatbook benchmark::benchmark)
target_include_directories(flatbook_bench SYSTEM PRIVATE ${liquibook_SOURCE_DIR}/src)

# TailLatency is x86-only (__rdtsc / <x86intrin.h>).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|i[3-6]86")
    add_executable(flatbook_tail src/TailLatency.cpp)
    target_link_libraries(flatbook_tail PRIVATE flatbook)
endif()

add_executable(flatbook_dispatcher_bench src/BenchDispatcher.cpp)
target_link_libraries(flatbook_dispatcher_bench PRIVATE flatbook)

add_executable(flatbook_lobster_bench src/BenchLobster.cpp)
target_link_libraries(flatbook_lobster_bench PRIVATE flatbook)

add_executable(flatbook_validate_lobster src/ValidateLobster.cpp)
target_link_libraries(flatbook_validate_lobster PRIVATE flatbook)