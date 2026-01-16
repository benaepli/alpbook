cmake_minimum_required(VERSION 3.30)

project(alpbook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# Build options
option(ALPBOOK_BUILD_TESTS "Build test programs" ON)
option(ALPBOOK_BUILD_BENCHMARKS "Build benchmark programs" OFF)
option(ALPBOOK_BUILD_DOCS "Build Doxygen documentation" ON)


option(ALP_USE_EXPERIMENTAL_SIMD "Use std::experimental::simd" OFF)

include(FetchContent)
set(EVE_BUILD_DOCUMENTATION OFF)
set(EVE_BUILD_BENCHMARKS OFF)
set(EVE_BUILD_TEST OFF)
set(EVE_BUILD_EXAMPLES OFF)

FetchContent_Declare(
        eve
        URL https://github.com/jfalcou/eve/archive/refs/tags/v2023.02.15.zip
)
FetchContent_MakeAvailable(eve)


add_library(alpbook)

target_sources(alpbook
        PUBLIC
        FILE_SET CXX_MODULES FILES
        src/alpbook.cppm
)
target_link_libraries(alpbook PRIVATE eve::eve)
target_compile_features(alpbook PUBLIC cxx_std_23)

if (ALPBOOK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (ALPBOOK_BUILD_BENCHMARKS)
#    add_subdirectory(benchmarks)
endif ()

if (ALPBOOK_BUILD_DOCS)
    add_subdirectory(docs)
endif ()