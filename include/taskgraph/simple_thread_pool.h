// The MIT License(MIT)
//
// Copyright(c) 2025 Eugene Smirnov
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

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
    explicit SimpleThreadPool(int thread_num);

    void windUpThreads(int thread_num);
    void shutdownThreads();
    void executeAndWait(CompiledTaskGraph *graph, bool use_wait);
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
    Vector<std::thread> threads;
    CondVar idleEvent;
    CondVar wakeEvent;
    CondVar doneEvent;
};

}