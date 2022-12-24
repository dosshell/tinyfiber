add_library(tinyfiber tinyfiber.cpp tinyfiber.h tinyringbuffer.hpp)

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GT")
endif ()

target_include_directories(tinyfiber PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if (MSVC)
    target_link_libraries(tinyfiber LINK_PUBLIC Kernel32.lib)
endif ()