find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)

# Locate the nanobind CMake package (installed via pip).
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_VARIABLE nanobind_cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE)
list(PREPEND CMAKE_PREFIX_PATH "${nanobind_cmake_dir}")
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(flatbook_py src/flatbook_py.cpp)
target_link_libraries(flatbook_py PRIVATE flatbook)

# Install the extension into the wheel's platlib (scikit-build-core sets
# SKBUILD_PLATLIB_DIR); a plain CMake build uses the in-tree module instead.
if(DEFINED SKBUILD_PLATLIB_DIR AND NOT SKBUILD_PLATLIB_DIR STREQUAL "")
    install(TARGETS flatbook_py LIBRARY DESTINATION ${SKBUILD_PLATLIB_DIR})
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/flatbook_py.pyi DESTINATION ${SKBUILD_PLATLIB_DIR})
endif()
