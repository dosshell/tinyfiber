find_package(Threads REQUIRED)
add_executable(tinyfiber-test tinyfiber_test.cpp tinyringbuffer_test.cpp main.cpp doctest.hpp)

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GT")
endif ()

target_link_libraries(tinyfiber-test LINK_PUBLIC tinyfiber)
target_link_libraries(tinyfiber-test PRIVATE Threads::Threads)