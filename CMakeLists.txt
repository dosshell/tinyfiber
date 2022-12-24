cmake_minimum_required(VERSION 3.10)
project(tinyfiber)

set(CMAKE_CXX_STANDARD 17)

if (WIN32)
    set(WINVER 0x0600)
    set(_WIN32_WINNT 0x0600)
endif ()

add_subdirectory(src)
add_subdirectory(tests)
