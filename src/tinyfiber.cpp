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

#include "tinyfiber.h"

#include "tinyqueue.hpp"

#include <thread>
#include <vector>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdint.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>
#include <synchapi.h>

using utils::TinyQueue;

namespace
{
const int DEFAULT_STACKSIZE = 0;
const int MAX_NUMBER_OF_THREADS = 32;
const int NUMBER_OF_FIBERS = 1024;
const int FIBER_POOL_SIZE = 64 * 1024;
const int JOB_QUEUE_SIZE = 64 * 1024;
TinyQueue<tinyfiber::JobDeclaration> g_job_queue;
TinyQueue<void*> g_fiber_pool;
std::condition_variable g_no_job_cv;
std::thread g_worker_threads[MAX_NUMBER_OF_THREADS];
int g_number_of_threads;
std::mutex g_no_job_mx;
std::atomic_int64_t g_active_threads;
std::atomic_bool g_should_exit;

thread_local void* l_primary_fiber;
thread_local void* l_put_me_back;
thread_local tinyfiber::WaitHandle* l_wait_handle;
} // namespace

namespace tinyfiber
{
int tinyfiber_add_job(JobDeclaration& job)
{
    if (job.func == nullptr)
        return 0;

    if (job.wait_handle != nullptr)
        job.wait_handle->counter++;

    int sts = g_job_queue.enqueue(job);
    if (sts == 0)
        g_no_job_cv.notify_one();
    return sts;
}

// Must have the same WaitHandler*
int tinyfiber_add_jobs(JobDeclaration jobs[], int64_t elements)
{
    if (jobs[0].wait_handle != nullptr)
        jobs[0].wait_handle->counter += elements;
    int sts = g_job_queue.enqueue(jobs, elements);
    if (sts == 0)
        g_no_job_cv.notify_one();
    return sts;
}

int tinyfiber_await(WaitHandle& wait_handle)
{
    AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&wait_handle.lock));
    // Put this to fiber queue
    if (wait_handle.counter == 0)
    {
        ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&wait_handle.lock));
        return 0;
    }

    wait_handle.fiber = GetCurrentFiber();
    l_wait_handle = &wait_handle;

    void* new_fiber;
    g_fiber_pool.dequeue(&new_fiber);
    SwitchToFiber(new_fiber);

    g_fiber_pool.enqueue(l_put_me_back);
    l_put_me_back = nullptr;

    return 0;
}

static void fiber_main_loop(void*)
{
    while (true)
    {
        if (l_wait_handle != nullptr)
        {
            ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&l_wait_handle->lock));
            l_wait_handle = nullptr;
        }

        JobDeclaration jb;
        if (g_job_queue.dequeue(&jb) == 0)
        {
            jb.func(jb.user_data);
            if (jb.wait_handle != nullptr)
            {
                if (--(jb.wait_handle->counter) == 0)
                {
                    AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&jb.wait_handle->lock));
                    void* fiber = jb.wait_handle->fiber;
                    if (fiber != nullptr)
                    {
                        ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&jb.wait_handle->lock));
                        SwitchToFiber(fiber);
                    }
                    else
                    {
                        ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&jb.wait_handle->lock));
                    }
                }
            }
        }
        else
        {
            l_put_me_back = GetCurrentFiber();
            SwitchToFiber(l_primary_fiber);
        }
    }
}

int tinyfiber_run(void (*entrypoint)(void*), void* user_param, int max_threads)
{
    g_job_queue.init(JOB_QUEUE_SIZE);
    g_fiber_pool.init(FIBER_POOL_SIZE);

    g_number_of_threads = std::thread::hardware_concurrency();
    g_number_of_threads = std::min(g_number_of_threads, MAX_NUMBER_OF_THREADS);

    if (max_threads != ALL_CORES)
        g_number_of_threads = std::min(g_number_of_threads, max_threads);

    for (int i = 0; i < NUMBER_OF_FIBERS; ++i)
    {
        void* fiber = CreateFiber(DEFAULT_STACKSIZE, fiber_main_loop, nullptr);
        int sts = g_fiber_pool.enqueue(fiber);
        if (sts != 0)
            return sts;
    }

    JobDeclaration jb = {entrypoint, user_param, nullptr};
    tinyfiber_add_job(jb);

    g_active_threads = g_number_of_threads;
    g_should_exit = false;

    for (int i = 0; i < g_number_of_threads; ++i)
    {
        g_worker_threads[i] = std::thread([] {
            l_primary_fiber = ConvertThreadToFiber(nullptr);
            l_wait_handle = nullptr;

            while (!g_should_exit)
            {
                // do work
                void* workFiber;
                if (g_fiber_pool.dequeue(&workFiber) == 0)
                    SwitchToFiber(workFiber);
                g_fiber_pool.enqueue(l_put_me_back);

                // wait for new job or signal exit if last one alive
                std::unique_lock<std::mutex> lk(g_no_job_mx);
                int64_t threadsLeft = --g_active_threads;
                if (threadsLeft == 0)
                {
                    g_should_exit = true;
                    g_no_job_cv.notify_all();
                }
                else
                {
                    g_no_job_cv.wait(lk, []() { return g_should_exit || !g_job_queue.empty(); });
                }
            }
        });
    }

    // Free system
    for (int i = 0; i < g_number_of_threads; ++i)
    {
        g_worker_threads[i].join();
    }

    g_job_queue.free();
    g_fiber_pool.free();
    return 0;
}

} // namespace tinyfiber
