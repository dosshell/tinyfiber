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

#include <tinyringbuffer.hpp>

#include "doctest.hpp"

#include <thread>
#include <cstdint>
#include <chrono>
#include <iostream>
#include <atomic>

using utils::TinyRingBuffer;
using utils::TinyRingBufferStatus;
using namespace std;

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

struct BiggerDataStruct
{
    int64_t a;
    int64_t b;
    double c;
};

static const int INTEGER_BUFFER_SIZE = 64 * 1024;
static const int BIGGER_DATA_BUFFER_SIZE = 64 * 1024;
static const int BUFFER_SIZE_SMALL = 64 * 1024;

} // namespace

TEST_CASE("Simple Init")
{
    // Given
    size_t length = 1024;

    // When
    TinyRingBuffer<void*> rb(length);

    // Then
    CHECK(rb.length() >= length);
    CHECK(rb.empty());
}

TEST_CASE("Dequeue Empty")
{
    // Given
    size_t length = 1;
    TinyRingBuffer<int> rb(length);
    
    // When
    auto sts = rb.dequeue(nullptr);
    
    // Then
    CHECK(sts == TinyRingBufferStatus::BUFFER_EMPTY);
}

TEST_CASE("Single Enqueue and Dequeue")
{
    // Given
    TinyRingBuffer<int> rb(1024);
    int a = 235;
    int b = 0;
    
    // When
    rb.enqueue(a);
    TinyRingBufferStatus sts = rb.dequeue(&b);

    // Then
    CHECK(sts == TinyRingBufferStatus::SUCCESS);
    CHECK(rb.empty());
    CHECK(rb.count() == 0);
    CHECK(rb.length() >= 1024);
    CHECK(a == b);
}

TEST_CASE("Enqueue Full")
{
    // Given
    TinyRingBuffer<int> rb(2);
    rb.enqueue(1);
    rb.enqueue(2);
    
    // When
    TinyRingBufferStatus sts = rb.enqueue(3);

    // Then
    CHECK(sts == TinyRingBufferStatus::BUFFER_FULL);
}

TEST_CASE("Double Enqueue and Dequeue")
{
    // Given
    TinyRingBuffer<int> rb(1);
    int a;
    int b;
    
    // When
    rb.enqueue(1);
    TinyRingBufferStatus sts1 = rb.dequeue(&a);
    rb.enqueue(2);
    TinyRingBufferStatus sts2 = rb.dequeue(&b);

    // Then
    CHECK(sts1 == TinyRingBufferStatus::SUCCESS);
    CHECK(sts2 == TinyRingBufferStatus::SUCCESS);

    CHECK(rb.empty());
    CHECK(rb.count() == 0);
    CHECK(rb.length() >= 1);
    CHECK(a == 1);
    CHECK(b == 2);
}

TEST_CASE("Move Constructor")
{
    // Given
    TinyRingBuffer<int> rb1{1};
    rb1.enqueue(1);
    
    // When
    TinyRingBuffer<int> rb2{std::move(rb1)};
    
    // Then
    CHECK(rb1.empty());
    CHECK(rb2.count() == 1);
    int a;
    rb2.dequeue(&a);
    CHECK(a == 1);
}

TEST_CASE("Move Assignment")
{
    // Given
    TinyRingBuffer<int> rb1{1};
    TinyRingBuffer<int> rb2;
    rb1.enqueue(1);
    
    // When
    rb2 = std::move(rb1);
    
    // Then
    CHECK(rb1.empty());
    CHECK(rb2.count() == 1);
    int a;
    rb2.dequeue(&a);
    CHECK(a == 1);
}

#ifdef PERFTEST
TEST_CASE("tinyringbuffer performance")
{
    {
        TinyRingBuffer<int> q_med;
        CHECK(q_med.init(INTEGER_BUFFER_SIZE) == TinyRingBufferStatus::SUCCESS);

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
        TinyRingBuffer<BiggerDataStruct> q_big;
        CHECK(q_big.init(BIGGER_DATA_BUFFER_SIZE) == TinyRingBufferStatus::SUCCESS);

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

    TinyRingBuffer<int> q;

    CHECK(q.init(BUFFER_SIZE_SMALL) == TinyRingBufferStatus::SUCCESS);

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
                    while (q.dequeue(&d) != TinyRingBufferStatus::SUCCESS)
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
                    while (q.enqueue(i) != TinyRingBufferStatus::SUCCESS)
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
                    while (q.enqueue(i) != TinyRingBufferStatus::SUCCESS)
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
#endif
