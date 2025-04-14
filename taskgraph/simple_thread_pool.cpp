#include "simple_thread_pool.h"

#define TP_VERBOSE(...) // debug("exec", __VA_ARGS__)
#define TP_SKIP_EXECUTION 0


namespace si::tg
{

// SimpleThreadPool

void SimpleThreadPool::windUpThreads(int thread_num)
{
    shutdownThreads();
    running = true;
    threads.reserve(thread_num);
    for (int i = 0; i < thread_num; i++)
        threads.emplace_back(exec, this, i);
    waitDone();
}

void SimpleThreadPool::shutdownThreads()
{
    running = false;
    if (threads.empty())
        return;
    wakeAll();
    for (std::thread& t: threads)
        t.join();
    threads.clear();
}

void SimpleThreadPool::execute(si::tg::CompiledTaskGraph* graph)
{
    doneEvent.word.store(0, std::memory_order_relaxed);
    executor.graphPtr = graph;
#if !TP_SKIP_EXECUTION
    executor.prepareForExecution(threads.size());
#endif
    wakeAll();
    waitDone();
#if !TP_SKIP_EXECUTION
    executor.validateAllDone();
#endif
}

void SimpleThreadPool::waitDone()
{
    TP_VERBOSE("all done - wait start");
    doneEvent.waitMask((uint64_t(1) << uint64_t(threads.size())) - 1);
    TP_VERBOSE("all done - wait end");
}

SimpleThreadPool::~SimpleThreadPool() { shutdownThreads(); }

thread_local int SimpleThreadPool::thisThreadId = -1;

void SimpleThreadPool::doThread(int thread_id)
{
    thisThreadId = thread_id;
    // sie::logger::debug("worker", "startup %i", thread_id);
    SI_TG_PROFILE_INTERNAL("worker_thread");
    while (running)
    {
        {
            // debug("exec", "[%i] idle started %u", thread_id, uint32_t(wakeEvent.wakeMask.load()));
            SI_TG_PROFILE_INTERNAL("thread_idle");
            TP_VERBOSE("[%i] thread wait", thread_id);
            doneEvent.wakeThread(thread_id);
            idleEvent.waitThread(thread_id);
            TP_VERBOSE("[%i] thread wake", thread_id);
            // debug("exec", "[%i] idle ended", thread_id);
        }
        while (running)
        {
            constexpr bool USE_WAIT = false;
            const auto wakeThreadsFn = [this](uint64_t mask) {
                SI_TG_PROFILE_EXCESSIVE("wake_threads");
                internal::iter_set_bits(mask, [&](uint32_t i) {
                    // sie::logger::debug("worker", "[%i] wake", int(i));
                });
                wakeEvent.wakeMask(mask);
            };

#if !TP_SKIP_EXECUTION
            bool end = false;
            constexpr int MAX_ATTEMPTS = 32;
            for (int i = 0; i < MAX_ATTEMPTS; i++)
            {
                const ThreadedTaskGraphExecutor::ThreadResult result = executor.doThread(thread_id,
                                                                                         USE_WAIT ? WakeThreadsCallback(wakeThreadsFn) : WakeThreadsCallback());
                end = result != ThreadedTaskGraphExecutor::ThreadResult::WAIT;
                if (end)
                    break;
                // SI_TG_PROFILE_EXCESSIVE("thread_yield");
                // std::this_thread::yield();
            }

            if (end)
#endif
                break;

            if (USE_WAIT)
            {
                SI_TG_PROFILE_INTERNAL("thread_wait");
                // sie::logger::debug("worker", "[%i] wait start", int(thread_id));
                wakeEvent.waitThread(thread_id);
                // sie::logger::debug("worker", "[%i] wait end", int(thread_id));
            }
        }
    }
    // sie::logger::debug("worker", "shutdown %i", thread_id);
    thisThreadId = -1;
}

void SimpleThreadPool::wakeAll()
{
    SI_TG_ASSERT(doneEvent.word == 0);
    TP_VERBOSE("wake all");
    idleEvent.wakeMask((uint64_t(1) << uint64_t(threads.size())) - 1);
}

void SimpleThreadPool::exec(SimpleThreadPool* self, int thread_id)
{
    char threadName[128];
    sprintf_s(threadName, 128, "WorkerThread_%i", thread_id);
    SI_TG_PROFILE_THREAD(threadName)
    self->doThread(thread_id);
}

void SimpleThreadPool::CondVar::wakeMask(uint64_t mask)
{
    if (!mask)
        return;
    mutex.lock();
    word.fetch_or(mask, std::memory_order_relaxed);
    mutex.unlock();
    condVar.notify_all();
}

void SimpleThreadPool::CondVar::waitThread(int thread_id)
{
    waitMask(uint64_t(1) << uint64_t(thread_id));
}

void SimpleThreadPool::CondVar::waitMask(uint64_t mask)
{
    std::unique_lock lock(mutex);
    waitMaskImpl(lock, mask);
}

void SimpleThreadPool::CondVar::waitMaskImpl(std::unique_lock<std::mutex> &lock, uint64_t mask)
{
    condVar.wait(lock, [&] {
        uint64_t w = word.load(std::memory_order_relaxed);
        if ((w & mask) != mask)
            return false;
        while (!word.compare_exchange_strong(w, w & ~mask)) { SI_TG_ASSERT((w & mask) == mask); }
        return true;
    });
}

}