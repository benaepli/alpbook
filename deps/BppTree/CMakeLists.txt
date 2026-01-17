cmake_minimum_required(VERSION 3.14)

project(BppTree LANGUAGES CXX)

add_library(bpptree INTERFACE)
add_library(bpptree::bpptree ALIAS bpptree)

target_include_directories(bpptree INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(bpptree INTERFACE cxx_std_17)
