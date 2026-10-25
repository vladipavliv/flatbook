if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "FLATBOOK_BUILD_FUZZER requires Clang (libFuzzer)")
endif()

add_executable(flatbook_fuzz fuzz_matching.cpp)
target_link_libraries(flatbook_fuzz PRIVATE flatbook)
target_compile_options(flatbook_fuzz PRIVATE
    -O1 -g
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
)
target_link_options(flatbook_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined
)
