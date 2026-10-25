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

add_executable(flatbook_bench
    src/BenchCppTrader.cpp
    src/BenchOrderBook.cpp
)
target_link_libraries(flatbook_bench PRIVATE flatbook benchmark::benchmark)

# TailLatency is x86-only (__rdtsc / <x86intrin.h>).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|i[3-6]86")
    add_executable(flatbook_tail src/TailLatency.cpp)
    target_link_libraries(flatbook_tail PRIVATE flatbook)
endif()

add_executable(flatbook_dispatcher_bench src/BenchDispatcher.cpp)
target_link_libraries(flatbook_dispatcher_bench PRIVATE flatbook)

add_executable(flatbook_lobster_bench src/BenchLobster.cpp)
target_link_libraries(flatbook_lobster_bench PRIVATE flatbook)