find_package(PkgConfig REQUIRED)
pkg_check_modules(libbpf REQUIRED IMPORTED_TARGET libbpf)
pkg_check_modules(libxdp REQUIRED IMPORTED_TARGET libxdp)
pkg_check_modules(liburing REQUIRED IMPORTED_TARGET liburing)

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
        src/alpdaq/source.cppm
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
        PkgConfig::libbpf
        PkgConfig::libxdp
        PkgConfig::liburing
)

target_compile_features(alpdaq PUBLIC cxx_std_23)