include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

add_executable(flatbook_tests
    src/TestBitmap.cpp
    src/TestDispatcher.cpp
    src/TestIds.cpp
    src/TestJournal.cpp
    src/TestLobster.cpp
    src/TestOrderBook.cpp
    src/TestOrderBookContainer.cpp
    src/TestPersist.cpp
    src/TestProperty.cpp
    src/TestSpsc.cpp
    src/TestWait.cpp
)

target_link_libraries(flatbook_tests
    PRIVATE
        flatbook
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(flatbook_tests)