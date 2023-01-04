tinyfiber
============
A cooperative lightweight job-system with a C-api. This library uses a N:M mapping between working threads and fibers. You can have thousands of fibers running from a thread pool without ever have a kernel thread switch.

Online documentation: https://tinyfiber.readthedocs.io/

This library currently only supports Windows.

```cpp
#include "tinyfiber.h"

void job(void* param);

int main(int argc, const char* argv[])
{
    tfb_init();
    std::atomic_int64_t depth = 3;
    job(&depth);
    tfb_free();
}

void job(void* param)
{
    std::atomic_int64_t* depth = (std::atomic_int64_t*)param;

    (*depth)--;

    if (*depth > 0)
    {
        // Add job
        TfbWaitHandle wh{};
        tfb_add_job(job, param, &wh);
        tfb_await(&wh);
    }
}
```

A fiber system is a way to switch thread without involving the kernel. This makes the switches fast with a low over-head. A fiber can be seen as a data structure containing the current execution context, i.e. the states of the registers and a stack, importantly this includes the instruction pointer.

A fiber switch will not change thread, which means that a working pool of threads can exchange fibers. This means that you can not trust local thread storage variables between yields; mutex and semaphores will not work. This also includes all 3:rd part library functions you may call. If your project needs to use TLS in a matter that is not compatible with these constraints I recommend that you consider UMS (User-Mode Scheduling) or C++20 coroutines instead.

# Diagram of internal switching
```mermaid
sequenceDiagram
    Note over main_fiber: tfb_init_ext(...)
    main_fiber->>worker_threads_fiber: switch fiber
    Note over worker_threads_fiber: start_workers(...)
    worker_threads_fiber->>l_worker_fiber[a]: thread
    worker_threads_fiber->>l_worker_fiber[b]: thread
    l_worker_fiber[a]->>main_fiber: switch
    Note over l_worker_fiber[b]: worker_function(...)
    Note over main_fiber: tfb_add_jobdecls_ext(..., &job)
    l_worker_fiber[b] ->> fiber_pool[0]: switch fiber
    Note over fiber_pool[0]: fiber_main_loop()
    Note over fiber_pool[0]: job.func(job.user_data)
    Note over main_fiber: tfb_await_ext(..., &job)
    main_fiber->>fiber_pool[1]: switch fiber
    Note over fiber_pool[1]: fiber_main_loop()
    Note over fiber_pool[1]: job.func(job.user_data)
    fiber_pool[0]->>main_fiber: switch (job had awaiting fiber)
    Note over main_fiber: tfb_free_ext(...)
    main_fiber ->> l_worker_fiber[b]: switch
    Note over l_worker_fiber[b]: thread exits
    fiber_pool[1] ->> l_worker_fiber[a]: switch
    Note over l_worker_fiber[a]: thread exits
    Note over worker_threads_fiber: threads.join()
    worker_threads_fiber ->> main_fiber: switch
```
