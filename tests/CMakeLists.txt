add_executable(tinyfiber-test tinyfiber_test.cpp tinyringbuffer_test main.cpp doctest.hpp)

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GT")
endif ()

target_link_libraries(tinyfiber-test LINK_PUBLIC tinyfiber)