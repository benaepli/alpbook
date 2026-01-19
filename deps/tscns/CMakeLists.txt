cmake_minimum_required(VERSION 3.14)

project(tscns LANGUAGES CXX)

add_library(tscns INTERFACE)
add_library(tscns::tscns ALIAS tscns)

target_include_directories(tscns INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(tscns INTERFACE cxx_std_11)