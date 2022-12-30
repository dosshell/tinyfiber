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

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>
#include <synchapi.h>
#else
#define __stdcall
#endif

using utils::TinyRingBuffer;
using utils::TinyRingBufferStatus;

const int TFB_DEFAULT_STACKSIZE = 0;
const int TFB_MAX_NUMBER_OF_THREADS = 256;
const int TFB_NUMBER_OF_FIBERS = 1024;
const int TFB_FIBER_POOL_SIZE = 1024;
const int TFB_JOB_QUEUE_SIZE = 1024;

struct TfbContext
{
    TinyRingBuffer<TfbJobDeclaration> job_queue;
    TinyRingBuffer<void*> fiber_pool;
    std::condition_variable no_job_cv;
    std::thread worker_threads[TFB_MAX_NUMBER_OF_THREADS];
    int no_of_worker_threads = 0;
    std::atomic_bool should_exit;
    std::mutex pending_jobs_mx;
    std::atomic_int64_t no_of_pending_jobs;
    std::atomic<void*> main_fiber;
    void* init_fibers_fiber = nullptr;
#ifdef _WIN32
    static thread_local void* l_worker_fiber;
    static thread_local void* l_finished_fiber;
    static thread_local PSRWLOCK l_wait_handle_lock;
#endif
};

#ifdef _WIN32
thread_local void* TfbContext::l_worker_fiber;
thread_local void* TfbContext::l_finished_fiber;
thread_local PSRWLOCK TfbContext::l_wait_handle_lock;
#endif

namespace
{
thread_local TfbContext* l_my_fiber_system;

static void __stdcall fiber_main_loop(void* fiber_system)
{
#ifdef _WIN32
    if (fiber_system == nullptr)
        return;

    TfbContext& fs = *(TfbContext*)fiber_system;
    while (true)
    {
        // allow to resume await fiber now, after we have switched from it
        if (fs.l_wait_handle_lock != nullptr)
        {
            ReleaseSRWLockExclusive(fs.l_wait_handle_lock);
            fs.l_wait_handle_lock = nullptr;
        }

        TfbJobDeclaration jb;
        if (!fs.should_exit && fs.job_queue.dequeue(&jb) == TinyRingBufferStatus::SUCCESS)
        {
            {
                std::lock_guard<std::mutex> lk(fs.pending_jobs_mx);
                --fs.no_of_pending_jobs;
            }

            jb.func(jb.user_data);
            fs.l_finished_fiber = GetCurrentFiber();

            // Take care of waiting
            if (jb.wait_handle != nullptr)
            {
                AcquireSRWLockExclusive((PSRWLOCK)&jb.wait_handle->_lock);

                reinterpret_cast<std::atomic_int64_t&>(jb.wait_handle->_counter)--;

                // if we are last and someone is waiting for us, yield to it
                if (reinterpret_cast<std::atomic_int64_t&>(jb.wait_handle->_counter).load() == 0)
                {
                    void* fiber = jb.wait_handle->_fiber;

                    // A fiber is waiting for us
                    if (fiber != nullptr)
                    {
                        jb.wait_handle->_fiber = nullptr;
                        ReleaseSRWLockExclusive((PSRWLOCK)&jb.wait_handle->_lock); // allow other jobs to await
                        SwitchToFiber(fiber);                                      // yield back to awaiter fiber, await will put us back at pool
                    }
                    else
                    {
                        // No one is awaiting for us
                        ReleaseSRWLockExclusive((PSRWLOCK)&jb.wait_handle->_lock);
                    }
                }
                else
                {
                    // There may be more jobs for us
                    ReleaseSRWLockExclusive((PSRWLOCK)&jb.wait_handle->_lock);
                }
            }
        }
        else
        {
            // There are no jobs for us or exit is requested, return to worker fiber whom can block us
            fs.l_finished_fiber = GetCurrentFiber();
            SwitchToFiber(fs.l_worker_fiber); // worker fiber will put us back to pool
        }
    }
#endif
}

static int worker_function(TfbContext& fs)
{
#ifdef _WIN32
    while (!fs.should_exit)
    {
        if (fs.no_of_pending_jobs > 0)
        {
            void* work_fiber;
            TinyRingBufferStatus sts = fs.fiber_pool.dequeue(&work_fiber);

            if (sts == TinyRingBufferStatus::SUCCESS)
            {
                SwitchToFiber(work_fiber);
                if (fs.l_finished_fiber != nullptr)
                {
                    fs.fiber_pool.enqueue(fs.l_finished_fiber);
                    fs.l_finished_fiber = nullptr;
                }
            }
            else
            {
                return -1; // no more fibers in the pool
            }
        }
        else
        {
            std::unique_lock<std::mutex> lk(fs.pending_jobs_mx);
            fs.no_job_cv.wait(lk, [&] { return fs.no_of_pending_jobs > 0 || fs.should_exit; });
        }
    }
#endif
    return 0;
}

static void __stdcall start_workers(void* fiber_system)
{
#ifdef _WIN32
    if (fiber_system == nullptr)
        return;

    TfbContext& fs = *(TfbContext*)fiber_system;
    // First worker will start at main fiber
    fs.worker_threads[0] = std::thread([&fs] {
        l_my_fiber_system = &fs;
        fs.l_worker_fiber = ConvertThreadToFiber(nullptr);
        SwitchToFiber(fs.main_fiber);
        if (fs.l_finished_fiber != nullptr)
            fs.fiber_pool.enqueue(fs.l_finished_fiber);
        fs.l_finished_fiber = nullptr;
        ConvertFiberToThread();
    });

    // Other workers will start with worker_function
    for (int i = 1; i < fs.no_of_worker_threads; ++i)
    {
        fs.worker_threads[i] = std::thread([&fs] {
            l_my_fiber_system = &fs;
            fs.l_worker_fiber = ConvertThreadToFiber(nullptr);
            worker_function(fs); // todo(markusl): handle return error code
            ConvertFiberToThread();
        });
    }

    // Wait for worker threads to exit
    for (int i = 0; i < fs.no_of_worker_threads; ++i)
    {
        fs.worker_threads[i].join();
    }
    SwitchToFiber(fs.main_fiber);
#endif
}

} // namespace

