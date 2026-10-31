from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class FlatbookConan(ConanFile):
    name = "flatbook"
    version = "0.3.0"
    license = "MIT"
    author = "Vladimir Pavliv <voldodimir@gmail.com>"
    url = "https://github.com/vladipavliv/flatbook"
    description = "Header-only, allocation-free, deterministic order book and matching engine for C++20."
    topics = ("order-book", "matching-engine", "low-latency", "header-only", "cpp20")
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"
    exports_sources = "include/*", "cmake/*", "CMakeLists.txt"
    no_copy_source = True

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure(
            variables={
                # Benchmarks/examples/tests are not exported; build the library only.
                "FLATBOOK_BUILD_BENCHMARKS": "OFF",
                "FLATBOOK_BUILD_EXAMPLES": "OFF",
                "FLATBOOK_BUILD_TESTS": "OFF",
                "FLATBOOK_BUILD_FUZZER": "OFF",
            }
        )
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_id(self):
        self.info.clear()  # header-only: no binary settings

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "flatbook")
        self.cpp_info.set_property("cmake_target_name", "flatbook::flatbook")
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
