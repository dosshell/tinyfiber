tinyfiber
============
A cooperative lightweight multithreading system. 

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
        TfbWaitHandle wh;
        TfbJobDeclaration jd{job, param, &wh};
        tfb_add_job(jd);
        tfb_await(wh);
    }
}
```