int tfb_init_ext(TfbContext** fiber_system, int max_threads)
{
#ifdef _WIN32
    TfbContext* fs = new TfbContext();
    l_my_fiber_system = fs;
    if (fiber_system != nullptr)
        *fiber_system = fs;

    // Init pools etc.
    fs->job_queue.init(TFB_JOB_QUEUE_SIZE);
    fs->fiber_pool.init(TFB_FIBER_POOL_SIZE);

    // -1 since main thread counts
    fs->no_of_worker_threads = std::thread::hardware_concurrency();
    fs->no_of_worker_threads = std::min(fs->no_of_worker_threads, TFB_MAX_NUMBER_OF_THREADS);

    if (max_threads != TFB_ALL_CORES)
        fs->no_of_worker_threads = std::min(fs->no_of_worker_threads, max_threads);

    void** allocated_fibers = nullptr;
    if (fs->fiber_pool.allocate(TFB_NUMBER_OF_FIBERS, &allocated_fibers) != TinyRingBufferStatus::SUCCESS)
        return -1;

    for (int i = 0; i < TFB_NUMBER_OF_FIBERS; ++i)
    {
        void* fiber = CreateFiber(TFB_DEFAULT_STACKSIZE, fiber_main_loop, fs);
        if (fiber == nullptr)
        {
            // DWORD err = GetLastError();
            // todo(markusl): free
            return -1;
        }
        allocated_fibers[i] = fiber;
    }

    // Switch away from main thread and start worker system
    fs->main_fiber = ConvertThreadToFiber(nullptr);
    fs->init_fibers_fiber = CreateFiber(TFB_DEFAULT_STACKSIZE, start_workers, fs);
    SwitchToFiber(fs->init_fibers_fiber); // Lose main fiber from main thread
    // Worker thread will execute from here now
#endif
    return 0;
}

