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
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct TfbContext TfbContext;

    // Internal structure, init to zero to use.
    typedef struct
    {
        void* fiber;
        int64_t counter; // rw will result in UF
        void* lock;
    } TfbWaitHandle;

    typedef struct
    {
        void (*func)(void*);
        void* user_data;
        TfbWaitHandle* wait_handle;
    } TfbJobDeclaration;

    const int TFB_ALL_CORES = 0;
    TfbContext* const TFB_MY_CONTEXT = NULL;

    int tfb_init_ext(TfbContext** fiber_system, int max_threads);

    inline int tfb_init(TfbContext** fiber_system)
    {
        return tfb_init_ext(fiber_system, TFB_ALL_CORES);
    }

    int tfb_free(TfbContext** fiber_system);

    int tfb_add_job_ext(TfbContext* fiber_system, TfbJobDeclaration* job_declaration);

    inline int tfb_add_job(TfbJobDeclaration* job_declaration)
    {
        return tfb_add_job_ext(TFB_MY_CONTEXT, job_declaration);
    }

    int tfb_add_jobs_ext(TfbContext* fiber_system, TfbJobDeclaration jobs[], int64_t elements);

    inline int tfb_add_jobs(TfbJobDeclaration jobs[], int64_t elements)
    {
        return tfb_add_jobs_ext(TFB_MY_CONTEXT, jobs, elements);
    }

    int tfb_await_ext(TfbContext* fiber_system, TfbWaitHandle* wait_handle);

    inline int tfb_await(TfbWaitHandle* wait_handle)
    {
        return tfb_await_ext(TFB_MY_CONTEXT, wait_handle);
    }

#ifdef __cplusplus
}
#endif
