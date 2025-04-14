#include <iostream>
#include <atomic>
#include <vector>
#include <thread>
#include <condition_variable>
#include <cassert>

#include "logger.h"
#include "task_graph_execute.h"

using sie::logger::debug;
using namespace sie;

void init_random_task_graph(TaskGraph &graph)
{
    srand(1234);
    graph.allNodes.resize(100);
    for (int i = 0; i < graph.allNodes.size(); i++)
    {
        graph.setTaskData(i, +[] (void* data, int idx)
        {
            debug("exec", "    test task %i (%i)", (int) (intptr_t) data, idx);
        }, nullptr, (void*) i);
        if (i >= graph.allNodes.size() - 1)
            continue;
        for (int n = 0; n < 5; n++)
            graph.setNext(i, i + 1 + rand() % (graph.allNodes.size() - i - 1));
        for (int n = 0; n < 5; n++)
            graph.addResource(i, rand() % 100, rand() % 5 == 0);
    }
}

void execute_task_graph(CompiledTaskGraph &graph, int thread_num = 1)
{
    ThreadedTaskGraphExecutor executor;
    executor.graphPtr = &graph;
    executor.prepareForExecution(thread_num);
    SimpleThreadPool pool;
    pool.executor = &executor;
    pool.windUp(thread_num);
    pool.waitAll();

    debug("validate", "stats:");
    auto stats = executor.getEventCountStats();
    for (int i = 0; i < int(ThreadedTaskGraphExecutor::Event::NUM); i++)
        debug("validate", "  %s %lli", ThreadedTaskGraphExecutor::EVENT_NAMES[i], stats[i]);
    debug("validate", "timed events:");
    for (const auto &evt : executor.getAllTimedEvents())
        debug("validate", "  %s %i", ThreadedTaskGraphExecutor::EVENT_NAMES[int(evt.event)], evt.ids[0]);
}


int main()
{

    sie::logger::init_default_log_handler("log.txt");

    TaskGraph graph;
    if (1)
        init_random_task_graph(graph);
    else
    {
        auto t0 = graph.addTask();
        auto t1 = graph.addTask();
        auto t2 = graph.addTask();
        auto t3 = graph.addTask();
        auto t4 = graph.addTask();
        auto t5= graph.addTask();
        graph.setNext(t0, t1);
        graph.setNext(t1, t2);
        graph.setNext(t1, t3);
        graph.setNext(t3, t4);
    }

    CompiledTaskGraph compiledGraph;
    assert(graph.validateAndNormalize(compiledGraph));
    execute_task_graph(compiledGraph);

    /*
    CompiledTaskGraph graph;
    graph.queueNodes.resize(21);
    graph.rebuildTree();
    for (int i = 0; i < 10; i++)
        graph.setTreeNode(i * 2);
    //graph.setTreeNode(5);
    //graph.setTreeNode(8);
    //graph.setTreeNode(10);
    sie::logger::flush_default_log();

    {
        TaskGraphExecutor tree;
        tree.graph = &graph;
        tree.windUp(8);
        tree.wakeAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        tree.shutdown();
    }*/

    sie::logger::shutdown_default_log_handler();
    return 0;
}