// Must be called from main fiber (eg, from no job)
int tfb_free_ext(TfbContext** fiber_system)
{
#ifdef _WIN32
    TfbContext* fs = l_my_fiber_system;

    if (fs == nullptr)
        return -1;

    if (fiber_system != nullptr && (*fiber_system != fs))
        return -1;

    {
        std::lock_guard<std::mutex> lk(fs->pending_jobs_mx);
        fs->should_exit = true;
    }

    fs->no_job_cv.notify_all();

    SwitchToFiber(fs->l_worker_fiber);

    ConvertFiberToThread();

    // Delete fibers
    DeleteFiber(fs->init_fibers_fiber);
    void** deallocate_fibers = fs->fiber_pool.data();
    for (int i = 0; i < TFB_NUMBER_OF_FIBERS; ++i)
        DeleteFiber(deallocate_fibers[i]);
    fs->fiber_pool.free();
    fs->job_queue.free();

    delete fs;

    l_my_fiber_system = nullptr;
    if (fiber_system != nullptr)
        *fiber_system = nullptr;
#endif
    return 0;
}

int tfb_add_jobdecl_ext(TfbContext* fiber_system, TfbJobDeclaration* job)
{
#ifdef _WIN32
    if (job == nullptr)
        return -1;

    if (job->func == nullptr)
        return 0;

    TfbContext& fs = *(fiber_system == TFB_MY_CONTEXT ? l_my_fiber_system : fiber_system);

    if (job->wait_handle != nullptr)
        reinterpret_cast<std::atomic_int64_t&>(job->wait_handle->_counter)++;

    TinyRingBufferStatus sts = fs.job_queue.enqueue(*job);
    if (sts != TinyRingBufferStatus::SUCCESS)
        return -1;

    {
        std::lock_guard<std::mutex> lk(fs.pending_jobs_mx);
        ++fs.no_of_pending_jobs;
    }
    fs.no_job_cv.notify_one();
#endif
    return 0;
}

// Must have the same WaitHandler*
int tfb_add_jobdecls_ext(TfbContext* fiber_system, TfbJobDeclaration jobs[], int64_t elements)
{
#ifdef _WIN32
    TfbContext& fs = *(fiber_system == TFB_MY_CONTEXT ? l_my_fiber_system : fiber_system);

    if (jobs[0].wait_handle != nullptr)
        reinterpret_cast<std::atomic_int64_t&>(jobs[0].wait_handle->_counter) += elements;

    if (fs.job_queue.enqueue(jobs, elements) != TinyRingBufferStatus::SUCCESS)
        return -1;

    {
        std::lock_guard<std::mutex> lk(fs.pending_jobs_mx);
        fs.no_of_pending_jobs += elements;
    }
    fs.no_job_cv.notify_all();
#endif
    return 0;
}

int tfb_await_ext(TfbContext* fiber_system, TfbWaitHandle* wait_handle)
{
    if (wait_handle == nullptr)
        return -1;
#ifdef _WIN32
    TfbContext& fs = *(fiber_system == TFB_MY_CONTEXT ? l_my_fiber_system : fiber_system);

    AcquireSRWLockExclusive((PSRWLOCK)&wait_handle->_lock);

    // Put this to fiber queue
    if (reinterpret_cast<std::atomic_int64_t&>(wait_handle->_counter).load() == 0)
    {
        ReleaseSRWLockExclusive((PSRWLOCK)&wait_handle->_lock);
        return 0;
    }

    wait_handle->_fiber = GetCurrentFiber();
    fs.l_wait_handle_lock = (PSRWLOCK)&wait_handle->_lock;

    void* new_fiber;
    if (fs.fiber_pool.dequeue(&new_fiber) == TinyRingBufferStatus::SUCCESS)
    {
        SwitchToFiber(new_fiber);
        // put back fiber we yield from to pool
        fs.fiber_pool.enqueue(fs.l_finished_fiber);
        fs.l_finished_fiber = nullptr;
    }
    else
    {
        return -1;
    }
#endif
    return 0;
}
