cmake_minimum_required(VERSION 3.25)

project(FlatBook
    VERSION 0.1.0
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(FLATBOOK_BUILD_TESTS "Build tests" OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(FLATBOOK_BUILD_BENCHMARKS_DEFAULT ON)
else()
    set(FLATBOOK_BUILD_BENCHMARKS_DEFAULT OFF)
endif()
option(FLATBOOK_BUILD_BENCHMARKS "Build benchmarks" ${FLATBOOK_BUILD_BENCHMARKS_DEFAULT})

add_library(flatbook INTERFACE)
add_library(flatbook::flatbook ALIAS flatbook)

target_include_directories(flatbook
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(flatbook
    INTERFACE
        cxx_std_23
)

if(FLATBOOK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FLATBOOK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# --- Install / find_package -------------------------------------------------

install(TARGETS flatbook EXPORT flatbookTargets)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT flatbookTargets
        FILE flatbookTargets.cmake
        NAMESPACE flatbook::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flatbook)

configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/flatbookConfig.cmake.in
    ${PROJECT_BINARY_DIR}/flatbookConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flatbook)

write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/flatbookConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)

install(FILES
    ${PROJECT_BINARY_DIR}/flatbookConfig.cmake
    ${PROJECT_BINARY_DIR}/flatbookConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flatbook)
