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

void TaskGraph::setNext(uint32_t task, uint32_t next)
{
    allNodes[task].addNext(next);
}

void TaskGraph::addResource(uint32_t task, uint64_t resId, bool write)
{
    allNodes[task].resources.push_back(ResourceRef{resId, uint64_t(write)});
}

bool TaskGraph::validateAndNormalize()
{
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
        for (uint32_t i2 = i1 + 1; i2 < allNodes.size(); i2++)
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
    for (uint32_t v = 0; v < allNodes.size(); v++)
    {
        base_graph_dfs(taskGraph, v, 0, [&] (uint32_t vv, auto dfs_next) {
            // logger::debug_inline("graph", " %i", vv);
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

    // normalize order
    taskGraph.setAllFlags(0, false);
    for (uint32_t v = 0; v < allNodes.size(); v++)
    {
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
        auto mergeSubgroups = [&] (uint64_t dst, uint64_t src)
        {
            for (uint32_t t : group.subgroups[src].tasks)
                group.subgroups[dst].tasks.push_back(t);
            group.subgroups[src].tasks.clear();
            group.subgroupExclusionGraph.iterEdges(src, [&] (uint32_t v) {
                group.subgroupExclusionGraph.setConnectedBoth(src, v, false);
                group.subgroupExclusionGraph.setConnectedBoth(dst, v, true);
            });
        };
        bool force = true;
        int minAllowedCost = 0;
        while (true)
        {
            bool anyMergedTrivially = false;
            int32_t minParallelCost = INT32_MAX;
            std::pair<uint32_t, uint32_t> maxPair = {0, 0};
            int subgroupCnt = 0;
            for (uint32_t i1 = 0; i1 < group.subgroups.size(); i1++)
            {
                if (group.subgroups[i1].tasks.empty())
                    continue;
                subgroupCnt++;
                for (uint32_t i2 = i1 + 1; i2 < group.subgroups.size(); i2++)
                {
                    if (group.subgroups[i2].tasks.empty())
                        continue;
                    if (!force && !group.subgroupExclusionGraph.isConnected(i1,i2)) // skip not connected (this includes empty)
                        continue;

                    std::span<uint64_t> edges1 = group.subgroupExclusionGraph.getEdges(i1);
                    std::span<uint64_t> edges2 = group.subgroupExclusionGraph.getEdges(i2);
                    int lockedTaskCnt1 = 0;
                    int lockedTaskCnt2 = 0;
                    int lockedTaskCntU = 0;
                    int lockingConstChange;
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
                    int lockingCost1 = lockedTaskCnt1 * group.subgroups[i1].tasks.size();
                    int lockingCost2 = lockedTaskCnt2 * group.subgroups[i2].tasks.size();
                    int lockingCostU = lockedTaskCntU * (group.subgroups[i1].tasks.size() + group.subgroups[i2].tasks.size());
                    lockingConstChange = lockingCostU - (lockingCost1 + lockingCost2);
                    assert(lockingConstChange >= 0);
                    if (lockingConstChange <= minAllowedCost)
                    {
                        if (lockingConstChange == 0)
                            logger::debug("graph-build", "merged trivially %i <- %i", i1, i2);
                        else
                            logger::debug("graph-build", "merged by threshold %i <- %i cost=%i", i1, i2, lockingConstChange);

                        mergeSubgroups(i1, i2);
                        anyMergedTrivially = true;
                        continue;
                    }
                    if (lockingConstChange < minParallelCost)
                    {
                        maxPair = {i1, i2};
                        minParallelCost = lockingConstChange;
                    }
                }
            }

            group.subgroupCnt = subgroupCnt;
            if (anyMergedTrivially)
                continue;
            if (subgroupCnt <= 64)
                break;
            minAllowedCost = minParallelCost;
            if (minParallelCost < INT32_MAX)
            {
                logger::debug("graph-build", "merged by min cost %i <- %i cost=%i", maxPair.first, maxPair.second, int(minParallelCost));
                mergeSubgroups(maxPair.first, maxPair.second);
            }
            else
                force = true;
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
                uint64_t idx = group.subgroups[v].subgroupIdx;
                subgroup.mask |= uint64_t(1) << idx;
            });
        }
        // cleanup
        group.subgroups.erase(std::remove_if(group.subgroups.begin(), group.subgroups.end(), [&] (auto &sg) { return sg.tasks.empty(); }), group.subgroups.end());
        for (uint32_t i = 0; i < group.subgroups.size(); i++)
            assert(i == group.subgroups[i].subgroupIdx);
    }

    // debug
    for (GroupData &group : groups)
    {
        logger::debug("graph", "group (%i)", group.subgroupCnt);
        int subgroupIdx = 0;
        for (auto &subgroup : group.subgroups)
        {
            if (subgroup.tasks.empty())
                continue;
            logger::debug_inline("graph", "  subgroup %02i [", subgroupIdx);
            for (int i = 0; i < 64; i++)
                logger::debug_inline("graph", "%i", (subgroup.mask >> uint64_t(i)) & 1);
            logger::debug_inline("graph", "]");
            for (uint32_t task : subgroup.tasks)
                logger::debug_inline("graph", " %i", int(task));
            logger::debug_inline("graph", "\n");
            subgroupIdx++;
        }
    }

    return true;
}


void TaskGraph::buildFibers()
{
    entryFiberIds.clear();
    allFibers.clear();
    for (uint32_t i = 0; i < allNodes.size(); i++)
        allNodes[i].fiberId = INVALID_ID;

    for (uint32_t i = 0; i < allNodes.size(); i++)
    {
        if (allNodes[i].fiberId != INVALID_ID)
            continue;
        // find base
        uint32_t base = i;
        while (true)
        {
            if (allNodes[base].prevTasks.size() != 1)
                break;
            uint32_t prev = allNodes[base].prevTasks.front();
            if (allNodes[prev].fiberId != INVALID_ID)
                break;
            base = prev;
        }
        Fiber fiber;
        fiber.dependencies = allNodes[base].prevTasks;
        uint32_t fiberId = allFibers.size();

        // find end and assign fiber
        uint32_t end = base;
        while (true)
        {
            allNodes[end].fiberId = fiberId;
            fiber.tasks.push_back(end);
            uint32_t next = INVALID_ID;
            for (uint32_t id : allNodes[end].nextTasks)
                if (allNodes[id].fiberId == INVALID_ID)
                    next = id;
            if (next == INVALID_ID)
                break;
            end = next;
        }

        if (fiber.dependencies.empty())
            entryFiberIds.push_back(fiberId);
        allFibers.push_back(std::move(fiber));
    }
}

template<typename U, typename F>
void TaskGraph::traverseNodeSequence(std::vector<U>& stack, std::vector<uint32_t>& visited, uint32_t node, F&& f)
{
    if (std::find(visited.begin(), visited.end(), node) != visited.end())
        return;
    stack.push_back(f(node));
    visited.push_back(node);
    auto tasks = allNodes[node].nextTasks;
    for (uint32_t next : tasks)
        traverseNodeSequence(stack, visited, next, f);
    stack.pop_back();
}

void TaskGraph::dumpToLog()
{
    std::vector<std::stringstream> rows;
    rows.resize(allFibers.size());

    struct FiberPrintData
    {
        uint32_t id = INVALID_ID;
        int pos;
        std::vector<uint32_t> deps;
        bool hadDeps;
    };
    std::vector<FiberPrintData> fibersToPrint;

    int baseStartPos = 0;
    for (uint32_t id = 0; id < allFibers.size(); id++)
    {
        fibersToPrint.push_back(FiberPrintData{id, 0, allFibers[id].dependencies, !allFibers[id].dependencies.empty()});
        rows[id] << "[ ";
        for (uint32_t t : allFibers[id].dependencies)
            rows[id] << t << " ";
        rows[id] << "] ";
        baseStartPos = std::max<int>(baseStartPos, rows[id].view().size());
    }
    for (FiberPrintData &f : fibersToPrint)
        f.pos = baseStartPos + 1;

    auto addSpace = [&] (uint32_t idx, int required_len)
    {
        auto &ss = rows[idx];
        while (ss.view().size() < required_len)
            ss << ' ';
    };

    while (true)
    {
        FiberPrintData fiber;
        for (auto it = fibersToPrint.begin(); it != fibersToPrint.end(); it++)
        {
            if (it->deps.empty())
            {
                fiber = std::move(*it);
                fibersToPrint.erase(it);
                break;
            }
        }
        if (fiber.id == INVALID_ID)
            break;
        addSpace(fiber.id, fiber.pos);

        bool first = true;
        for (uint32_t taskId : allFibers[fiber.id].tasks)
        {
            if (first && fiber.hadDeps)
                rows[fiber.id] << "|---> ";
            else if (!first)
                rows[fiber.id] << " --> ";
            first = false;
            rows[fiber.id] << taskId;

            for (FiberPrintData &f : fibersToPrint)
            {
                while (true)
                {
                    if (auto it = std::find(f.deps.begin(), f.deps.end(), taskId); it != f.deps.end())
                    {
                        f.deps.erase(it);
                        f.pos = std::max<int>(f.pos, rows[fiber.id].view().size() - 1);
                    }
                    else
                        break;
                }
            }
        }
    }

    for (auto &ss : rows)
    {
        auto s = ss.str();
        logger::debug("graph", "%s", s.c_str());
    }
    logger::flush_default_log();
}

bool TaskGraph::compileTo(sie::CompiledTaskGraph& graph)
{
    /*
    if (!validateAndNormalize())
        return false;
    buildFibers();

    graph.allTasks.clear();
    graph.allTasks.resize(allNodes.size());
    for (uint32_t taskId = 0; taskId < allNodes.size(); taskId++)
    {
        graph.allTasks[taskId].task = allNodes[taskId].fn;
        graph.allTasks[taskId].queueId = allNodes[taskId].fiberId;
    }

    graph.queueNodes.clear();
    graph.queueNodes.resize(allFibers.size());
    for (uint32_t fiberId = 0; fiberId < allFibers.size(); fiberId++)
    {
        graph.queueNodes[fiberId].taskQueue = allFibers[fiberId].tasks;
        graph.queueNodes[fiberId].setRequirementsCount(allFibers[fiberId].dependencies.size());
        for (uint32_t depId : allFibers[fiberId].tasks)
            graph.allTasks[depId].nextQueues.push_back(fiberId);
    }
    */

    return true;
}

}