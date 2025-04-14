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
    graph.allNodes.resize(1000);
    for (int i = 0; i < graph.allNodes.size() - 1; i++)
    {
        for (int n = 0; n < 5; n++)
            graph.setNext(i, i + 1 + rand() % (graph.allNodes.size() - i - 1));
        for (int n = 0; n < 20; n++)
            graph.addResource(i, rand() % 200, rand() % 5 == 0);
    }
}

constexpr uint32_t INVALID_ID = ~0u;

void sanitize_task_graph(TaskGraph &graph)
{
    for (auto &node : graph.allNodes)
        node.visited = false;
    while (true)
    {
        uint32_t taskId = 0;
        for (; taskId < graph.allNodes.size() && graph.allNodes[taskId].visited; taskId++) {}
        if (taskId == graph.allNodes.size())
            break;

    }
}

CompiledTaskGraph compile_task_graph(const TaskGraph &raw)
{
    CompiledTaskGraph compiled;

    // build queues
    /*
    std::vector<uint32_t> assignedQueue;
    assignedQueue.resize(raw.allNodes.size(), INVALID_ID);
    while (true)
    {
        uint32_t taskId = INVALID_ID;
        auto [queueId, taskId] = entries.back();
        entries.pop_back();
        while (taskId != INVALID_ID)
        {
            compiled.queueNodes[queueId].taskQueue.push_back(taskId);
            auto &nextTasks = raw.allNodes[taskId].nextTasks;
            taskId = INVALID_ID;
            for (uint32_t nextTaskId : nextTasks)
                if (!visited[nextTaskId])
                {
                    if ()
                    taskId = nextTaskId;
                }
        }
    }
     */

    return compiled;
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

    assert(graph.validateAndNormalize());

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
