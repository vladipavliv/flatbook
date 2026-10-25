if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "FLATBOOK_BUILD_FUZZER requires Clang (libFuzzer)")
endif()

function(flatbook_add_fuzz_target name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE flatbook)
    target_compile_options(${name} PRIVATE
        -O1 -g
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(${name} PRIVATE
        -fsanitize=fuzzer,address,undefined
    )
endfunction()

flatbook_add_fuzz_target(flatbook_fuzz fuzz_matching.cpp)
flatbook_add_fuzz_target(flatbook_fuzz_persist fuzz_persist.cpp)
flatbook_add_fuzz_target(flatbook_fuzz_lobster_line fuzz_lobster_line.cpp)
