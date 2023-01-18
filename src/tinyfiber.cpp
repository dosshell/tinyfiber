/*
MIT Licenserelease of the first text-to-speech (TTS) modelrelease of the first text-to-speech (TTS) model

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
#include <cstdint>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>
#include <synchapi.h>
#else
#include <ucontext.h>
#endif

using utils::TinyRingBuffer;
using utils::TinyRingBufferStatus;

#ifdef _WIN32
const int TFB_DEFAULT_STACK_SIZE = 0;
#define FIBER_TYPE void*
#define LOCK_TYPE SRWLOCK
#define FIBER_FUNCTION(X) void __stdcall X(void*)
typedef void (*__stdcall FIBER_FUNC_TYPE)(void*);
#else
const int TFB_DEFAULT_STACK_SIZE = 8192; // * 1024
#define FIBER_TYPE ucontext_t*
#define LOCK_TYPE std::mutex
#define FIBER_FUNCTION(X) void X(void)
typedef void (*FIBER_FUNC_TYPE)(void);
#endif
const int TFB_MAX_NUMBER_OF_THREADS = 256;
const int TFB_NUMBER_OF_FIBERS = 1024;
const int TFB_JOB_QUEUE_SIZE = 1024;

#ifdef NDEBUG
#define LOG(X)
#else
#define LOG(X) std::cout << std::this_thread::get_id() << "; " << g_fs.l_thread_number << "; " << __FUNCTION__ << "; " << X << std::endl;
#endif

struct TfbContext
{
    TinyRingBuffer<TfbJobDeclaration> job_queue;
    TinyRingBuffer<FIBER_TYPE> fiber_pool;
    std::condition_variable no_job_cv;
    std::thread worker_threads[TFB_MAX_NUMBER_OF_THREADS];
    int no_of_worker_threads;
    std::atomic_bool should_exit;
    std::mutex pending_jobs_mx;
    std::atomic_int64_t no_of_pending_jobs;
    std::atomic<FIBER_TYPE> main_fiber;       // Atomic?
    FIBER_TYPE start_workers_fiber = nullptr; // Main thread will run this fiber and wait for worker threads
    static std::atomic_int64_t thread_number_counter;
    static thread_local FIBER_TYPE l_worker_fiber;
    static thread_local FIBER_TYPE l_finished_fiber;
    static thread_local LOCK_TYPE* l_wait_handle_lock;
    static thread_local std::int64_t l_thread_number;
#ifndef _WIN32
    static thread_local FIBER_TYPE l_current_fiber;
    std::atomic_int64_t fibers_used = 0;
    ucontext_t fibers[TFB_NUMBER_OF_FIBERS + TFB_MAX_NUMBER_OF_THREADS + 1];
    char stacks[TFB_DEFAULT_STACK_SIZE * (TFB_NUMBER_OF_FIBERS + TFB_MAX_NUMBER_OF_THREADS + 1)];
#endif
};

thread_local FIBER_TYPE TfbContext::l_worker_fiber;
thread_local FIBER_TYPE TfbContext::l_finished_fiber;
thread_local LOCK_TYPE* TfbContext::l_wait_handle_lock;
std::atomic_int64_t TfbContext::thread_number_counter;
thread_local std::int64_t TfbContext::l_thread_number;

#ifndef _WIN32
thread_local FIBER_TYPE TfbContext::l_current_fiber;
#endif

namespace
{
TfbContext g_fs;

FIBER_TYPE create_fiber(FIBER_FUNC_TYPE func)
{
#ifdef _WIN32
    return CreateFiber(TFB_DEFAULT_STACK_SIZE, func, nullptr);
#else
    int64_t n = g_fs.fibers_used++;
    getcontext(&g_fs.fibers[n]);
    g_fs.fibers[n].uc_stack.ss_sp = reinterpret_cast<void*>(&g_fs.stacks[n * TFB_DEFAULT_STACK_SIZE]);
    g_fs.fibers[n].uc_stack.ss_size = TFB_DEFAULT_STACK_SIZE;
    makecontext(&g_fs.fibers[n], func, 0);
    return &g_fs.fibers[n];
#endif
}

void switch_to_fiber(FIBER_TYPE fiber)
{
#ifdef _WIN32
    SwitchToFiber(fiber);
#else
    FIBER_TYPE previous_fiber = g_fs.l_current_fiber;
    g_fs.l_current_fiber = fiber;
    swapcontext(previous_fiber, fiber);
#endif
}

FIBER_TYPE get_current_fiber()
{
#ifdef _WIN32
    return GetCurrentFiber();
#else
    return g_fs.l_current_fiber;
#endif
}

FIBER_TYPE convert_thread_to_fiber()
{
#ifdef _WIN32
    return ConvertThreadToFiber(nullptr);
#else
    int64_t n = g_fs.fibers_used++;
    getcontext(&g_fs.fibers[n]);
    g_fs.l_current_fiber = &g_fs.fibers[n];
    return &g_fs.fibers[n];
#endif
}

void convert_fiber_to_thread()
{
#ifdef _WIN32
    ConvertFiberToThread();
#else
#endif
}

void delete_fiber(FIBER_TYPE fiber)
{
#ifdef _WIN32
    DeleteFiber(fiber);
#else
#endif
}
void lock(LOCK_TYPE& lock)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&lock);
#else
    lock.lock();
#endif
}

void unlock(LOCK_TYPE& lock)
{
#ifdef _WIN32
    ReleaseSRWLockExclusive(&lock);
#else
    lock.unlock();
#endif
}

void init_lock(LOCK_TYPE& lock)
{
#ifdef _WIN32
    lock = SRWLOCK_INIT;
#else
#endif
}

static FIBER_FUNCTION(fiber_main_loop)
{
    LOG("Enter");
    while (true)
    {
        // allow to resume await fiber now, after we have switched from it
        if (g_fs.l_wait_handle_lock != nullptr)
        {
            unlock(*g_fs.l_wait_handle_lock);
            g_fs.l_wait_handle_lock = nullptr;
        }

        TfbJobDeclaration jb;
        if (!g_fs.should_exit && g_fs.job_queue.dequeue(&jb) == TinyRingBufferStatus::SUCCESS)
        {
            {
                std::scoped_lock<std::mutex> lk(g_fs.pending_jobs_mx);
                g_fs.no_of_pending_jobs--;
            }

            jb.func(jb.user_data);
            g_fs.l_finished_fiber = get_current_fiber();

            // Take care of waiting
            if (jb.wait_handle != nullptr)
            {
                lock(jb.wait_handle->_lock);

                int64_t n = --(jb.wait_handle->_counter);

                // if we are last and someone is waiting for us, yield to it
                if (n == 0)
                {
                    FIBER_TYPE fiber = jb.wait_handle->_fiber;

                    // A fiber is waiting for us
                    if (fiber != nullptr)
                    {
                        jb.wait_handle->_fiber = nullptr;
                        unlock(jb.wait_handle->_lock);
                        switch_to_fiber(fiber); // yield back to awaiter fiber, await will put us back at pool
                    }
                    else
                    {
                        // No one is awaiting for us
                        unlock(jb.wait_handle->_lock);
                    }
                }
                else
                {
                    // There may be more jobs for us
                    unlock(jb.wait_handle->_lock);
                }
            }
        }
        else
        {
            // There are no jobs for us or exit is requested, return to worker fiber whom can block us
            g_fs.l_finished_fiber = get_current_fiber();
            switch_to_fiber(g_fs.l_worker_fiber);
        }
    }
}

static int worker_function()
{
    while (!g_fs.should_exit)
    {
        if (g_fs.no_of_pending_jobs > 0)
        {
            FIBER_TYPE work_fiber;
            TinyRingBufferStatus sts = g_fs.fiber_pool.dequeue(&work_fiber);

            if (sts == TinyRingBufferStatus::SUCCESS)
            {
                switch_to_fiber(work_fiber);
                if (g_fs.l_finished_fiber != nullptr)
                {
                    g_fs.fiber_pool.enqueue(g_fs.l_finished_fiber);
                    g_fs.l_finished_fiber = nullptr;
                }
            }
            else
            {
                LOG("No more fibers in the pool");
                return -1;
            }
        }
        else
        {
            std::unique_lock<std::mutex> lk(g_fs.pending_jobs_mx);
            g_fs.no_job_cv.wait(lk, [&] { return g_fs.no_of_pending_jobs > 0 || g_fs.should_exit; });
        }
    }

    return 0;
}

static FIBER_FUNCTION(start_workers)
{
    LOG("called.");
    // First worker will start at main fiber
    g_fs.worker_threads[0] = std::thread(
        [&]
        {
            g_fs.l_thread_number = g_fs.thread_number_counter++;
            LOG("Started");
            g_fs.l_worker_fiber = nullptr;
            g_fs.l_finished_fiber = nullptr;
            g_fs.l_wait_handle_lock = nullptr;
#ifndef _WIN32
            g_fs.l_current_fiber = nullptr;
#endif

            g_fs.l_worker_fiber = convert_thread_to_fiber();
            LOG("Switch: Main-worker-thread to main fiber");
            switch_to_fiber(g_fs.main_fiber);

            if (g_fs.l_finished_fiber != nullptr)
            {
                g_fs.fiber_pool.enqueue(g_fs.l_finished_fiber);
                g_fs.l_finished_fiber = nullptr;
            }
            convert_fiber_to_thread();
            delete_fiber(g_fs.l_worker_fiber);
        });

    // Other workers will start with worker_function
    for (int i = 1; i < g_fs.no_of_worker_threads; ++i)
    {
        g_fs.worker_threads[i] = std::thread(
            [&]
            {
                g_fs.l_thread_number = g_fs.thread_number_counter++;
                LOG("Started");
                g_fs.l_worker_fiber = nullptr;
                g_fs.l_finished_fiber = nullptr;
                g_fs.l_wait_handle_lock = nullptr;
#ifndef _WIN32
                g_fs.l_current_fiber = nullptr;
#endif
                g_fs.l_worker_fiber = convert_thread_to_fiber();
                worker_function(); // todo(markusl): handle return error code
                convert_fiber_to_thread();
                delete_fiber(g_fs.l_worker_fiber);
            });
    }

    LOG("Wait for join");
    // Wait for worker threads to exit
    for (int i = 0; i < g_fs.no_of_worker_threads; ++i)
    {
        g_fs.worker_threads[i].join();
    }
    LOG("Joined. Switching back main-fiber");
    switch_to_fiber(g_fs.main_fiber);
}

} // namespace

int tfb_init_ext(int max_threads)
{
    g_fs.l_thread_number = g_fs.thread_number_counter++;
    LOG("called.");

    g_fs.job_queue = TinyRingBuffer<TfbJobDeclaration>(TFB_JOB_QUEUE_SIZE);
    g_fs.fiber_pool = TinyRingBuffer<FIBER_TYPE>(TFB_NUMBER_OF_FIBERS);
    g_fs.main_fiber = nullptr;
    g_fs.should_exit = false;
    g_fs.no_of_pending_jobs = 0;
    g_fs.start_workers_fiber = nullptr;
#ifndef _WIN32
    g_fs.fibers_used = 0;
#endif

    g_fs.no_of_worker_threads = std::thread::hardware_concurrency();
    g_fs.no_of_worker_threads = std::min(g_fs.no_of_worker_threads, TFB_MAX_NUMBER_OF_THREADS);
    // DEBUG
    g_fs.no_of_worker_threads = 1;

    if (max_threads != TFB_ALL_CORES)
        g_fs.no_of_worker_threads = std::min(g_fs.no_of_worker_threads, max_threads);

    for (int i = 0; i < TFB_NUMBER_OF_FIBERS; ++i)
    {
        FIBER_TYPE fiber = create_fiber(fiber_main_loop);
        g_fs.fiber_pool.enqueue(fiber);
    }

    // Switch away from main thread and start worker system
    g_fs.main_fiber = convert_thread_to_fiber();
    g_fs.start_workers_fiber = create_fiber(start_workers);
    switch_to_fiber(g_fs.start_workers_fiber); // Lose main fiber from main thread
    // Worker thread will execute from here now
    LOG("Now as a main fiber in the pool.");
    return 0;
}

// Must be called from main fiber (eg, from no job)
int tfb_free()
{
    LOG("called.");

    {
        std::scoped_lock<std::mutex> lk(g_fs.pending_jobs_mx);
        g_fs.should_exit = true;
    }

    g_fs.no_job_cv.notify_all();
    LOG("Switching to worker fiber");
    switch_to_fiber(g_fs.l_worker_fiber);
    LOG("The main thread is back on main with no worker threads!");
    convert_fiber_to_thread();

    // Delete fibers

    delete_fiber(g_fs.start_workers_fiber);
    FIBER_TYPE deallocate_fibers[TFB_NUMBER_OF_FIBERS];

    std::int64_t dequed;
    LOG("before");
    g_fs.fiber_pool.dequeue(deallocate_fibers, TFB_NUMBER_OF_FIBERS, &dequed);
    LOG("after");

    for (int i = 0; i < dequed; ++i)
        delete_fiber(deallocate_fibers[i]);
    g_fs.job_queue = TinyRingBuffer<TfbJobDeclaration>();
    g_fs.fiber_pool = TinyRingBuffer<FIBER_TYPE>();
    return 0;
}

int tfb_add_jobdecl(TfbJobDeclaration* job)
{
    if (job == nullptr)
        return -1;

    if (job->func == nullptr)
        return 0;

    if (job->wait_handle != nullptr)
    {
        job->wait_handle->_counter++;
    }

    TinyRingBufferStatus sts = g_fs.job_queue.enqueue(*job);
    if (sts != TinyRingBufferStatus::SUCCESS)
        return -1;

    {
        std::scoped_lock<std::mutex> lk(g_fs.pending_jobs_mx);
        ++g_fs.no_of_pending_jobs;
    }
    g_fs.no_job_cv.notify_one();

    return 0;
}

// Must have the same WaitHandler*
int tfb_add_jobdecls(TfbJobDeclaration jobs[], int64_t elements)
{
    if (jobs[0].wait_handle != nullptr)
        jobs[0].wait_handle->_counter += elements;

    if (g_fs.job_queue.enqueue(jobs, elements) != TinyRingBufferStatus::SUCCESS)
        return -1;

    {
        std::scoped_lock<std::mutex> lk(g_fs.pending_jobs_mx);
        g_fs.no_of_pending_jobs += elements;
    }
    g_fs.no_job_cv.notify_all();
    return 0;
}

int tfb_await(TfbWaitHandle* wait_handle)
{
    if (wait_handle == nullptr)
        return -1;

    lock(wait_handle->_lock);

    // Put this to fiber queue
    if (wait_handle->_counter == 0)
    {
        unlock(wait_handle->_lock);
        return 0;
    }

    wait_handle->_fiber = get_current_fiber();
    g_fs.l_wait_handle_lock = &wait_handle->_lock;

    FIBER_TYPE new_fiber;
    if (g_fs.fiber_pool.dequeue(&new_fiber) == TinyRingBufferStatus::SUCCESS)
    {
        switch_to_fiber(new_fiber);
        // put back fiber we yield from to pool
        g_fs.fiber_pool.enqueue(g_fs.l_finished_fiber);
        g_fs.l_finished_fiber = nullptr;
    }
    else
    {
        LOG("Failed to dequeue fiber");
        return -1;
    }
    return 0;
}

int tfb_thread_id()
{
    return g_fs.l_thread_number;
}