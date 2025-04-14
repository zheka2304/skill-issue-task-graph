#include "task_graph.h"

#include <cassert>
#include <sstream>
#include <unordered_map>
#include "logger.h"
#include "task_graph_execute.h"


namespace si::tg
{

using namespace sie;
using sie::logger::error;

// TaskGraph
TaskId TaskGraph::addTask(const TaskData &data)
{
    TaskId id(taskData.size());
    taskData.push_back(data);
    taskSubGraph.push_back(nullptr);
    taskOrderGraph.resize(taskData.size());
    return id;
}

TaskId TaskGraph::addSubGraphTask(const TaskData &data, const TaskGraph *graph)
{
    TaskId id(taskData.size());
    SI_TG_ASSERT(data.taskFn == nullptr);
    taskData.push_back(data);
    taskSubGraph.push_back(graph);
    taskOrderGraph.resize(taskData.size());
    return id;
}

void TaskGraph::setTaskData(TaskId task, const TaskData &data)
{
    SI_TG_ASSERT(task.value() < taskData.size());
}

void TaskGraph::setNext(TaskId task, TaskId next, bool set)
{
    SI_TG_ASSERT(task.value() != next.value());
    taskOrderGraph.setConnected(task.value(), next.value(), set);
}

void TaskGraph::setResourceUsage(TaskId task, uint64_t resource_id, ResourceUsage usage)
{
    auto [begin, end] = taskResourceUsage.equal_range(resource_id);
    if (begin == end)
    {
        if (usage == ResourceUsage::NOT_USED)
            return;
        TaskResourceUsageData usageData;
        usageData.task = task.value();
        usageData.lockingBit = usage == ResourceUsage::LOCKING;
        taskResourceUsage.emplace(resource_id, usageData);
        return;
    }
    bool found = false;
    for (auto it = begin; it != end; )
    {
        if (it->second.task == task.value())
        {
            SI_TG_ASSERT(!found);
            if (usage == ResourceUsage::NOT_USED)
            {
                it = taskResourceUsage.erase(it);
                continue;
            }
            it->second.lockingBit = usage == ResourceUsage::LOCKING;
            found = true;
        }
        ++it;
    }
    if (!found && usage != ResourceUsage::NOT_USED)
    {
        TaskResourceUsageData usageData;
        usageData.task = task.value();
        usageData.lockingBit = usage == ResourceUsage::LOCKING;
        taskResourceUsage.emplace(resource_id, usageData);
    }
}

ResourceUsage TaskGraph::getResourceUsage(si::tg::TaskId task_id, uint64_t resource_id) const
{
    auto [begin, end] = taskResourceUsage.equal_range(resource_id);
    for (auto it = begin; it != end; ++it)
    {
        if (it->second.task == task_id.value())
            return it->second.lockingBit ? ResourceUsage::LOCKING : ResourceUsage::SHARED;
    }
    return ResourceUsage::NOT_USED;
}

void TaskGraph::clear()
{
    taskOrderGraph.resize(0);
    taskData.clear();
    taskResourceUsage.clear();
}

static int task_graph_resolve_size(const TaskGraph &graph)
{
    int count = 0;
    for (int i = 0; i < graph.taskData.size(); i++)
        if (graph.taskSubGraph[i] != nullptr)
            count += 2 + task_graph_resolve_size(*graph.taskSubGraph[i]); // entry + exit tasks
        else
            count++;
    return count;
}

static void resolve_resources_and_dependencies(PrebuiltTaskGraph &prebuilt, const TaskGraph &graph)
{
    // copy tasks
    uint32_t tasksOffset = prebuilt.taskData.size();
    for (const TaskData &task : graph.taskData)
        prebuilt.taskData.push_back(task); // add subgraph entry tasks as-is with everything else, keep indices
    SI_TG_ASSERT(prebuilt.taskGraph.size() >= prebuilt.taskData.size());
    SI_TG_ASSERT(prebuilt.exclusionGraph.size() >= prebuilt.taskData.size());

    // move task order graph
    for (uint32_t taskId = 0; taskId < graph.taskOrderGraph.size(); taskId++)
        graph.taskOrderGraph.iterEdges(taskId, [&] (uint32_t next_id) {
            prebuilt.taskGraph.setConnected(tasksOffset + taskId, tasksOffset + next_id, true);
        });

    // all tasks, that can run in parallel (not sequenced), but aren't allowed because of resource usage
    for (auto baseIt = graph.taskResourceUsage.begin(); baseIt != graph.taskResourceUsage.end(); )
    {
        auto begin = baseIt;
        while (baseIt != graph.taskResourceUsage.end() && begin->first == baseIt->first)
            baseIt++;
        auto end = baseIt;
        for (auto it1 = begin; it1 != end; it1++)
            for (auto it2 = it1; it2 != end; it2++)
            {
                if (it1->second.lockingBit || it2->second.lockingBit)
                    prebuilt.exclusionGraph.setConnectedBoth(tasksOffset + it1->second.task, tasksOffset + it2->second.task, true);
            }
    }

    for (int i = 0; i < graph.taskData.size(); i++)
    {
        if (graph.taskSubGraph[i] != nullptr)
        {
            PrebuiltTaskGraph::SubGraphData subGraphData;
            subGraphData.entryTaskId = tasksOffset + i;
            subGraphData.exitTaskId = prebuilt.taskData.size();
            prebuilt.taskData.emplace_back();
            const TaskGraph& subGraph = *graph.taskSubGraph[i];
            SI_TG_ASSERT(prebuilt.taskGraph.size() >= prebuilt.taskData.size());
            SI_TG_ASSERT(prebuilt.exclusionGraph.size() >= prebuilt.taskData.size());

            // ordering
            prebuilt.taskGraph.iterEdges(subGraphData.entryTaskId, [&] (uint32_t v) {
                prebuilt.taskGraph.setConnected(subGraphData.entryTaskId, v, false);
                prebuilt.taskGraph.setConnected(subGraphData.exitTaskId, v, true);
            });
            prebuilt.taskGraph.setConnected(subGraphData.entryTaskId, subGraphData.exitTaskId, true);
            prebuilt.exclusionGraph.setConnectedBoth(subGraphData.entryTaskId, subGraphData.entryTaskId, true);
            prebuilt.exclusionGraph.setConnectedBoth(subGraphData.exitTaskId, subGraphData.exitTaskId, true);

            // rely on the same tasks will be added at the start of recursive call
            subGraphData.tasksStart = prebuilt.subGraphTasks.size();
            for (int j = 0; j < subGraph.taskData.size(); j++)
            {
                // intentionally include subgraph tasks, these tasks will become startup
                // tasks for child subgraphs, so this subgraph will reset their state, in case it restarts
                const uint32_t subGraphTaskId = j + prebuilt.taskData.size();
                prebuilt.subGraphTasks.push_back(subGraphTaskId);
                // ordering for subgraph tasks
                prebuilt.taskGraph.setConnected(subGraphData.entryTaskId, subGraphTaskId, true);
                prebuilt.taskGraph.setConnected(subGraphTaskId, subGraphData.exitTaskId, true);
            }
            subGraphData.tasksEnd = prebuilt.subGraphTasks.size();
            prebuilt.subGraphData.push_back(subGraphData);
            resolve_resources_and_dependencies(prebuilt, subGraph);
        }
    }
}

bool prebuild_task_graph(PrebuiltTaskGraph &prebuilt, const TaskGraph &graph)
{
    logger::debug("compile", "building graph");
    const int totalTaskCount = task_graph_resolve_size(graph);
    prebuilt.taskGraph.resize(totalTaskCount);
    prebuilt.exclusionGraph.resize(totalTaskCount);
    prebuilt.taskData.reserve(totalTaskCount);
    prebuilt.taskGraph.setFlagCount(2);
    prebuilt.subGraphTasks.reserve(totalTaskCount);

    resolve_resources_and_dependencies(prebuilt, graph);
    SI_TG_ASSERT(totalTaskCount == prebuilt.taskData.size());

    // validate no cycles
    Vector<uint64_t> &stack = prebuilt.traversalData;
    bool isValid = true;
    prebuilt.taskGraph.setAllFlags(0, false);
    prebuilt.taskGraph.setAllFlags(1, true);
    for (uint32_t v = 0; v < prebuilt.taskGraph.size(); v++)
    {
        base_graph_dfs(prebuilt.taskGraph, v, 0, [&] (uint32_t vv, auto dfs_next) {
            if (v != vv)
                prebuilt.taskGraph.setFlag(vv, 1, false);
            if (std::find(stack.begin(), stack.end(), vv) != stack.end())
            {
                error("graph", "node cycle detected! %i", v);
                isValid = false;
            }
            stack.push_back(vv);
            dfs_next();
            stack.pop_back();
        });
    }
    if (!isValid)
        return false;

    // normalize order
    for (uint32_t baseId = 0; baseId < prebuilt.taskGraph.size(); baseId++)
    {
        prebuilt.taskGraph.setAllFlags(0, false);
        prebuilt.traversalData.clear();
        prebuilt.traversalData.push_back(baseId);
        prebuilt.taskGraph.setFlag(baseId, 0, true);
        int bfsPos = 0;
        while (bfsPos < prebuilt.traversalData.size())
        {
            const uint64_t idAndDepth = uint64_t(prebuilt.traversalData[bfsPos++]);
            const uint32_t curId = uint32_t(idAndDepth);
            const uint32_t depth = uint32_t(idAndDepth >> uint64_t(32));
            if (depth != 0)
                prebuilt.exclusionGraph.setConnectedBoth(baseId, curId, false);
            if (depth > 1)
            {
                if (prebuilt.taskGraph.isConnected(baseId, curId))
                    logger::debug("prebuilt", "disconnect %i %i", baseId, curId);
                prebuilt.taskGraph.setConnected(baseId, curId, false);
            }
            prebuilt.taskGraph.iterEdges(curId, [&] (uint32_t nextId) {
                if (prebuilt.taskGraph.getFlag(nextId, 0))
                    return;
                prebuilt.taskGraph.setFlag(nextId, 0, true);
                prebuilt.traversalData.push_back((uint64_t(depth + 1) << uint64_t(32)) | uint64_t(nextId));
            });
        }
    }

    SI_TG_ASSERT(prebuilt.taskData.size() == prebuilt.taskGraph.size());
    SI_TG_ASSERT(prebuilt.taskData.size() == prebuilt.exclusionGraph.size());

    return true;
}


bool compile_task_graph(CompiledTaskGraph &compiled, PrebuiltTaskGraph &prebuilt, strategy::MergeSubgroupsState &c_state)
{
    using GroupData = strategy::MergeSubgroupsState::GroupData;
    using SubgroupData = strategy::MergeSubgroupsState::SubgroupData;
    using TaskData = strategy::MergeSubgroupsState::TaskData;
    using MergeState = strategy::MergeSubgroupsState::MergeState;

    // build groups
    prebuilt.exclusionGraph.setFlagCount(1);

    {
        GroupData group;
        group.subgroups = c_state.subgroups;
        group.subgroupExclusionGraph = std::move(c_state.subgroupExclusionGraph);
        SubgroupData subgroup;
        subgroup.tasks = c_state.tasks;
        prebuilt.exclusionGraph.setAllFlags(0, false);
        for (uint32_t v = 0; v < prebuilt.exclusionGraph.size(); v++)
        {
            group.subgroups.clear();
            group.subgroupExclusionGraph.resize(0);
            base_graph_dfs(prebuilt.exclusionGraph, v, 0, [&](uint32_t vv, auto dfs_next) {
                subgroup.tasks.clear();
                subgroup.tasks.push_back(vv);
                group.subgroups.push_back(std::move(subgroup));
                dfs_next();
            });
            if (!group.subgroups.empty())
            {
                group.subgroupExclusionGraph.resize(group.subgroups.size());
                group.subgroupExclusionGraph.setFlagCount(1);
                for (uint32_t i1 = 0; i1 < group.subgroups.size(); i1++)
                {
                    group.subgroupExclusionGraph.setConnectedBoth(i1, i1, true);
                    for (uint32_t i2 = i1 + 1; i2 < group.subgroups.size(); i2++)
                    {
                        uint32_t t1 = group.subgroups[i1].tasks.back();
                        uint32_t t2 = group.subgroups[i2].tasks.back();
                        group.subgroupExclusionGraph.setConnectedBoth(i1, i2, prebuilt.exclusionGraph.isConnected(t1, t2));
                    }
                }
                c_state.groups.push_back(std::move(group));
            }
        }
    }

    // merge subgroups
    for (GroupData &group : c_state.groups)
    {
        group.subgroupCnt = group.subgroups.size();

        auto mergeSubgroups = [&] (uint64_t dst, uint64_t src)
        {
            // logger::debug("compile", "  merge %i <- %i", dst, src);
            SI_TG_ASSERT(src != dst);
            SI_TG_ASSERT(!group.subgroups[src].tasks.empty());
            SI_TG_ASSERT(!group.subgroups[dst].tasks.empty());
            group.subgroupCnt--;
            // logger::debug("", "merged %i <- %i", int(dst), int(src));
            for (uint32_t t : group.subgroups[src].tasks)
                group.subgroups[dst].tasks.push_back(t);
            group.subgroups[src].tasks.clear();
            group.subgroupExclusionGraph.setConnectedBoth(src, dst, false);
            group.subgroupExclusionGraph.iterEdges(src, [&] (uint32_t v) {
                if (v == src)
                    return;
                // int cnt = group.subgroupExclusionGraph.countEdges(v);
                group.subgroupExclusionGraph.setConnectedBoth(src, v, false);
                group.subgroupExclusionGraph.setConnectedBoth(dst, v, true);
            });
        };

        const auto calcPairMergeValue = [&] (uint32_t sg1, uint32_t sg2)
        {
            SI_TG_ASSERT(sg1 != sg2);
            const BaseGraphEdgesRef edges1 = group.subgroupExclusionGraph.getEdges(sg1);
            const BaseGraphEdgesRef edges2 = group.subgroupExclusionGraph.getEdges(sg2);
            const bool isConnected = group.subgroupExclusionGraph.isConnected(sg1, sg2);
            int value = isConnected ? 1 : 0; // +1 edge eliminated
            group.subgroupExclusionGraph.setConnectedBoth(sg1, sg2, false);
            for (int i = 0; i < edges1.size(); i++)
                value += internal::count_set_bits(edges1[i] & edges2[i]); // unified edges will be eliminated
            group.subgroupExclusionGraph.setConnectedBoth(sg1, sg2, isConnected);
            return value;
        };

        const auto calcPairSetValue = [&] (MergeState &state, uint32_t sg1, uint32_t sg2)
        {
            if (state.sgPair[sg1] == sg2)
            {
                SI_TG_ASSERT(state.sgPair[sg2] == sg1);
                return 0;
            }
            int value = calcPairMergeValue(sg1, sg2);
            if (state.sgPair[sg1] >= 0)
                value -= calcPairMergeValue(sg1, state.sgPair[sg1]);
            if (state.sgPair[sg2] >= 0)
                value -= calcPairMergeValue(sg2, state.sgPair[sg2]);
            return value;
        };

        const auto makePair = [&] (MergeState &state, uint32_t sg1, uint32_t sg2)
        {
            int valueChange = calcPairSetValue(state, sg1, sg2);
            state.totalValue += valueChange;
            if (state.sgPair[sg1] >= 0)
            {
                //logger::debug("compile", "    unpair %i %i", sg1, sgPair[sg1]);
                state.sgPair[state.sgPair[sg1]] = -1;
                state.pairCnt--;
            }
            if (state.sgPair[sg2] >= 0)
            {
                //logger::debug("compile", "    unpair %i %i", bestIdx, sgPair[bestIdx]);
                state.sgPair[state.sgPair[sg2]] = -1;
                state.pairCnt--;
            }
            //logger::debug("compile", "    pair %i %i", sg1, bestIdx);
            state.sgPair[sg2] = sg1;
            state.sgPair[sg1] = sg2;
            state.pairCnt++;
            return valueChange;
        };

        const auto resetMergeState = [&] (MergeState &state)
        {
            state.sgPair.clear();
            state.sgPair.resize(group.subgroups.size(), -1);
            state.totalValue = 0;
            state.pairCnt = 0;
        };

        std::vector<uint32_t> subgroupsToMerge;
        subgroupsToMerge.reserve(group.subgroups.size());

        while (group.subgroupCnt > 64)
        {
            subgroupsToMerge.clear();
            for (uint32_t sg = 0; sg < group.subgroups.size(); sg++)
            {
                if (group.subgroups[sg].tasks.empty())
                    continue;
                subgroupsToMerge.push_back(sg);
            }
            std::sort(subgroupsToMerge.begin(), subgroupsToMerge.end(), [&] (uint32_t a, uint32_t b) {
                return group.subgroupExclusionGraph.getEdges(a).count() > group.subgroupExclusionGraph.getEdges(b).count();
            });
            while (int(group.subgroupCnt) - int(subgroupsToMerge.size()) / 2 < 64)
                subgroupsToMerge.pop_back();

            const auto runSinglePass = [&] (MergeState &state)
            {
                while (true)
                {
                    int valueChange = 0;
                    for (uint32_t sg1: subgroupsToMerge)
                    {
                        int bestVal = -1;
                        int bestIdx = -1;
                        for (uint32_t sg2: subgroupsToMerge)
                        {
                            if (sg1 == sg2)
                                continue;
                            int val = calcPairSetValue(state, sg1, sg2);
                            if (bestVal < val)
                            {
                                bestIdx = sg2;
                                bestVal = val;
                            }
                        }
                        if (bestIdx >= 0)
                        {
                            SI_TG_ASSERT(makePair(state, sg1, bestIdx) == bestVal);
                            valueChange += bestVal;
                        }
                    }
                    if (valueChange == 0)
                        break;
                }
            };

            MergeState &baseState = c_state.mergeState;
            resetMergeState(baseState);
            runSinglePass(baseState);

            /*
            srand(6000);
            for (int n = 0; n < 5; n++)
            {
                logger::debug("compile", "  epoch %i: value %i", n, baseState.totalValue);
                MergeState bestState;
                resetMergeState(bestState);
                for (int k = 0; k < 5; k++)
                {
                    MergeState state = baseState;
                    // make random change
                    for (int i = 0; i < 100; i++)
                    {
                        int sg1 = subgroupsToMerge[rand() % subgroupsToMerge.size()];
                        int sg2 = subgroupsToMerge[rand() % subgroupsToMerge.size()];
                        if (sg1 == sg2)
                            continue;
                        makePair(state, sg1, sg2);
                    }
                    runSinglePass(state);
                    logger::debug("compile", "    attempt %i: value %i", k, state.totalValue);
                    if (state.totalValue > bestState.totalValue && state.totalValue > baseState.totalValue)
                        bestState = std::move(state);
                }
                if (bestState.totalValue > baseState.totalValue)
                    baseState = std::move(bestState);
            }*/

            // merge pairs
            int cntBeforeMerge = group.subgroupCnt;
            for (uint32_t sg1 = 0; sg1 < group.subgroups.size(); sg1++)
            {
                if (group.subgroups[sg1].tasks.empty())
                    continue;
                if (baseState.sgPair[sg1] >= 0)
                    mergeSubgroups(sg1, baseState.sgPair[sg1]);
                group.subgroupExclusionGraph.iterEdges(sg1, [&] (uint32_t sg2) { SI_TG_ASSERT(!group.subgroups[sg2].tasks.empty()); });
            }
            logger::debug("compile", "  merged %i/%i subgroups, value: %i, remaining %i", baseState.pairCnt * 2, cntBeforeMerge, baseState.totalValue, group.subgroupCnt);
        }
    }

    // merge small groups
    while (true)
    {
        int prevCandidateIdx = -1;
        bool anyMerged = false;
        for (int i = 0; i < c_state.groups.size(); i++)
        {
            if (prevCandidateIdx >= 0 && c_state.groups[i].subgroupCnt + c_state.groups[prevCandidateIdx].subgroupCnt <= 32)
            {
                GroupData &dst = c_state.groups[prevCandidateIdx];
                GroupData &src = c_state.groups[i];
                dst.subgroupExclusionGraph.resize(dst.subgroupExclusionGraph.size() + src.subgroupExclusionGraph.size());
                for (int sg1 = 0; sg1 < src.subgroups.size(); sg1++)
                {
                    if (src.subgroups[sg1].tasks.empty())
                        continue;
                    src.subgroupExclusionGraph.iterEdges(sg1, [&] (uint32_t sg2) {
                        dst.subgroupExclusionGraph.setConnectedBoth(sg1 + dst.subgroups.size(), sg2 + dst.subgroups.size(), true);
                    });
                }
                for (SubgroupData &sg : src.subgroups)
                    dst.subgroups.push_back(std::move(sg));
                dst.subgroupCnt += src.subgroupCnt;
                src.subgroupCnt = 0;
                src.subgroups.clear();
                anyMerged = true;
            }
            else if ((prevCandidateIdx < 0 || c_state.groups[i].subgroupCnt < c_state.groups[prevCandidateIdx].subgroupCnt) && c_state.groups[i].subgroupCnt < 32)
                prevCandidateIdx = i;
        }
        c_state.groups.erase(std::remove_if(c_state.groups.begin(), c_state.groups.end(), [&] (auto &g) { return g.subgroups.empty(); }), c_state.groups.end());
        if (!anyMerged)
            break;
    }

    // cleanup subgroups & init masks
    for (GroupData &group : c_state.groups)
    {
        // init actual index
        int nextSubgroupIdx = 0;
        for (SubgroupData &subgroup : group.subgroups)
            if (!subgroup.tasks.empty())
                subgroup.subgroupIdx = nextSubgroupIdx++;
        // init mask
        for (uint32_t i = 0; i < group.subgroups.size(); i++)
        {
            SubgroupData &subgroup = group.subgroups[i];
            if (subgroup.tasks.empty())
                continue;
            subgroup.mask = uint64_t(1) << uint64_t(subgroup.subgroupIdx);
            group.subgroupExclusionGraph.iterEdges(i, [&] (uint32_t v) {
                SI_TG_ASSERT(!group.subgroups[v].tasks.empty());
                uint64_t idx = group.subgroups[v].subgroupIdx;
                subgroup.mask |= uint64_t(1) << idx;
            });
        }
        // cleanup
        group.subgroups.erase(std::remove_if(group.subgroups.begin(), group.subgroups.end(), [&] (auto &sg) { return sg.tasks.empty(); }), group.subgroups.end());
        for (uint32_t i = 0; i < group.subgroups.size(); i++)
            SI_TG_ASSERT(i == group.subgroups[i].subgroupIdx);
    }

    // write to compiled
    c_state.allTasks.resize(prebuilt.taskData.size());
    for (int i = 0; i < prebuilt.taskData.size(); i++)
    {
        c_state.allTasks[i].isPendingOnStart = prebuilt.taskGraph.getFlag(i, 1);
        c_state.allTasks[i].allowToRunInParallelWithItself = !prebuilt.exclusionGraph.isConnected(i, i);
        prebuilt.taskGraph.iterEdges(i, [&] (uint32_t v) {
            c_state.allTasks[i].nextTasks.push_back(v);
            c_state.allTasks[v].depsCnt++;
        });
    }
    for (TaskData &task : c_state.allTasks)
    {
        if (task.depsCnt == 0)
            task.isPendingOnStart = true;
        if (task.depsCnt == 1)
            task.depsCnt = 0;
    }

    compiled.allGroups.resize(c_state.groups.size());
    for (int groupId = 0; groupId < c_state.groups.size(); groupId++)
    {
        GroupData &group = c_state.groups[groupId];
        uint64_t &initialPendingSubgroups = compiled.allGroups[groupId].initialPending;

        compiled.allGroups[groupId].subGroupsStart = compiled.allSubGroups.size();
        for (SubgroupData &subgroup : group.subgroups)
        {
            const int globalSubgroupId = compiled.allSubGroups.size();
            compiled.allSubGroups.emplace_back();
            compiled.allSubGroups.back().excludedMask = subgroup.mask;
            SI_TG_ASSERT(globalSubgroupId == subgroup.subgroupIdx + compiled.allGroups[groupId].subGroupsStart);

            compiled.allSubGroups.back().tasksStart = compiled.allTasks.size();
            for (uint32_t taskId : subgroup.tasks)
            {
                c_state.allTasks[taskId].remapTaskId = compiled.allTasks.size();
                CompiledTaskGraph::Task &task = compiled.allTasks.emplace_back();
                task.groupId = groupId;
                task.subGroupId = globalSubgroupId;
                task.isPendingOnStart = c_state.allTasks[taskId].isPendingOnStart;
                task.allowToRunInParallelWithItself = c_state.allTasks[taskId].allowToRunInParallelWithItself;
                task.taskData = prebuilt.taskData[taskId];
                task.dependencies.store(uint64_t(c_state.allTasks[taskId].depsCnt) << uint64_t(32u), std::memory_order_relaxed);
                if (task.isPendingOnStart)
                    initialPendingSubgroups |= uint64_t(1) << uint64_t(subgroup.subgroupIdx);
            }
            compiled.allSubGroups.back().tasksEnd = compiled.allTasks.size();
        }
        compiled.allGroups[groupId].subGroupsEnd = compiled.allSubGroups.size();
    }
    compiled.allGroupsState.resize(compiled.allGroups.size());
    compiled.allSubGroupsState.resize(compiled.allSubGroups.size());

    for (TaskData &task : c_state.allTasks)
    {
        SI_TG_ASSERT(task.remapTaskId < c_state.allTasks.size());
        compiled.allTasks[task.remapTaskId].nextTasksStart = compiled.nextTaskIds.size();
        for (uint32_t nextId: task.nextTasks)
            compiled.nextTaskIds.push_back(c_state.allTasks[nextId].remapTaskId);
        compiled.allTasks[task.remapTaskId].nextTasksEnd = compiled.nextTaskIds.size();
    }

    int subGraphDataReserve = 0;
    for (PrebuiltTaskGraph::SubGraphData &subGraphData : prebuilt.subGraphData)
        subGraphDataReserve += 4 + int(subGraphData.tasksEnd - subGraphData.tasksStart);
    compiled.subGraphData.reserve(subGraphDataReserve);
    for (PrebuiltTaskGraph::SubGraphData &subGraphData : prebuilt.subGraphData)
    {
        SI_TG_ASSERT(subGraphData.tasksEnd != subGraphData.tasksStart);
        const uint32_t entryTaskId = c_state.allTasks[subGraphData.entryTaskId].remapTaskId;
        const uint32_t exitTaskId = c_state.allTasks[subGraphData.exitTaskId].remapTaskId;
        compiled.allTasks[entryTaskId].subgraphDataIdx = compiled.subGraphData.size();
        compiled.allTasks[exitTaskId].subgraphDataIdx = compiled.subGraphData.size();
        SI_TG_ASSERT(!compiled.allTasks[entryTaskId].allowToRunInParallelWithItself);
        SI_TG_ASSERT(!compiled.allTasks[exitTaskId].allowToRunInParallelWithItself);
        compiled.subGraphData.push_back(subGraphData.tasksEnd - subGraphData.tasksStart);
        compiled.subGraphData.push_back(entryTaskId);
        compiled.subGraphData.push_back(exitTaskId);
        compiled.subGraphData.push_back(-1);
        for (uint32_t i = subGraphData.tasksStart; i < subGraphData.tasksEnd; i++)
            compiled.subGraphData.push_back(c_state.allTasks[prebuilt.subGraphTasks[i]].remapTaskId);
    }

    // debug
#if 0
    logger::debug("graph", "COMPILED GRAPH");
    for (int groupId = 0; groupId < compiled.allGroups.size(); groupId++)
    {
        CompiledTaskGraph::TaskGroup &group = compiled.allGroups[groupId];
        logger::debug("graph", "group #%i (%i)", groupId, int(group.subGroupsEnd - group.subGroupsStart));
        for (int subgroupId = group.subGroupsStart; subgroupId < group.subGroupsEnd; subgroupId++)
        {
            CompiledTaskGraph::TaskSubGroup &subgroup = compiled.allSubGroups[subgroupId];
            logger::debug_inline("graph", "  subgroup %02i (%3i) [", subgroupId - group.subGroupsStart, subgroup.tasksEnd - subgroup.tasksStart);
            for (int i = 0; i < 64; i++)
                logger::debug_inline("graph", "%i", (subgroup.excludedMask >> uint64_t(i)) & uint64_t(1));
            logger::debug_inline("graph", "]");
            for (uint32_t taskId = subgroup.tasksStart; taskId < subgroup.tasksEnd; taskId++)
            {
                CompiledTaskGraph::Task &task = compiled.allTasks[taskId];
                logger::debug_inline("graph", "%i{", int(taskId));
                if (task.subgraphDataIdx != -1)
                    logger::debug_inline("graph", "sg:%i|%i ", compiled.subGraphData[task.subgraphDataIdx + 1], compiled.subGraphData[task.subgraphDataIdx + 2]);
                int deps = int(task.dependencies >> 32u);
                if (deps == 0 && !task.isPendingOnStart)
                    deps = 1;
                logger::debug_inline("graph", "d:%i", deps);
                if (task.nextTasksStart != task.nextTasksEnd)
                {
                    logger::debug_inline("graph", " next:");
                    for (int i = task.nextTasksStart; i < task.nextTasksEnd; i++)
                        logger::debug_inline("graph", " %i", compiled.nextTaskIds[i]);
                }
                logger::debug_inline("graph", "} ", int(taskId), deps);
            }
            logger::debug_inline("graph", "\n");
        }
    }

    if (!compiled.subGraphData.empty())
        logger::debug("graph", "SUB-GRAPHS");
    for (int idx = 0; idx < compiled.subGraphData.size(); )
    {
        const int32_t id = idx;
        int32_t cnt = compiled.subGraphData[idx++];
        int32_t subgraphEntryTask = compiled.subGraphData[idx++];
        int32_t subgraphExitTask = compiled.subGraphData[idx++];
        idx++;
        logger::debug_inline("graph", "  subgraph #%i (%i) [%i|%i]: ", id, cnt, subgraphEntryTask, subgraphExitTask);
        for (uint32_t i = 0; i < cnt; i++)
            logger::debug_inline("graph", " %i", compiled.subGraphData[idx++]);
        logger::debug_inline("graph", "\n");
    }
#endif
    return true;
}



}