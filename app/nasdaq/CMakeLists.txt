add_executable(alpdaq
        src/main.cpp
)

target_sources(alpdaq
        PUBLIC
        FILE_SET CXX_MODULES FILES
        src/alpdaq.cppm
        src/logging.cppm
        src/logging/logger.cppm
        src/logging/messages.cppm
        src/placement.cppm
)

target_link_libraries(alpdaq
        PRIVATE
        alpbook
        tomlplusplus::tomlplusplus
        readerwriterqueue
        hwloc
)

target_compile_features(alpdaq PRIVATE cxx_std_23)
