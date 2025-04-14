#include "task_graph.h"

#include <cassert>
#include <sstream>
#include <unordered_map>
#include "logger.h"
#include "task_graph_execute.h"

namespace sie
{

using logger::error;

// TaskNode

void TaskGraph::TaskNode::addNext(uint32_t id)
{
    if (std::find(nextTasks.begin(), nextTasks.end(), id) == nextTasks.end())
        nextTasks.push_back(id);
}

void TaskGraph::TaskNode::removeNext(uint32_t id)
{
    if (auto it = std::find(nextTasks.begin(), nextTasks.end(), id); it != nextTasks.end())
        nextTasks.erase(it);
}


// TaskGraph

uint32_t TaskGraph::addTask()
{
    uint32_t id = allNodes.size();
    allNodes.emplace_back();
    return id;
}

void TaskGraph::setTaskData(uint32_t task, sie::TaskFnPtr fn, sie::VarTaskFnPtr var_fn, void* data)
{
    allNodes[task].fn = fn;
    allNodes[task].varFn = var_fn;
    allNodes[task].userData = data;
}

void TaskGraph::setNext(uint32_t task, uint32_t next)
{
    allNodes[task].addNext(next);
}

void TaskGraph::addResource(uint32_t task, uint64_t resId, bool write)
{
    allNodes[task].resources.push_back(ResourceRef{resId, uint64_t(write)});
}

bool TaskGraph::validateAndNormalize(CompiledTaskGraph &compiled)
{
    logger::debug("compile", "building graph");
    // directed graph
    BaseGraph taskGraph;
    taskGraph.resize(allNodes.size());
    taskGraph.setFlagCount(8);
    for (uint32_t nodeId = 0; nodeId < allNodes.size(); nodeId++)
    {
        for (uint32_t nextNodeId : allNodes[nodeId].nextTasks)
            taskGraph.setConnected(nodeId, nextNodeId, true);
    }

    // all tasks, that can run in parallel (not sequenced), but aren't allowed because of resource usage
    BaseGraph exclusionGraph;
    exclusionGraph.resize(allNodes.size());
    exclusionGraph.setFlagCount(8);
    for (uint32_t i1 = 0; i1 < allNodes.size(); i1++)
    {
        for (uint32_t i2 = i1; i2 < allNodes.size(); i2++)
        {
            TaskNode& n1 = allNodes[i1];
            TaskNode& n2 = allNodes[i2];
            bool excluded = false;
            for (uint32_t r1 = 0; r1 < n1.resources.size(); r1++)
                for (uint32_t r2 = r1 + 1; r2 < n2.resources.size(); r2++)
                {
                    const auto rr1 = n1.resources[r1];
                    const auto rr2 = n2.resources[r2];
                    excluded |= (rr1.id == rr2.id && (rr1.write || rr2.write));
                }
            exclusionGraph.setConnectedBoth(i1, i2, excluded);
        }
    }

    // validate no cycles
    bool isValid = true;
    std::vector<uint32_t> stack;
    taskGraph.setAllFlags(0, false);
    taskGraph.setAllFlags(1, true);
    for (uint32_t v = 0; v < allNodes.size(); v++)
    {
        base_graph_dfs(taskGraph, v, 0, [&] (uint32_t vv, auto dfs_next) {
            if (v != vv)
                taskGraph.setFlag(vv, 1, false);
            if (std::find(stack.begin(), stack.end(), vv) != stack.end())
            {
                error("graph", "node cycle detected! %i", v);
                assert(0);
                isValid = false;
            }
            stack.push_back(vv);
            dfs_next();
            stack.pop_back();
        });
    }
    if (!isValid)
        return false;

    logger::debug("compile", "normalizing graph");
    // normalize order
    for (uint32_t v = 0; v < allNodes.size(); v++)
    {
        taskGraph.setAllFlags(0, false);
        base_graph_dfs(taskGraph, v, 0, [&] (uint32_t vv, auto dfs_next) {
            if (stack.size() > 1)
                taskGraph.setConnected(v, vv, false);
            exclusionGraph.setConnectedBoth(v, vv, false);
            stack.push_back(vv);
            dfs_next();
            stack.pop_back();
        });
    }

    // build groups
    logger::debug("compile", "building groups & subgroups");
    struct SubgroupData
    {
        uint64_t mask;
        int subgroupIdx;
        std::vector<uint32_t> tasks;
        bool isFake = false;
    };
    struct GroupData
    {
        int subgroupCnt;
        BaseGraph subgroupExclusionGraph;
        std::vector<SubgroupData> subgroups;
        std::vector<std::pair<int, int>> subgroupsLockingCost;

        auto &lockingCost(uint32_t sg1, uint32_t sg2)
        {
            if (sg1 > sg2) std::swap(sg1, sg2);
            return subgroupsLockingCost[sg1 * subgroups.size() + sg2];
        }
    };
    std::vector<GroupData> groups;
    {
        GroupData group;
        exclusionGraph.setAllFlags(0, false);
        for (uint32_t v = 0; v < allNodes.size(); v++)
        {
            group.subgroups.clear();
            group.subgroupExclusionGraph.resize(0);
            base_graph_dfs(exclusionGraph, v, 0, [&](uint32_t vv, auto dfs_next) {
                SubgroupData subgroup;
                subgroup.tasks.push_back(vv);
                group.subgroups.push_back(std::move(subgroup));
                dfs_next();
            });
            if (!group.subgroups.empty())
            {
                /*
                if (group.subgroups.size() > 64)
                {
                    uint32_t desiredCnt = group.subgroups.size();
                    desiredCnt--;
                    desiredCnt |= desiredCnt >> 1;
                    desiredCnt |= desiredCnt >> 2;
                    desiredCnt |= desiredCnt >> 4;
                    desiredCnt |= desiredCnt >> 8;
                    desiredCnt |= desiredCnt >> 16;
                    desiredCnt++;
                    int i = 0;
                    while (group.subgroups.size() < desiredCnt)
                    {
                        SubgroupData sg = group.subgroups[i++];
                        sg.isFake = true;
                        group.subgroups.push_back(std::move(sg));
                    }
                }
                */
                group.subgroupExclusionGraph.resize(group.subgroups.size());
                group.subgroupExclusionGraph.setFlagCount(8);
                for (uint32_t i1 = 0; i1 < group.subgroups.size(); i1++)
                {
                    group.subgroupExclusionGraph.setConnectedBoth(i1, i1, true);
                    for (uint32_t i2 = i1 + 1; i2 < group.subgroups.size(); i2++)
                    {
                        uint32_t t1 = group.subgroups[i1].tasks.back();
                        uint32_t t2 = group.subgroups[i2].tasks.back();
                        group.subgroupExclusionGraph.setConnectedBoth(i1, i2, exclusionGraph.isConnected(t1, t2));
                    }
                }
                groups.push_back(std::move(group));
            }
        }
    }

    // merge subgroups
    for (GroupData &group : groups)
    {
        group.subgroupCnt = group.subgroups.size();
        struct MergeState
        {
            std::vector<int> sgPair;
            int totalValue;
            int pairCnt;
        };

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
            const std::span<uint64_t> edges1 = group.subgroupExclusionGraph.getEdges(sg1).getSpan();
            const std::span<uint64_t> edges2 = group.subgroupExclusionGraph.getEdges(sg2).getSpan();
            const bool isConnected = group.subgroupExclusionGraph.isConnected(sg1, sg2);
            int value = isConnected ? 1 : 0; // +1 edge eliminated
            group.subgroupExclusionGraph.setConnectedBoth(sg1, sg2, false);
            for (int i = 0; i < edges1.size(); i++)
                value += count_set_bits(edges1[i] & edges2[i]); // unified edges will be eliminated
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

            MergeState baseState;
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
        for (int i = 0; i < groups.size(); i++)
        {
            if (prevCandidateIdx >= 0 && groups[i].subgroupCnt + groups[prevCandidateIdx].subgroupCnt <= 32)
            {
                GroupData &dst = groups[prevCandidateIdx];
                GroupData &src = groups[i];
                dst.subgroupExclusionGraph.resize(dst.subgroupExclusionGraph.getVertexCount() + src.subgroupExclusionGraph.getVertexCount());
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
            else if ((prevCandidateIdx < 0 || groups[i].subgroupCnt < groups[prevCandidateIdx].subgroupCnt) && groups[i].subgroupCnt < 32)
                prevCandidateIdx = i;
        }
        groups.erase(std::remove_if(groups.begin(), groups.end(), [&] (auto &g) { return g.subgroups.empty(); }), groups.end());
        if (!anyMerged)
            break;
    }

    // cleanup subgroups & init masks
    for (GroupData &group : groups)
    {
        // init actual index
        int nextSubgroupIdx = 0;
        for (SubgroupData &subgroup : group.subgroups)
            if (!subgroup.tasks.empty() && !subgroup.isFake)
                subgroup.subgroupIdx = nextSubgroupIdx++;
        // init mask
        for (uint32_t i = 0; i < group.subgroups.size(); i++)
        {
            SubgroupData &subgroup = group.subgroups[i];
            if (subgroup.tasks.empty() || subgroup.isFake)
                continue;
            subgroup.mask = uint64_t(1) << uint64_t(subgroup.subgroupIdx);
            group.subgroupExclusionGraph.iterEdges(i, [&] (uint32_t v) {
                assert(!group.subgroups[v].tasks.empty());
                uint64_t idx = group.subgroups[v].subgroupIdx;
                subgroup.mask |= uint64_t(1) << idx;
            });
        }
        // cleanup
        group.subgroups.erase(std::remove_if(group.subgroups.begin(), group.subgroups.end(), [&] (auto &sg) { return sg.tasks.empty() || sg.isFake; }), group.subgroups.end());
        for (uint32_t i = 0; i < group.subgroups.size(); i++)
            assert(i == group.subgroups[i].subgroupIdx);
    }

    // write to compiled
    struct TaskData
    {
        uint32_t remapTaskId = ~uint32_t(0);
        int depsCnt = 0;
        bool isPendingOnStart = false;
        bool allowToRunInParallelWithItself = false;
        std::vector<uint32_t> nextTasks;
    };
    std::vector<TaskData> allTasks;
    allTasks.resize(allNodes.size());
    for (int i = 0; i < allNodes.size(); i++)
    {
        allTasks[i].isPendingOnStart = taskGraph.getFlag(i, 1);
        allTasks[i].allowToRunInParallelWithItself = !exclusionGraph.isConnected(i, i);
        taskGraph.iterEdges(i, [&] (uint32_t v) {
            allTasks[i].nextTasks.push_back(v);
            allTasks[v].depsCnt++;
        });
    }
    for (TaskData &task : allTasks)
    {
        if (task.depsCnt == 0)
            task.isPendingOnStart = true;
        if (task.depsCnt == 1)
            task.depsCnt = 0;
    }

    compiled.allGroups.resize(groups.size());
    for (int groupId = 0; groupId < groups.size(); groupId++)
    {
        GroupData &group = groups[groupId];
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
                allTasks[taskId].remapTaskId = compiled.allTasks.size();
                CompiledTaskGraph::Task &task = compiled.allTasks.emplace_back();
                task.groupId = groupId;
                task.subGroupId = globalSubgroupId;
                task.isPendingOnStart = allTasks[taskId].isPendingOnStart;
                task.allowToRunInParallelWithItself = allTasks[taskId].allowToRunInParallelWithItself;
                task.task = allNodes[taskId].fn;
                task.varTask = allNodes[taskId].varFn;
                task.taskUserData = allNodes[taskId].userData;
                task.dependencies.store(uint64_t(allTasks[taskId].depsCnt) << uint64_t(32u), std::memory_order_relaxed);
                if (task.isPendingOnStart)
                    initialPendingSubgroups |= uint64_t(1) << uint64_t(subgroup.subgroupIdx);
            }
            compiled.allSubGroups.back().tasksEnd = compiled.allTasks.size();
        }
        compiled.allGroups[groupId].subGroupsEnd = compiled.allSubGroups.size();
    }
    compiled.allGroupsState.resize(compiled.allGroups.size());
    compiled.allSubGroupsState.resize(compiled.allSubGroups.size());

    for (TaskData &task : allTasks)
    {
        assert(task.remapTaskId < allTasks.size());
        for (uint32_t nextId: task.nextTasks)
            compiled.allTasks[task.remapTaskId].nextTasks.push_back(allTasks[nextId].remapTaskId);
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