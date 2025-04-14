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


bool prebuild_task_graph(PrebuiltTaskGraph &prebuilt, const TaskGraph &graph)
{
    logger::debug("compile", "building graph");
    // directed graph
    prebuilt.taskGraph.copyFrom(graph.taskOrderGraph);
    prebuilt.taskGraph.setFlagCount(2);

    // all tasks, that can run in parallel (not sequenced), but aren't allowed because of resource usage
    prebuilt.exclusionGraph.resize(prebuilt.taskGraph.size());
    prebuilt.exclusionGraph.setFlagCount(2);
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
                    prebuilt.exclusionGraph.setConnectedBoth(it1->second.task, it2->second.task, true);
            }
    }

    // validate no cycles
    bool isValid = true;
    prebuilt.taskGraph.setAllFlags(0, false);
    prebuilt.taskGraph.setAllFlags(1, true);
    for (uint32_t v = 0; v < prebuilt.taskGraph.size(); v++)
    {
        base_graph_dfs(prebuilt.taskGraph, v, 0, [&] (uint32_t vv, auto dfs_next) {
            if (v != vv)
                prebuilt.taskGraph.setFlag(vv, 1, false);
            if (std::find(prebuilt.cycleDetectStack.begin(), prebuilt.cycleDetectStack.end(), vv) != prebuilt.cycleDetectStack.end())
            {
                error("graph", "node cycle detected! %i", v);
                isValid = false;
            }
            prebuilt.cycleDetectStack.push_back(vv);
            dfs_next();
            prebuilt.cycleDetectStack.pop_back();
        });
    }
    if (!isValid)
        return false;

    logger::debug("compile", "normalizing graph");
    // normalize order
    for (uint32_t v = 0; v < prebuilt.taskGraph.size(); v++)
    {
        prebuilt.taskGraph.setAllFlags(0, false);
        base_graph_dfs(prebuilt.taskGraph, v, 0, [&] (uint32_t vv, auto dfs_next) {
            if (prebuilt.cycleDetectStack.size() > 1)
                prebuilt.taskGraph.setConnected(v, vv, false);
            prebuilt.exclusionGraph.setConnectedBoth(v, vv, false);
            prebuilt.cycleDetectStack.push_back(vv);
            dfs_next();
            prebuilt.cycleDetectStack.pop_back();
        });
    }

    prebuilt.taskData = graph.taskData;

    return true;
}


bool compile_task_graph(CompiledTaskGraph &compiled, PrebuiltTaskGraph &prebuilt, strategy::MergeSubgroupsState &c_state)
{
    using GroupData = strategy::MergeSubgroupsState::GroupData;
    using SubgroupData = strategy::MergeSubgroupsState::SubgroupData;
    using TaskData = strategy::MergeSubgroupsState::TaskData;
    using MergeState = strategy::MergeSubgroupsState::MergeState;

    // build groups
    logger::debug("compile", "building groups & subgroups");

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
                group.subgroupExclusionGraph.setFlagCount(8);
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
            assert(src != dst);
            assert(!group.subgroups[src].tasks.empty());
            assert(!group.subgroups[dst].tasks.empty());
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
            assert(sg1 != sg2);
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
                assert(state.sgPair[sg2] == sg1);
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
                            assert(makePair(state, sg1, bestIdx) == bestVal);
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
                group.subgroupExclusionGraph.iterEdges(sg1, [&] (uint32_t sg2) { assert(!group.subgroups[sg2].tasks.empty()); });
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
                assert(!group.subgroups[v].tasks.empty());
                uint64_t idx = group.subgroups[v].subgroupIdx;
                subgroup.mask |= uint64_t(1) << idx;
            });
        }
        // cleanup
        group.subgroups.erase(std::remove_if(group.subgroups.begin(), group.subgroups.end(), [&] (auto &sg) { return sg.tasks.empty(); }), group.subgroups.end());
        for (uint32_t i = 0; i < group.subgroups.size(); i++)
            assert(i == group.subgroups[i].subgroupIdx);
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
            assert(globalSubgroupId == subgroup.subgroupIdx + compiled.allGroups[groupId].subGroupsStart);

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
        assert(task.remapTaskId < c_state.allTasks.size());
        for (uint32_t nextId: task.nextTasks)
            compiled.allTasks[task.remapTaskId].nextTasks.push_back(c_state.allTasks[nextId].remapTaskId);
    }

    // debug
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
                int deps = int(task.dependencies >> 32u);
                if (deps == 0 && !task.isPendingOnStart)
                    deps = 1;
                logger::debug_inline("graph", " %i{d:%i", int(taskId), deps);
                if (!task.nextTasks.empty())
                {
                    logger::debug_inline("graph", " next:");
                    for (uint32_t nextId : task.nextTasks)
                        logger::debug_inline("graph", " %i", nextId);
                }
                logger::debug_inline("graph", "} ", int(taskId), deps);
            }
            logger::debug_inline("graph", "\n");
        }
    }

    return true;
}



}