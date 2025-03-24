#include <atomic>
#include <condition_variable>


struct ThreadPool
{
    std::condition_variable cond;
};