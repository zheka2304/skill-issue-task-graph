#pragma once

#include <taskgraph/task_graph.h>


namespace si::tg
{

struct PrebuiltTaskGraph
{
    Vector<TaskData> taskData;
    BaseGraph taskGraph;
    BaseGraph exclusionGraph;

    struct SubGraphData
    {
        uint32_t entryTaskId;
        uint32_t exitTaskId;
        uint32_t tasksStart;
        uint32_t tasksEnd;
    };
    Vector<SubGraphData> subGraphData;
    Vector<uint32_t> subGraphTasks;
    Vector<uint64_t> traversalData;

    PrebuiltTaskGraph() = default;
    template<typename Allocator>
    explicit PrebuiltTaskGraph(const Allocator &allocator) :
            taskData(allocator), taskGraph(allocator), exclusionGraph(allocator), subGraphData(allocator), subGraphTasks(allocator), traversalData(allocator) {}
};

bool prebuild_task_graph(const TaskGraph &graph, PrebuiltTaskGraph *prebuilt_graph);


struct CompiledTaskGraph;

namespace strategy
{

struct MergeSubgroups
{
    struct MergeState
    {
        Vector<int32_t> sgPair;
        int32_t totalValue;
        int32_t pairCnt;
    };
    struct SubgroupData
    {
        uint64_t mask = 0;
        int32_t subgroupIdx = 0;
        FixedVector<uint32_t, 1, true> tasks;
    };
    struct GroupData
    {
        int32_t subgroupCnt = 0;
        FixedVector<SubgroupData, 64, true> subgroups;
        BaseGraph subgroupExclusionGraph;
    };
    struct TaskData
    {
        uint32_t remapTaskId = ~uint32_t(0);
        int32_t depsCnt = 0;
        bool isPendingOnStart = false;
        bool allowToRunInParallelWithItself = false;
        Vector<uint32_t> nextTasks;
    };
    Vector<GroupData> groups;
    Vector<TaskData> allTasks;

    // kept to copy allocator from it
    BaseGraph subgroupExclusionGraph;
    FixedVector<SubgroupData, 64, true> subgroups;
    FixedVector<uint32_t, 1, true> tasks;
    Vector<uint32_t> nextTasks;
    Vector<uint32_t> subgroupsToMerge;
    MergeState mergeState;

    MergeSubgroups() = default;
    template<typename Allocator>
    explicit MergeSubgroups(const Allocator &allocator) :
            groups(allocator), allTasks(allocator), subgroupExclusionGraph(allocator),
            subgroups(allocator), tasks(allocator), nextTasks(allocator),
            subgroupsToMerge(allocator), mergeState(allocator)
    {
        mergeState.sgPair = Vector<int32_t>(allocator);
    }
};

}

template<typename StrategyT>
bool compile_task_graph(PrebuiltTaskGraph &prebuilt_graph, CompiledTaskGraph *compiled_graph, StrategyT *strategy_state_ptr);

template<typename StrategyT>
bool build_and_compile_graph(const TaskGraph &graph, CompiledTaskGraph *compiled_graph, PrebuiltTaskGraph *prebuilt_graph = nullptr, StrategyT *c_state = nullptr)
{
    PrebuiltTaskGraph prebuilt;
    if (prebuilt_graph == nullptr)
        prebuilt_graph = &prebuilt;
    if (!prebuild_task_graph(graph, prebuilt_graph))
        return false;
    StrategyT strategyState;
    if (c_state == nullptr)
        c_state = &strategyState;
    return compile_task_graph(*prebuilt_graph, compiled_graph, c_state);
}

void print_compiled_graph(const CompiledTaskGraph &compiled_graph);

}