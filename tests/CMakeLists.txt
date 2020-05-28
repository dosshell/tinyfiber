add_executable(tinyfiber-test tinyfiber_test.cpp doctest.hpp)
target_link_libraries(tinyfiber-test LINK_PUBLIC tinyfiber)
