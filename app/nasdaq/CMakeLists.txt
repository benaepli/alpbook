add_executable(alpdaq_app
        src/main.cpp
)

target_link_libraries(alpdaq_app
        PRIVATE
        alpdaq
        tomlplusplus::tomlplusplus
)

target_compile_features(alpdaq_app PRIVATE cxx_std_23)

set_target_properties(alpdaq_app PROPERTIES OUTPUT_NAME alpdaq)
