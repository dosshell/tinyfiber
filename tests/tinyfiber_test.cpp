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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.hpp"

#include <tinyqueue.hpp>
#include <thread>
#include <cstdint>
#include <chrono>

using utils::TinyQueue;

namespace
{
// ticktock in microseconds
int64_t ticktock()
{
    static auto s_lastTimeStamp = std::chrono::high_resolution_clock::now();
    auto t2 = std::chrono::high_resolution_clock::now();
    int64_t elapsed = int64_t(std::chrono::duration_cast<std::chrono::microseconds>(t2 - s_lastTimeStamp).count());
    s_lastTimeStamp = t2;
    return elapsed;
}

} // namespace

namespace tinyfiber
{
const int NO_OF_CHILD_JOBS = 100;

struct ChildJobArgs
{
    int n;
    int* result;
};

void child_job(void* param)
{
    ChildJobArgs args = *(ChildJobArgs*)param;
    int n = args.n;
    args.result[n] = (n + 1) * (n - 1) * (n + 2) * (n - 2);
}

void root_job(void* param)
{
    if (param == nullptr)
        return;

    WaitHandle h;
    JobDeclaration child_jobs[NO_OF_CHILD_JOBS];
    ChildJobArgs child_job_args[NO_OF_CHILD_JOBS];
    int result[NO_OF_CHILD_JOBS];

    for (int i = 0; i < NO_OF_CHILD_JOBS; ++i)
    {
        child_job_args[i] = {i, result};
        child_jobs[i] = {child_job, &child_job_args[i], &h};
    }
    tinyfiber_add_jobs(child_jobs, NO_OF_CHILD_JOBS);
    tinyfiber_await(h);

    int sum = 0;
    for (int i = 0; i < NO_OF_CHILD_JOBS; ++i)
    {
        sum += result[i];
    }
    *reinterpret_cast<int*>(param) = sum;
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
    // Given
    int mt_sum = 0;
    // Do job in single threaded environment
    ChildJobArgs args[NO_OF_CHILD_JOBS];
    int st_result[NO_OF_CHILD_JOBS];
    for (int i = 0; i < NO_OF_CHILD_JOBS; ++i)
    {
        args[i] = {i, st_result};
        child_job(&args[i]);
    }
    int st_sum = 0;
    for (int i = 0; i < NO_OF_CHILD_JOBS; ++i)
        st_sum += st_result[i];

    // When
    int sts = tinyfiber_run(root_job, &mt_sum);

    // Then
    CHECK(sts == 0);
    CHECK(st_sum == mt_sum);
    CHECK(st_sum > 0); // Test overflow
}

TEST_CASE("tinyfiber reentrancy")
{
    // Given
    int result1 = 0;
    int result2 = 0;
    tinyfiber_run(root_job, &result1);

    // When
    int sts = tinyfiber_run(root_job, &result2);

    // Then
    CHECK(sts == 0);
    CHECK(result1 == result2);
}

struct BiggerDataStruct
{
    int64_t a;
    int64_t b;
    double c;
};

static const int INTEGER_BUFFER_SIZE = 64 * 1024;
static const int BIGGER_DATA_BUFFER_SIZE = 64 * 1024;
static const int BUFFER_SIZE_SMALL = 64 * 1024;

TEST_CASE("tinyqueue performance")
{
    {
        TinyQueue<int> q_med;
        CHECK_EQ(q_med.init(INTEGER_BUFFER_SIZE), 0);

        ticktock();
        for (int round = 0; round < 100; ++round)
        {
            for (int i = 0; i < 16000; ++i)
            {
                q_med.enqueue(i);
            }
            int d;
            for (int i = 0; i < 16000; ++i)
            {
                q_med.dequeue(&d);
            }
        }

        uint64_t time = ticktock();
        std::cout << "Integer Data: " << std::endl;
        std::cout << "Time: " << time << std::endl;
        std::cout << "Ops/s: " << 3200000.0 / time * 1000000.0 << std::endl;
        std::cout << std::endl;
    }

    {
        TinyQueue<BiggerDataStruct> q_big;
        CHECK_EQ(q_big.init(BIGGER_DATA_BUFFER_SIZE), 0);

        ticktock();
        for (int round = 0; round < 100; ++round)
        {
            for (int i = 0; i < 16000; ++i)
            {
                BiggerDataStruct b = {1, 2, 0.5};
                q_big.enqueue(b);
            }
            BiggerDataStruct d;
            for (int i = 0; i < 16000; ++i)
            {
                q_big.dequeue(&d);
            }
        }

        int64_t time = ticktock();
        std::cout << "Bigger Data: " << std::endl;
        std::cout << "Time: " << time << std::endl;
        std::cout << "Ops/s: " << 3200000.0 / time * 1000000.0 << std::endl;
        std::cout << std::endl;
    }

    TinyQueue<int> q;

    CHECK_EQ(q.init(BUFFER_SIZE_SMALL), 0);

    for (int round = 0; round < 100; ++round)
    {
        ticktock();
        std::atomic_int starvations = 0;
        std::atomic_int overflows = 0;
        std::atomic_int stalls = 0;
        std::atomic_int sum = 0;

        std::thread consumers[3];
        for (int t = 0; t < 3; ++t)
        {
            consumers[t] = std::thread([&] {
                for (int i = 1; i <= 20000; ++i)
                {
                    int d;
                    while (q.dequeue(&d) != 0)
                    {
                        starvations++;
                    }
                    sum += d;
                }
            });
        }

        std::thread feeders1[3];
        for (int t = 0; t < 3; ++t)
        {
            feeders1[t] = std::thread([&] {
                for (int i = 1; i <= 10000; ++i)
                {
                    while (q.enqueue(i) != 0)
                    {
                        overflows++;
                    }
                }
            });
        }
        for (int t = 0; t < 3; ++t)
            feeders1[t].join();

        std::thread feeders2[3];
        for (int t = 0; t < 3; ++t)
        {
            feeders2[t] = std::thread([&] {
                for (int i = 1; i <= 10000; ++i)
                {
                    while (q.enqueue(i) != 0)
                    {
                        overflows++;
                    }
                }
            });
        }
        for (int t = 0; t < 3; ++t)
            feeders2[t].join();

        for (int t = 0; t < 3; ++t)
            consumers[t].join();

        int64_t time = ticktock();
        CHECK_EQ(sum, 300030000);
    }
}
} // namespace tinyfiber
