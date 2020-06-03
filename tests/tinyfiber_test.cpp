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
void job(void* param)
{
    if (param == nullptr)
        return;

    std::atomic_int64_t* depth = (std::atomic_int64_t*)param;

    if (*depth == 0)
    {
        return;
    }

    (*depth)--;

    WaitHandle h;
    JobDeclaration jd;
    jd.func = job;
    jd.user_data = param;
    jd.wait_handle = &h;

    tinyfiber_add_job(jd);
    tinyfiber_await(h);
}

TEST_CASE("tinyfiber null")
{
    // Given
    // When
    int sts = tinyfiber_run(nullptr, nullptr, 0);

    // Then
    CHECK(sts == 0);
}

TEST_CASE("tinyfiber run")
{
    //Given
    std::atomic_int64_t depth = 512;

    //When
    int sts = tinyfiber_run(job, &depth);

    //Then
    CHECK(sts == 0);
    CHECK(depth == 0);
}

TEST_CASE("tinyfiber mt consistency")
{
    // Given
    std::atomic_int64_t depth1 = 313;
    std::atomic_int64_t depth2 = 313;
    int result1 = 0;

    // When
    int sts1 = tinyfiber_run(job, &depth1, 1);
    int sts2 = tinyfiber_run(job, &depth2, 2);

    // Then
    CHECK(sts1 == 0);
    CHECK(sts2 == 0);
    CHECK(depth1 == 0);
    CHECK(depth2 == 0);
    CHECK(depth1 == depth2);
}
} // namespace tinyfiber
