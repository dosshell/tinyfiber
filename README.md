tinyfiber
============
A cooperative lightweight multithreading system. 

```cpp
#include "tinyfiber.h"

void job(void* param);

int main(int argc, const char* argv[])
{
    tinyfiber_init();
    std::atomic_int64_t depth = 3;
    job(&depth);
    tinyfiber_free();
}

void job(void* param)
{
    std::atomic_int64_t* depth = (std::atomic_int64_t*)param;

    (*depth)--;

    if (*depth > 0)
    {
        // Add job
        WaitHandle wh;
        JobDeclaration jd{job, param, &wh};
        tinyfiber_add_job(jd);
        tinyfiber_await(wh);
    }
}
```