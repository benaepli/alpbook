find_package(Threads REQUIRED)

add_executable(alpdaq_app
        src/main.cpp
)

target_sources(alpdaq_app
        PRIVATE
        FILE_SET CXX_MODULES FILES
        src/strategy.cppm
)

target_link_libraries(alpdaq_app
        PRIVATE
        alpdaq
        tomlplusplus::tomlplusplus
        Threads::Threads
)

target_compile_features(alpdaq_app PRIVATE cxx_std_23)

set_target_properties(alpdaq_app PROPERTIES OUTPUT_NAME alpdaq)
