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

add_executable(flatbook_tail src/TailLatency.cpp)
target_link_libraries(flatbook_tail PRIVATE flatbook)