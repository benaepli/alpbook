add_library(alpdaq)

target_sources(alpdaq
        PUBLIC
        FILE_SET CXX_MODULES FILES
        src/alpdaq/alpdaq.cppm
        src/alpdaq/config.cppm
        src/alpdaq/internal.cppm
        src/alpdaq/internal/overloaded.cppm
        src/alpdaq/logging.cppm
        src/alpdaq/logging/file.cppm
        src/alpdaq/logging/logger.cppm
        src/alpdaq/logging/messages.cppm
        src/alpdaq/network.cppm
        src/alpdaq/network/af_xdp.cppm
        src/alpdaq/placement.cppm
        src/alpdaq/simulated.cppm
        src/alpdaq/simulated/binary.cppm
        src/alpdaq/system.cppm
        src/alpdaq/system/container.cppm
        src/alpdaq/system/container/container.cppm
        src/alpdaq/system/container/strategized.cppm
        src/alpdaq/system/state.cppm
)

target_link_libraries(alpdaq
        PUBLIC
        alpbook
        PRIVATE
        tomlplusplus::tomlplusplus
        readerwriterqueue
        hwloc
        spdlog::spdlog
)

target_compile_features(alpdaq PUBLIC cxx_std_23)