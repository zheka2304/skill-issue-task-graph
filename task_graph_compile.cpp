#include "task_graph.h"

#include <cassert>
#include <sstream>
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
                    const auto rr2 = n1.resources[r2];
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
                group.subgroupExclusionGraph.resize(group.subgroups.size());
                group.subgroupExclusionGraph.setFlagCount(8);
                for (uint32_t i1 = 0; i1 < group.subgroups.size(); i1++)
                    for (uint32_t i2 = i1 + 1; i2 < group.subgroups.size(); i2++)
                    {
                        uint32_t t1 = group.subgroups[i1].tasks.back();
                        uint32_t t2 = group.subgroups[i2].tasks.back();
                        group.subgroupExclusionGraph.setConnectedBoth(i1, i2, exclusionGraph.isConnected(t1, t2));
                    }
                groups.push_back(std::move(group));
            }
        }
    }

    // merge subgroups
    for (GroupData &group : groups)
    {
        logger::debug("compile", "merging subgroups");
        group.subgroupCnt = group.subgroups.size();

        auto calcLockingConst = [&] (uint32_t sg1, uint32_t sg2) -> std::pair<int, int>
        {
            std::span<uint64_t> edges1 = group.subgroupExclusionGraph.getEdges(sg1);
            std::span<uint64_t> edges2 = group.subgroupExclusionGraph.getEdges(sg2);
            assert(edges1.size() == edges2.size());
            int lockedTaskCnt1 = 0;
            int lockedTaskCnt2 = 0;
            int lockedTaskCntU = 0;
            if constexpr (false)
            {
                iter_set_bits_span_var([&] (uint32_t v, bool e1, bool e2){
                    if (e1)
                        lockedTaskCnt1 += group.subgroups[v].tasks.size();
                    if (e2)
                        lockedTaskCnt2 += group.subgroups[v].tasks.size();
                    lockedTaskCntU += group.subgroups[v].tasks.size();
                }, edges1, edges2);
            }
            else
            {
                for (int i = 0; i < edges1.size(); i++)
                {
                    lockedTaskCnt1 += count_set_bits(edges1[i]);
                    lockedTaskCnt2 += count_set_bits(edges2[i]);
                    lockedTaskCntU += count_set_bits(edges1[i] | edges2[i]);
                }
            }
            int lockingCost1 = lockedTaskCnt1 * group.subgroups[sg1].tasks.size();
            int lockingCost2 = lockedTaskCnt2 * group.subgroups[sg2].tasks.size();
            int lockingCostU = lockedTaskCntU * (group.subgroups[sg1].tasks.size() + group.subgroups[sg2].tasks.size());
            return {lockingCost1 + lockingCost2, lockingCostU};
        };

        logger::debug("compile", "  initial cost calc");
        group.subgroupsLockingCost.resize(group.subgroups.size() * group.subgroups.size());
        for (uint32_t i1 = 0; i1 < group.subgroups.size(); i1++)
            for (uint32_t i2 = i1 + 1; i2 < group.subgroups.size(); i2++)
                group.lockingCost(i1, i2) = calcLockingConst(i1, i2);
        auto mergeSubgroups = [&] (uint64_t dst, uint64_t src)
        {
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
                // int cnt = group.subgroupExclusionGraph.countEdges(v);
                group.subgroupExclusionGraph.setConnectedBoth(src, v, false);
                group.subgroupExclusionGraph.setConnectedBoth(dst, v, true);
            });
            group.subgroupExclusionGraph.iterEdges(dst, [&] (uint32_t v) {
                // logger::debug("", "  updated cost %i", v);
                group.lockingCost(dst, v) = calcLockingConst(dst, v);
                group.subgroupExclusionGraph.iterEdges(v, [&] (uint32_t vv) {
                    group.lockingCost(v, vv) = calcLockingConst(v, vv);
                });
            });
        };

        bool force = false;
        int32_t mergeThreshold = 0;
        while (true)
        {
            if (false)
            {
                logger::debug("", "iteration");
                for (uint32_t i1 = 0; i1 < group.subgroups.size(); i1++)
                    for (uint32_t i2 = i1 + 1; i2 < group.subgroups.size(); i2++)
                        if (group.subgroupExclusionGraph.isConnected(i1, i2) && !group.subgroups[i1].tasks.empty() && !group.subgroups[i2].tasks.empty())
                            if (group.lockingCost(i1, i2) != calcLockingConst(i1, i2))
                            {
                                auto c1 = group.lockingCost(i1, i2);
                                auto c2 = calcLockingConst(i1, i2);
                                logger::debug("", "assert failed %i-%i (%i,%i/%i,%i)", i1, i2, c1.first, c1.second, c2.first, c2.second);
                                logger::flush_default_log();
                                assert(0);
                            }
            }

            bool anyMergedTrivially = false;
            int32_t minMergeCost = INT_MAX;
            std::pair<int32_t, int32_t> minMergePair = {-1, -1};
            for (uint32_t i1 = 0; i1 < group.subgroups.size(); i1++)
            {
                if (group.subgroups[i1].tasks.empty())
                {
                    group.subgroupExclusionGraph.iterEdges(i1, [&] (uint32_t) { assert(0); });
                    continue;
                }
                for (uint32_t i2 = i1 + 1; i2 < group.subgroups.size(); i2++)
                {
                    if (group.subgroupCnt <= 64 && !anyMergedTrivially)
                        break;
                    if (group.subgroups[i2].tasks.empty())
                        continue;
                    if (!group.subgroupExclusionGraph.isConnected(i1,i2)) // skip not connected
                        continue;
                    auto [lockingCostSum, lockingCostUni] = group.lockingCost(i1, i2);
                    int lockingCostChange = lockingCostUni - lockingCostSum;
                    assert(lockingCostChange >= 0);
                    if (lockingCostChange < minMergeCost)
                    {
                        if (lockingCostChange > mergeThreshold)
                            minMergePair = {i1, i2};
                        minMergeCost = lockingCostChange;
                    }
                    if (lockingCostChange <= mergeThreshold)
                    {
                        mergeSubgroups(i1, i2);
                        if (i1 == minMergePair.first || i1 == minMergePair.second || i2 == minMergePair.first || i2 == minMergePair.second)
                            minMergePair = {-1, -1};
                        if (lockingCostChange == 0)
                        {
                            anyMergedTrivially = true;
                            mergeThreshold = 0;
                        }
                        continue;
                    }
                }
            }

            logger::debug("compile", "  remaining %i", group.subgroupCnt);
            if (anyMergedTrivially)
                continue;
            if (group.subgroupCnt <= 64)
                break;
            if (minMergePair.first >= 0)
            {
                mergeThreshold = minMergeCost;
                // logger::debug("compile", "merged by min cost %i <- %i cost=%i", maxPair.first, maxPair.second, int(minParallelCost));
                mergeSubgroups(minMergePair.first, minMergePair.second);
            }
        }
    }

    // cleanup subgroups & init masks
    for (GroupData &group : groups)
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
            logger::debug_inline("graph", "  subgroup %02i [", subgroupId);
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