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
