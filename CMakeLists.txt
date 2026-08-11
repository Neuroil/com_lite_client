cmake_minimum_required(VERSION 3.14)
project(com_lite_custom)

set(CMAKE_CXX_STANDARD 17)

add_library(client_base INTERFACE)

target_include_directories(client_base INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_options(client_base INTERFACE /utf-8)

add_executable(client main.cpp)
target_link_libraries(client PRIVATE client_base)