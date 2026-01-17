include(CTest)

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
FetchContent_MakeAvailable(googletest)

add_executable(alpbook_test)

target_link_libraries(alpbook_test
        PRIVATE
        alpbook
        GTest::gtest
        GTest::gtest_main
)

target_sources(alpbook_test
        PRIVATE
        src/internal/pool.cpp
        src/book/nasdaq.cpp
)

add_test(NAME alpbook_test COMMAND alpbook_test)