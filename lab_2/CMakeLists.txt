cmake_minimum_required(VERSION 3.10)
project(Lab2 C)

set(CMAKE_C_STANDARD 11)

add_library(procrun procrun.c)
target_include_directories(procrun PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(proc_test test.c)
target_link_libraries(proc_test procrun)