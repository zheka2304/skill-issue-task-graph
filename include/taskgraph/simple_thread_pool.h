#pragma once

#include <thread>
#include <condition_variable>

#include <taskgraph/task_graph_execute.h>


namespace si::tg
{

struct SimpleThreadPool
{
    SimpleThreadPool() = default;
    ~SimpleThreadPool();
    SimpleThreadPool(const SimpleThreadPool&) = delete;
    SimpleThreadPool& operator=(const SimpleThreadPool&) = delete;

    void windUpThreads(int thread_num);
    void shutdownThreads();
    void execute(CompiledTaskGraph *graph, bool use_wait);
    void waitDone();
    void wakeAll();

    struct CondVar
    {
        std::mutex mutex;
        std::condition_variable condVar;
        std::atomic<uint64_t> word = 0;

        void wakeThread(int tid) { wakeMask(uint64_t(1) << uint64_t(tid)); }
        void waitThread(int thread_id);
        void wakeMask(uint64_t mask);
        void waitMask(uint64_t mask);
        void waitMaskImpl(std::unique_lock<std::mutex> &lock, uint64_t mask);
    };

    static thread_local int thisThreadId;

private:
    void doThread(int thread_id);
    static void exec(SimpleThreadPool* self, int thread_id);

public:
    ThreadedTaskGraphExecutor executor;

private:
    bool running = false;
    bool useWait = false;
    std::vector<std::thread> threads;
    CondVar idleEvent;
    CondVar wakeEvent;
    CondVar doneEvent;
};

}