#include <iostream>
#include <atomic>
#include <vector>
#include <cassert>

#include "optick.h"
#include "taskgraph/task_graph_compile.h"
#include "taskgraph/task_graph_execute.h"
#include "taskgraph/simple_thread_pool.h"


using namespace si::tg;

std::atomic<int64_t> dbgTasksDone = 0;

void some_work()
{
    // OPTICK_EVENT("test_task");
    volatile int x = 0;
    volatile int y = 0;
    for (int i = 0; i < 1000; i++)
        x = y;
    (void) x;
}

template<int GroupId>
[[clang::optnone]]
void append_random_task_graph(si::tg::TaskGraph &graph, int sz, int res_ofs)
{
    std::vector<TaskId> tasks;
    tasks.reserve(sz);
    int idxOfs = graph.taskData.size();
    for (int i = 0; i < sz; i++)
    {
        TaskId id = graph.addTask({
            .taskFn = +[]([[maybe_unused]] void* data, [[maybe_unused]] int idx) {
                // dbgTasksDone.fetch_add(1, std::memory_order_relaxed);
                some_work();
                // printf("    [%i] test task %i (%i)\n", GroupId, (int) (intptr_t) data, idx);
            },
            .taskVarFn = nullptr,
            .userData = (void*) intptr_t(i + idxOfs)
        });
        tasks.push_back(id);
    }
    for (int i = 0; i < sz; i++)
    {
        if (i >= sz - 1)
            continue;
        for (int n = 0; n < 0; n++)
            graph.setNext(tasks[i], tasks[i + 1 + rand() % (sz - i - 1)]);
        for (int n = 0; n < 5; n++)
        {
            uint64_t res = res_ofs + rand() % 200;
            si::tg::ResourceUsage usage = rand() % 5 == 0 ? si::tg::ResourceUsage::LOCKING : si::tg::ResourceUsage::SHARED;
            graph.setResourceUsage(tasks[i], res, usage);
            assert(graph.getResourceUsage(tasks[i], res) == usage);
        }
    }
}

void init_random_task_graph(TaskGraph &graph)
{
    srand(1234);
    append_random_task_graph<0>(graph, 1000, 0);
    // append_random_task_graph(graph, 1000, 190);
}

void execute_task_graph(CompiledTaskGraph &graph, int thread_num = 4)
{
    printf("[exec] start\n");
    SimpleThreadPool pool;
    pool.windUpThreads(thread_num);

    std::vector<uint64_t> times;
    [[maybe_unused]] Array<int64_t, uint8_t(si::tg::ThreadedTaskGraphExecutor::Event::NUM)> stats = {0};

    for (int i = 0; i < 1000; i++)
    {
        // if (i % 100 == 0) sie::logger::debug("exec", "iter %i", i);
        OPTICK_FRAME("MainThread");
        OPTICK_EVENT()
        dbgTasksDone = 0;
        const auto beginTime = std::chrono::high_resolution_clock::now();
        pool.execute(&graph, true);
        const auto endTime = std::chrono::high_resolution_clock::now();
        times.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - beginTime).count());
        pool.executor.getEventCountStats(stats);
        // SI_TG_ASSERT(dbgTasksDone == 1000);
    }

    {
        uint64_t maxNS = 0;
        uint64_t minNS = ~uint64_t(0);
        uint64_t totalNS = 0;
        for (uint64_t t : times)
        {
            maxNS = std::max(maxNS, t);
            minNS = std::min(minNS, t);
            totalNS += t;
        }
        printf("[exec] time: max=%.3lfms min=%.3lfms avg=%.3lfms", double(maxNS) * 1e-6, double(minNS) * 1e-6, double(totalNS) / double(times.size()) * 1e-6);
    }
#if SI_TG_ENABLE_DEBUG_STAT_EVENTS
    printf("[validate] stats:");
    for (int i = 0; i < int(ThreadedTaskGraphExecutor::Event::NUM); i++)
        printf("[validate]   %s %lli", ThreadedTaskGraphExecutor::EVENT_NAMES[i], stats[i]);
#endif
}

int main()
{
    OPTICK_START_CAPTURE();

    TaskGraph graph1;
    // graph1.addTask({.taskFn = +[] (void*, int i) { dbgTasksDone++; }, .taskVarFn = +[] (void*) { return 100; } });
    append_random_task_graph<1>(graph1, 1000, 0);
    TaskGraph graph2;
    // append_random_task_graph<2>(graph2, 1000, 0);
    TaskGraph graph;
    {
        [[maybe_unused]] auto t1 = graph.addSubGraphTask({.taskVarFn = +[] (void*) { return 10; }}, &graph1);
        // auto t2 = graph.addSubGraphTask({.taskVarFn = +[] (void*) { return 1; }}, &graph2);
        // auto t3 = graph.addSubGraphTask({.taskVarFn = +[] (void*) { return 5; }}, &graph3);
        // graph.setNext(t1, t2);
    }

    PrebuiltTaskGraph prebuiltGraph;
    assert(si::tg::prebuild_task_graph(prebuiltGraph, graph));
    si::tg::strategy::MergeSubgroupsState state;
    CompiledTaskGraph compiledGraph;
    assert(si::tg::compile_task_graph(compiledGraph, prebuiltGraph, state));

    execute_task_graph(compiledGraph);

    OPTICK_STOP_CAPTURE();
    OPTICK_SAVE_CAPTURE("../test-capture.opt");
    return 0;
}
