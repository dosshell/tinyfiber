add_executable(tinyfiber-test tinyfiber_test.cpp tinyringbuffer_test main.cpp doctest.hpp)
target_link_libraries(tinyfiber-test LINK_PUBLIC tinyfiber)
