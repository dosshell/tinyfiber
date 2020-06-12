/*
MIT License

Copyright (c) 2020 Markus Lindelöw

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <tinyfiber.h>

#include "doctest.hpp"

namespace tinyfiber
{
void recursive_job(void* param)
{
    if (param == nullptr)
        return;

    const int64_t no = *(std::atomic_int64_t*)param;
    std::atomic_int64_t* depth = (std::atomic_int64_t*)param;

    (*depth)--;

    if (*depth > 0)
    {
        WaitHandle wh;
        JobDeclaration jd;
        jd.func = recursive_job;
        jd.user_data = param;
        jd.wait_handle = &wh;

        tinyfiber_add_job(jd);
        tinyfiber_await(wh);
    }

    int64_t d = *depth;
    CHECK((int64_t)d == 0);
}

TEST_CASE("tinyfiber null")
{
    // Given
    FiberSystem* fs;
    REQUIRE(tinyfiber_init(&fs) == 0);

    // When
    int sts = tinyfiber_free(&fs);

    // Then
    REQUIRE(sts == 0);
}

TEST_CASE("tinyfiber run 1 core")
{
    //Given
    FiberSystem* fs;
    REQUIRE(tinyfiber_init(&fs, 1) == 0);

    std::atomic_int64_t depth = 512;

    //When
    recursive_job(&depth);

    //Then
    CHECK(depth == 0);

    // Cleanup
    REQUIRE(tinyfiber_free(&fs) == 0);
}

TEST_CASE("tinyfiber run 3 cores")
{
    for (int i = 0; i < 128; ++i)
    {
        //Given
        FiberSystem* fs;
        REQUIRE(tinyfiber_init(&fs, 3) == 0);
        std::atomic_int64_t depth = 3;

        //When
        depth = 3;
        recursive_job(&depth);

        //Then
        CHECK(depth == 0);

        // Cleanup
        REQUIRE(tinyfiber_free(&fs) == 0);
    }
}

TEST_CASE("tinyfiber mt consistency")
{
    // Given
    std::atomic_int64_t depth1 = 512;
    std::atomic_int64_t depth2 = 512;
    int result1 = 0;

    // When
    FiberSystem* fs;
    REQUIRE(tinyfiber_init(&fs, 1) == 0);
    recursive_job(&depth1);
    REQUIRE(tinyfiber_free(&fs) == 0);

    REQUIRE(tinyfiber_init(&fs, 2) == 0);
    recursive_job(&depth2);
    REQUIRE(tinyfiber_free(&fs) == 0);

    // Then
    CHECK(depth1 == 0);
    CHECK(depth2 == 0);
    CHECK(depth1 == depth2);
}

void job(void* param)
{
    std::atomic_int64_t* depth = (std::atomic_int64_t*)param;

    (*depth)--;

    if (*depth > 0)
    {
        // Add job
        WaitHandle wh;
        JobDeclaration jd{job, param, &wh};
        tinyfiber_add_job(jd);
        tinyfiber_await(wh);
    }
}

TEST_CASE("Example code")
{
    FiberSystem* fs;
    tinyfiber_init(&fs);
    std::atomic_int64_t depth = 3;
    job(&depth);
    tinyfiber_free(&fs);
}
} // namespace tinyfiber
