cmake_minimum_required(VERSION 3.10)
project(HelloProject C)

set(CMAKE_C_STANDARD 11)

add_executable(hello main.c mathutils.c)