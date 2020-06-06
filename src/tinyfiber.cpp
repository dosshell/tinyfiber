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

#include "tinyringbuffer.hpp"

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

using utils::TinyRingBuffer;
using utils::TinyRingBufferStatus;

using namespace std::chrono_literals;

namespace
{
const int DEFAULT_STACKSIZE = 0;
const int MAX_NUMBER_OF_THREADS = 32;
const int NUMBER_OF_FIBERS = 1024;
const int FIBER_POOL_SIZE = 64 * 1024;
const int JOB_QUEUE_SIZE = 64 * 1024;
TinyRingBuffer<tinyfiber::JobDeclaration> g_job_queue;
TinyRingBuffer<void*> g_fiber_pool;
std::condition_variable g_no_job_cv;
std::thread g_worker_threads[MAX_NUMBER_OF_THREADS];
int g_no_of_worker_threads;
std::mutex g_no_job_mx;
std::atomic_int64_t g_active_threads;
std::atomic_bool g_should_exit;
std::atomic<void*> g_main_fiber;
void* g_init_fibers_fiber;

thread_local void* l_worker_fiber;
thread_local void* l_finished_fiber;
thread_local PSRWLOCK l_wait_handle_lock;
thread_local int thread_number = -1;
} // namespace

namespace tinyfiber
{
namespace
{
void fiber_main_loop(void*)
{
    while (true)
    {
        // allow to resume await fiber now, after we have switched from it
        if (l_wait_handle_lock != nullptr)
        {
            ReleaseSRWLockExclusive(l_wait_handle_lock);
            l_wait_handle_lock = nullptr;
        }

        JobDeclaration jb;
        if (g_job_queue.dequeue(&jb) == TinyRingBufferStatus::SUCCESS)
        {
            jb.func(jb.user_data);
            l_finished_fiber = GetCurrentFiber();

            // Take care of waiting
            if (jb.wait_handle != nullptr)
            {
                AcquireSRWLockExclusive((PSRWLOCK)&jb.wait_handle->lock);

                jb.wait_handle->counter--;

                // if we are last and someone is waiting for us, yield to it
                if (jb.wait_handle->counter == 0)
                {
                    void* fiber = jb.wait_handle->fiber;

                    // A fiber is waiting for us
                    if (fiber != nullptr)
                    {
                        jb.wait_handle->fiber = nullptr;
                        ReleaseSRWLockExclusive((PSRWLOCK)&jb.wait_handle->lock); // allow other jobs to await
                        SwitchToFiber(fiber);                                     // yield back to awaiter fiber, await will put us back at pool
                    }
                    else
                    {
                        // No one is waiting for us
                        ReleaseSRWLockExclusive((PSRWLOCK)&jb.wait_handle->lock);
                    }
                }
                else
                {
                    // We are not done yet
                    ReleaseSRWLockExclusive((PSRWLOCK)&jb.wait_handle->lock);
                }
            }
        }
        else
        {
            // There are no jobs for us, return to worker fiber
            l_finished_fiber = GetCurrentFiber();
            SwitchToFiber(l_worker_fiber); // worker fiber will put us back to pool
        }
    }
}

void worker_function()
{
    while (!g_should_exit)
    {
        void* work_fiber;
        if (g_fiber_pool.dequeue(&work_fiber) == TinyRingBufferStatus::SUCCESS)
        {
            SwitchToFiber(work_fiber);
            if (l_finished_fiber != nullptr)
            {
                g_fiber_pool.enqueue(l_finished_fiber);
                l_finished_fiber = nullptr;
            }
            else
            {
                int k = 0;
            }
        }
        // wait for new job or signal to exit (maybe put this first?)
        std::unique_lock<std::mutex> lk(g_no_job_mx);
        g_no_job_cv.wait(lk, []() { return g_should_exit || !g_job_queue.empty(); });
    }
}

} // namespace

int tinyfiber_await(WaitHandle& wait_handle)
{
    AcquireSRWLockExclusive((PSRWLOCK)&wait_handle.lock);

    // Put this to fiber queue
    if (wait_handle.counter == 0)
    {
        ReleaseSRWLockExclusive((PSRWLOCK)&wait_handle.lock);
        return 0;
    }

    wait_handle.fiber = GetCurrentFiber();
    l_wait_handle_lock = (PSRWLOCK)&wait_handle.lock;

    void* new_fiber;
    g_fiber_pool.dequeue(&new_fiber);
    SwitchToFiber(new_fiber); // if there is no job, we will resume (!?!?!?) bug here?

    // put back fiber we yield from to pool
    g_fiber_pool.enqueue(l_finished_fiber);
    l_finished_fiber = nullptr;

    return 0;
}

int tinyfiber_add_job(JobDeclaration& job)
{
    if (job.func == nullptr)
        return 0;

    if (job.wait_handle != nullptr)
        job.wait_handle->counter++;

    TinyRingBufferStatus sts = g_job_queue.enqueue(job);
    if (sts != TinyRingBufferStatus::SUCCESS)
    {
        return -1;
    }
    g_no_job_cv.notify_one();
    return 0;
}

// Must have the same WaitHandler*
int tinyfiber_add_jobs(JobDeclaration jobs[], int64_t elements)
{
    if (jobs[0].wait_handle != nullptr)
        jobs[0].wait_handle->counter += elements;
    TinyRingBufferStatus sts = g_job_queue.enqueue(jobs, elements);
    if (sts != TinyRingBufferStatus::SUCCESS)
    {
        return -1;
    }
    g_no_job_cv.notify_all();
    return 0;
}

void start_workers(void*)
{
    // First worker will start with main fiber
    g_worker_threads[0] = std::thread([] {
        l_worker_fiber = ConvertThreadToFiber(nullptr);
        SwitchToFiber(g_main_fiber);
        if (l_finished_fiber != nullptr)
            g_fiber_pool.enqueue(l_finished_fiber);
        l_finished_fiber = nullptr;
        worker_function();
        ConvertFiberToThread();
    });

    // Other workers will start with worker_function
    for (int i = 1; i < g_no_of_worker_threads; ++i)
    {
        g_worker_threads[i] = std::thread([] {
            l_worker_fiber = ConvertThreadToFiber(nullptr);
            worker_function();
            ConvertFiberToThread();
        });
    }

    // Wait for worker threads to exit
    for (int i = 0; i < g_no_of_worker_threads; ++i)
    {
        g_worker_threads[i].join();
    }
    SwitchToFiber(g_main_fiber);
}

int tinyfiber_init(int max_threads)
{
    // Init pools etc.
    g_job_queue.init(JOB_QUEUE_SIZE);
    g_fiber_pool.init(FIBER_POOL_SIZE);

    // -1 since main thread counts
    g_no_of_worker_threads = std::thread::hardware_concurrency();
    g_no_of_worker_threads = std::min(g_no_of_worker_threads, MAX_NUMBER_OF_THREADS);

    if (max_threads != ALL_CORES)
        g_no_of_worker_threads = std::min(g_no_of_worker_threads, max_threads);

    void** allocated_fibers = nullptr;
    if (g_fiber_pool.allocate(NUMBER_OF_FIBERS, &allocated_fibers) != TinyRingBufferStatus::SUCCESS)
        return -1;

    for (int i = 0; i < NUMBER_OF_FIBERS; ++i)
    {
        void* fiber = CreateFiber(DEFAULT_STACKSIZE, fiber_main_loop, nullptr);
        if (fiber == nullptr)
        {
            // DWORD err = GetLastError();
            // todo(markusl): free
            return -1;
        }
        allocated_fibers[i] = fiber;
    }

    // Switch away from main thread and start worker system
    g_main_fiber = ConvertThreadToFiber(nullptr);
    g_init_fibers_fiber = CreateFiber(DEFAULT_STACKSIZE, start_workers, nullptr);
    SwitchToFiber(g_init_fibers_fiber); // Lose main thread
    // Worker thread will execute from here now
    return 0;
}

// Must be called from main fiber (eg, from no job)
int tinyfiber_free()
{
    g_should_exit = true;
    g_no_job_cv.notify_all();

    SwitchToFiber(l_worker_fiber);

    ConvertFiberToThread();

    // Delete fibers
    DeleteFiber(g_init_fibers_fiber);
    void** deallocate_fibers = g_fiber_pool.data();
    for (int i = 0; i < NUMBER_OF_FIBERS; ++i)
        DeleteFiber(deallocate_fibers[i]);
    g_fiber_pool.free();
    g_job_queue.free();

    return 0;
}

} // namespace tinyfiber
