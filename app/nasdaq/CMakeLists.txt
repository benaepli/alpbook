add_executable(alpbook_nasdaq
        src/main.cpp
)

target_link_libraries(alpbook_nasdaq
        PRIVATE
        alpbook
        tomlplusplus::tomlplusplus
)

target_compile_features(alpbook_nasdaq PRIVATE cxx_std_23)
