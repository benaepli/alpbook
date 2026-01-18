cmake_minimum_required(VERSION 3.30)

project(alpbook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# Build options
option(ALPBOOK_BUILD_TESTS "Build test programs" ON)
option(ALPBOOK_BUILD_BENCHMARKS "Build benchmark programs" OFF)
option(ALPBOOK_BUILD_DOCS "Build Doxygen documentation" ON)

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

FetchContent_Declare(
        absl
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG 20240116.2
        FIND_PACKAGE_ARGS
)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_BUILD_MONOLITHIC_TESTS OFF CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(absl)

FetchContent_Declare(
        hwloc
        GIT_REPOSITORY https://github.com/open-mpi/hwloc.git
        GIT_TAG hwloc-2.12.0
        FIND_PACKAGE_ARGS
)
set(HWLOC_SKIP_LSTOPO ON CACHE BOOL "Skip building lstopo")
set(HWLOC_SKIP_TOOLS ON CACHE BOOL "Skip building hwloc tools")
FetchContent_MakeAvailable(hwloc)

FetchContent_Declare(
        readerwriterqueue
        GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue
        GIT_TAG v1.0.7
        FIND_PACKAGE_ARGS
)
FetchContent_MakeAvailable(readerwriterqueue)

add_subdirectory(deps/BppTree)

add_library(alpbook)

target_sources(alpbook
        PUBLIC
        FILE_SET CXX_MODULES FILES
        src/alpbook.cppm
        src/alpbook/book.cppm
        src/alpbook/book/core.cppm
        src/alpbook/book/nasdaq.cppm
        src/alpbook/book/nasdaq-state.cppm
        src/alpbook/common.cppm
        src/alpbook/internal.cppm
        src/alpbook/internal/backoff.cppm
        src/alpbook/internal/pin.cppm
        src/alpbook/internal/pool.cppm
        src/alpbook/itch.cppm
        src/alpbook/itch/dispatch.cppm
)
target_link_libraries(alpbook PRIVATE eve::eve absl::flat_hash_map absl::numeric bpptree::bpptree hwloc readerwriterqueue)
target_compile_features(alpbook PUBLIC cxx_std_23)

if (ALPBOOK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (ALPBOOK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()

if (ALPBOOK_BUILD_DOCS)
    add_subdirectory(docs)
endif ()