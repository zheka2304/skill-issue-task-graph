#pragma once

#include "task_graph.h"


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

bool prebuild_task_graph(PrebuiltTaskGraph &prebuilt_graph, const TaskGraph &graph);


struct CompiledTaskGraph;

namespace strategy
{

struct MergeSubgroupsState
{
    struct MergeState
    {
        Vector<int> sgPair;
        int totalValue;
        int pairCnt;
    };
    struct SubgroupData
    {
        uint64_t mask = 0;
        int subgroupIdx = 0;
        FixedVector<uint32_t, 1, true> tasks;
    };
    struct GroupData
    {
        int subgroupCnt = 0;
        FixedVector<SubgroupData, 64, true> subgroups;
        BaseGraph subgroupExclusionGraph;
    };
    struct TaskData
    {
        uint32_t remapTaskId = ~uint32_t(0);
        int depsCnt = 0;
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
    MergeState mergeState;

    MergeSubgroupsState() = default;
    template<typename Allocator>
    explicit MergeSubgroupsState(const Allocator &allocator) :
            groups(allocator), allTasks(allocator), subgroupExclusionGraph(allocator),
            subgroups(allocator), tasks(allocator), nextTasks(allocator), mergeState(allocator)
    {
        mergeState.sgPair = Vector<int>(allocator);
    }
};

}

bool compile_task_graph(CompiledTaskGraph &compiled_graph, PrebuiltTaskGraph &prebuilt_graph, strategy::MergeSubgroupsState &state);

}