add_executable(alpbook_latency_benchmark
        src/main.cpp
)

add_subdirectory("${PROJECT_SOURCE_DIR}/deps/tscns" "${CMAKE_CURRENT_BINARY_DIR}/tscns_build")

target_link_libraries(alpbook_latency_benchmark
        PRIVATE
        alpbook
        tscns::tscns
)

target_compile_features(alpbook_latency_benchmark PRIVATE cxx_std_23)

target_sources(alpbook_latency_benchmark
        PUBLIC
        FILE_SET CXX_MODULES FILES
        src/sink.cppm
        src/strategy.cppm
)