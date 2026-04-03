add_executable(alpdaq
        src/main.cpp
)

target_sources(alpdaq
        PUBLIC
        FILE_SET CXX_MODULES FILES
        src/alpdaq.cppm
        src/config.cppm
        src/internal.cppm
        src/internal/overloaded.cppm
        src/logging.cppm
        src/logging/file.cppm
        src/logging/logger.cppm
        src/logging/messages.cppm
        src/network.cppm
        src/placement.cppm
        src/simulated.cppm
        src/simulated/binary.cppm
        src/system.cppm
        src/system/state.cppm
)

target_link_libraries(alpdaq
        PRIVATE
        alpbook
        tomlplusplus::tomlplusplus
        readerwriterqueue
        hwloc
        spdlog::spdlog
)

target_compile_features(alpdaq PRIVATE cxx_std_23)
