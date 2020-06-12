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

#pragma once
#include <atomic>

namespace tinyfiber
{
struct FiberSystem;

typedef struct
{
    void* fiber;
    std::atomic_int64_t counter;
    void* lock;
} WaitHandle;

typedef struct
{
    void (*func)(void*);
    void* user_data;
    WaitHandle* wait_handle;
} JobDeclaration;

const int ALL_CORES = 0;
FiberSystem* const MY_FIBER_SYSTEM = nullptr;

int tinyfiber_init(FiberSystem** fiber_system, int max_threads = ALL_CORES);
int tinyfiber_free(FiberSystem** fiber_system);
int tinyfiber_add_job(FiberSystem* fiber_system, JobDeclaration& job_declaration);
inline int tinyfiber_add_job(JobDeclaration& job_declaration)
{
    return tinyfiber_add_job(MY_FIBER_SYSTEM, job_declaration);
}
int tinyfiber_add_jobs(FiberSystem* fiber_system, JobDeclaration jobs[], int64_t elements);
inline int tinyfiber_add_jobs(JobDeclaration jobs[], int64_t elements)
{
    return tinyfiber_add_jobs(MY_FIBER_SYSTEM, jobs, elements);
}
int tinyfiber_await(FiberSystem* fiber_system, WaitHandle& wait_handle);
inline int tinyfiber_await(WaitHandle& wait_handle)
{
    return tinyfiber_await(MY_FIBER_SYSTEM, wait_handle);
}

} // namespace tinyfiber
