tinyfiber
============
A cooperative lightweight multithreading system. 

```cpp
#include <iostream>

const int NO_OF_CHILD_JOBS = 100;

struct ChildJobArgs
{
    int n;
    int* result;
};

void child_job(void* param)
{
    ChildJobArgs args = *(ChildJobArgs*)param;
    int n = args.n;
    args.result[n] = (n + 1) * (n - 1) * (n + 2) * (n - 2);
}

void root_job(void* param)
{
    if (param == nullptr)
        return;

    // Declare and define jobs
    WaitHandle h;
    JobDeclaration child_jobs[NO_OF_CHILD_JOBS];
    ChildJobArgs child_job_args[NO_OF_CHILD_JOBS];
    int result[NO_OF_CHILD_JOBS];

    for (int i = 0; i < NO_OF_CHILD_JOBS; ++i)
    {
        child_job_args[i] = {i, result};
        child_jobs[i] = {child_job, &child_job_args[i], &h};
    }

    // Start jobs
    tinyfiber_add_jobs(child_jobs, NO_OF_CHILD_JOBS);
    tinyfiber_await(h);

    // Sum up the result from all jobs
    int sum = 0;
    for (int i = 0; i < NO_OF_CHILD_JOBS; ++i)
    {
        sum += result[i];
    }

    // Return result
    *reinterpret_cast<int*>(param) = sum;
}

int main(int argc, const char* argv[])
{
    int result;
    tinyfiber_run(root_job, &result);
    std::cout << result << std::endl;
    return 0;
}
```