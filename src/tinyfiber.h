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
class WaitHandle
{
public:
    WaitHandle() : counter(0), fiber(nullptr), lock(nullptr)
    {
    }
    void* fiber;
    std::atomic_int64_t counter;
    void* lock;
};

struct JobDeclaration
{
    void (*func)(void*);
    void* user_data;
    WaitHandle* wait_handle;
};

const int ALL_CORES = 0;
int tinyfiber_run(void (*entrypoint)(void*), void* user_param, int max_threads = ALL_CORES);
int tinyfiber_add_job(JobDeclaration& job_declaration);
int tinyfiber_add_jobs(JobDeclaration jobs[], int64_t elements);
int tinyfiber_await(WaitHandle& wait_handle);

} // namespace tinyfiber
