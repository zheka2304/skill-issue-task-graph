#include <iostream>
#include <atomic>
#include <vector>
#include <cassert>

#include "logger.h"
#include "task_graph_execute.h"
#include "optick.h"


using sie::logger::debug;
using namespace si::tg;

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
            .taskFn = +[](void* data, int idx) {
                OPTICK_EVENT("test_task");
                static volatile int x = 0;
                static volatile int y = 0;
                for (int i = 0; i < 1000; i++)
                    x = y;
                // printf("    [%i] test task %i (%i)\n", GroupId, (int) (intptr_t) data, idx);
            },
            .taskVarFn = nullptr,
            .userData = (void*) (i + idxOfs)
        });
        tasks.push_back(id);
    }
    for (int i = 0; i < sz; i++)
    {
        if (i >= sz - 1)
            continue;
        for (int n = 0; n < 0; n++)
            graph.setNext(tasks[i], tasks[i + 1 + rand() % (sz - i - 1)]);
        for (int n = 0; n < 3; n++)
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
    ThreadedTaskGraphExecutor executor;
    {
        OPTICK_FRAME("MainThread");
        OPTICK_EVENT()
        executor.graphPtr = &graph;
        executor.prepareForExecution(thread_num);
        SimpleThreadPool pool;
        pool.executor = &executor;
        pool.windUp(thread_num);
        pool.waitAll();
    }

    /*
    debug("validate", "timed events:");
    for (const auto &evt : executor.getAllTimedEvents())
        debug("validate", "  %s %i", ThreadedTaskGraphExecutor::EVENT_NAMES[int(evt.event)], evt.ids[0]);
    debug("validate", "stats:");
    auto stats = executor.getEventCountStats();
    for (int i = 0; i < int(ThreadedTaskGraphExecutor::Event::NUM); i++)
        debug("validate", "  %s %lli", ThreadedTaskGraphExecutor::EVENT_NAMES[i], stats[i]);
    */
}

int main()
{
    sie::logger::init_default_log_handler("log.txt");
    OPTICK_START_CAPTURE();

    TaskGraph graph1;
    append_random_task_graph<1>(graph1, 1000, 0);
    TaskGraph graph2;
    append_random_task_graph<2>(graph2, 500, 0);
    TaskGraph graph3;
    append_random_task_graph<3>(graph3, 100, 0);
    TaskGraph graph;
    {
        auto t1 = graph.addSubGraphTask({.taskVarFn = +[] (void*) { return 10; }}, &graph1);
        // auto t2 = graph.addSubGraphTask({}, &graph2);
        // auto t3 = graph.addSubGraphTask({.taskVarFn = +[] (void*) { return 5; }}, &graph3);
        // graph.setNext(t1, t2);
    }

    PrebuiltTaskGraph prebuiltGraph;
    assert(si::tg::prebuild_task_graph(prebuiltGraph, graph));
    si::tg::strategy::MergeSubgroupsState state;
    CompiledTaskGraph compiledGraph;
    assert(si::tg::compile_task_graph(compiledGraph, prebuiltGraph, state));

    sie::logger::flush_default_log();
    execute_task_graph(compiledGraph);

    OPTICK_STOP_CAPTURE();
    OPTICK_SAVE_CAPTURE("../test-capture.opt");
    sie::logger::shutdown_default_log_handler();
    return 0;
}
