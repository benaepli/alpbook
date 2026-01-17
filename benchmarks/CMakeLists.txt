include(FetchContent)
FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.4
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(alpbook_benchmarks
        src/book/nasdaq.cpp
)

target_link_libraries(alpbook_benchmarks
        PRIVATE
        alpbook
        benchmark::benchmark_main
)

target_compile_features(alpbook_benchmarks PRIVATE cxx_std_23)

target_compile_options(alpbook_benchmarks PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
