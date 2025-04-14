#include <cstring>

#include <taskgraph/task_graph_compile.h>
#include <taskgraph/task_graph_execute.h>


namespace si::tg
{

// BaseGraphEdgesRef

bool BaseGraphEdgesRef::operator==(const BaseGraphEdgesRef& rhs) const
{
    if (size() != rhs.size())
        return false;
    bool r = true;
    for (uint32_t i = 0; i < size(); i++)
        r &= data[i] == rhs.data[i];
    return r;
}

uint32_t BaseGraphEdgesRef::count() const
{
    uint32_t r = 0;
    for (uint64_t w : *this)
        r += internal::count_set_bits(w);
    return r;
}

size_t BaseGraphEdgesRef::hash() const
{
    size_t seed = size();
    for (uint64_t x : *this)
    {
        x = ((x >> 16) ^ x) * 0x45d9f3b;
        x = ((x >> 16) ^ x) * 0x45d9f3b;
        x = (x >> 16) ^ x;
        seed ^= x + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
}

// BaseGraph

void BaseGraph::resize(int32_t newSize)
{
    SI_TG_ASSERT(newSize >= 0);
    if (int32_t(vertexCnt) == newSize)
        return;
    const uint32_t oldWordCnt = uint32_t((vertexCnt + 63u) / 64u);
    const uint32_t newWordCnt = uint32_t((uint32_t(newSize) + 63u) / 64u);
    vertexFlags.resize(newSize * vertexFlagCount, 0);
    if (oldWordCnt == newWordCnt)
    {
        connections.resize(newWordCnt * newSize, 0);
    }
    else if (newWordCnt > oldWordCnt)
    {
        connections.resize(newWordCnt * newSize, 0);
        for (int v = vertexCnt - 1; v >= 0; v--)
        {
            uint64_t* from = connections.data() + v * oldWordCnt;
            uint64_t* to = connections.data() + v * newWordCnt;
            memmove(to, from, sizeof(uint64_t) * oldWordCnt);
            memset(to + oldWordCnt, 0, sizeof(uint64_t) * (newWordCnt - oldWordCnt));
        }
    }
    else
    {
        for (int v = 0; v < newSize; v++)
        {
            uint64_t* from = connections.data() + uint32_t(v) * oldWordCnt;
            uint64_t* to = connections.data() + uint32_t(v) * newWordCnt;
            memmove(to, from, sizeof(uint64_t) * newWordCnt);
        }
        connections.resize(newWordCnt * newSize, 0);
    }
    wordCnt = newWordCnt;
    vertexCnt = newSize;
}

void BaseGraph::reserve(int newSize)
{
    const uint32_t newWordCnt = uint32_t((uint32_t(newSize) + 63u) / 64u);
    vertexFlags.reserve(newSize * vertexFlagCount);
    connections.reserve(newSize * newWordCnt);
}

BaseGraph& BaseGraph::operator=(BaseGraph&& rhs)
{
    connections = std::move(rhs.connections);
    vertexCnt = std::exchange(rhs.vertexCnt, 0);
    wordCnt = std::exchange(rhs.wordCnt, 0);
    vertexFlags = std::move(rhs.vertexFlags);
    vertexFlagCount = std::exchange(rhs.vertexFlagCount, 0);
    return *this;
}

void BaseGraph::copyFrom(const BaseGraph& rhs)
{
    connections = rhs.connections;
    vertexCnt = rhs.vertexCnt;
    wordCnt = rhs.wordCnt;
    vertexFlags = rhs.vertexFlags;
    vertexFlagCount = rhs.vertexFlagCount;
}

// BaseGraph - edges

void BaseGraph::setConnected(uint32_t v1, uint32_t v2, bool set)
{
    SI_TG_ASSERT(v1 < vertexCnt && v2 < vertexCnt);
    uint64_t &word = connections[uint32_t(v1) * wordCnt + uint32_t(v2) / 64u];
    uint64_t bit = uint64_t(1) << uint64_t(v2 % 64u);
    if (set)
        word |= bit;
    else
        word &= ~bit;
}

bool BaseGraph::isConnected(uint32_t v1, uint32_t v2) const
{
    SI_TG_ASSERT(v1 < vertexCnt && v2 < vertexCnt);
    uint64_t word = connections[v1 * wordCnt + v2 / 64u];
    uint64_t bit = uint64_t(1) << uint64_t(v2 % 64u);
    return (word & bit) != 0;
}

BaseGraphEdgesRef BaseGraph::getEdges(uint32_t v1)
{
    SI_TG_ASSERT(v1 < vertexCnt);
    return BaseGraphEdgesRef(connections.data() + v1 * wordCnt, wordCnt);
}

// BaseGraph - flags


bool BaseGraph::getFlag(uint32_t idx, uint32_t flag) const
{
    SI_TG_ASSERT(idx < vertexCnt);
    SI_TG_ASSERT(flag < vertexFlagCount);
    idx = idx * vertexFlagCount + flag;
    return bool(vertexFlags[idx / 64u] & (uint64_t(1) << uint64_t(idx % 64u)));
}

void BaseGraph::setFlag(uint32_t idx, uint32_t flag, bool set)
{
    SI_TG_ASSERT(idx < vertexCnt);
    SI_TG_ASSERT(flag < vertexFlagCount);
    idx = idx * vertexFlagCount + flag;
    uint64_t &word = vertexFlags[idx / 64u];
    uint64_t bit = uint64_t(1) << uint64_t(idx % 64u);
    if (set)
        word |= bit;
    else
        word &= ~bit;
}

void BaseGraph::setAllFlags(uint32_t flag, bool set)
{
    SI_TG_ASSERT(flag < vertexFlagCount);
    for (uint32_t idx = 0; idx < vertexCnt; idx++)
        setFlag(idx, flag, set);
}

void BaseGraph::setFlagCount(uint32_t cnt)
{
    if (vertexFlagCount != cnt)
    {
        vertexFlags.clear();
        vertexFlags.resize(vertexCnt * cnt, 0);
        vertexFlagCount = cnt;
    }
}

}


namespace si::tg
{

// TaskGraph

TaskId TaskGraph::addTask(const TaskData& data)
{
    TaskId id(taskData.size());
    TaskDataExt task;
    task.task = data;
    task.subGraph = nullptr;
    taskData.push_back(task);
    taskOrderGraph.resize(taskData.size());
    return id;
}

TaskId TaskGraph::addSubGraphTask(const TaskData& data, const TaskGraph* graph)
{
    TaskId id(taskData.size());
    SI_TG_ASSERT(data.taskFn == nullptr);
    TaskDataExt task;
    task.task = data;
    task.subGraph = graph;
    taskData.push_back(task);
    taskOrderGraph.resize(taskData.size());
    return id;
}

void TaskGraph::setTaskData(TaskId task, const TaskData& data)
{
    SI_TG_ASSERT(task.value() < taskData.size());
    taskData[task.value()].task = data;
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
    for (auto it = begin; it != end;)
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

}

// PrebuiltTaskGraph

namespace si::tg
{

template<typename F, typename BaseGraphT>
void base_graph_dfs(BaseGraphT& graph, uint32_t v, uint32_t visited_flag, F&& f)
{
    if (graph.getFlag(v, visited_flag))
        return;
    graph.setFlag(v, visited_flag, true);
    f(v, [v, &graph, visited_flag, &f] {
        graph.iterEdges(v, [&](uint32_t vv) {
            base_graph_dfs(graph, vv, visited_flag, f);
        });
    });
}

static uint32_t task_graph_resolve_size(const TaskGraph &graph)
{
    int count = 0;
    for (const TaskGraph::TaskDataExt &task : graph.taskData)
        if (task.subGraph != nullptr)
            count += 2 + task_graph_resolve_size(*task.subGraph); // entry + exit tasks
        else
            count++;
    return count;
}

static void resolve_resources_and_dependencies(PrebuiltTaskGraph &prebuilt, const TaskGraph &graph)
{
    // copy tasks
    uint32_t tasksOffset = prebuilt.taskData.size();
    for (const TaskGraph::TaskDataExt &task : graph.taskData)
        prebuilt.taskData.push_back(task.task); // add subgraph entry tasks as-is with everything else, keep indices
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

    for (uint32_t i = 0; i < graph.taskData.size(); i++)
    {
        if (graph.taskData[i].subGraph != nullptr)
        {
            PrebuiltTaskGraph::SubGraphData subGraphData;
            subGraphData.entryTaskId = tasksOffset + i;
            subGraphData.exitTaskId = prebuilt.taskData.size();
            prebuilt.taskData.emplace_back();
            const TaskGraph& subGraph = *graph.taskData[i].subGraph;
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
            for (uint32_t j = 0; j < subGraph.taskData.size(); j++)
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

bool prebuild_task_graph(const TaskGraph &graph, PrebuiltTaskGraph *prebuilt_ptr)
{
    PrebuiltTaskGraph &prebuilt = *prebuilt_ptr;

    SI_TG_VERBOSE(0, "building graph\n");
    const uint32_t totalTaskCount = task_graph_resolve_size(graph);
    prebuilt.taskGraph.resize(totalTaskCount);
    prebuilt.exclusionGraph.resize(totalTaskCount);
    prebuilt.taskData.reserve(totalTaskCount);
    prebuilt.taskGraph.setFlagCount(2);
    prebuilt.subGraphTasks.reserve(totalTaskCount);

    resolve_resources_and_dependencies(prebuilt, graph);
    SI_TG_ASSERT(totalTaskCount == prebuilt.taskData.size());

    // validate no cycles
    SI_TG_VERBOSE(1, "  validating graph\n");
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
                internal::log_error("node cycle detected! %i", v);
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
    SI_TG_VERBOSE(1, "  normalizing graph\n");
    for (uint32_t baseId = 0; baseId < prebuilt.taskGraph.size(); baseId++)
    {
        prebuilt.traversalData.clear();
        prebuilt.traversalData.resize(prebuilt.taskGraph.size(), 0);
        uint32_t bfsPos = prebuilt.traversalData.size();
        prebuilt.traversalData.push_back(baseId);
        prebuilt.taskGraph.setFlag(baseId, 0, true);
        while (bfsPos < prebuilt.traversalData.size())
        {
            const uint64_t idAndDepth = uint64_t(prebuilt.traversalData[bfsPos++]);
            const uint32_t curId = uint32_t(idAndDepth);
            const uint32_t depth = uint32_t(idAndDepth >> uint64_t(32));
            if (prebuilt.traversalData[curId] >= depth && depth > 0)
                continue;
            prebuilt.traversalData[curId] = depth;
            if (baseId != curId)
                prebuilt.exclusionGraph.setConnectedBoth(baseId, curId, false);
            prebuilt.taskGraph.iterEdges(curId, [&] (uint32_t nextId) {
                if (prebuilt.traversalData[curId] >= depth + 1)
                    return;
                prebuilt.traversalData.push_back((uint64_t(depth + 1) << uint64_t(32)) | uint64_t(nextId));
            });
        }
        prebuilt.taskGraph.iterEdges(baseId, [&] (uint32_t connId) {
            if (prebuilt.traversalData[connId] > 1)
                prebuilt.taskGraph.setConnected(baseId, connId, false);
        });
    }

    for ([[maybe_unused]] const PrebuiltTaskGraph::SubGraphData &sgData : prebuilt.subGraphData)
    {
        SI_TG_ASSERT(sgData.tasksStart < sgData.tasksEnd);
        SI_TG_ASSERT(!prebuilt.taskGraph.isConnected(sgData.entryTaskId, sgData.exitTaskId));
    }

    SI_TG_ASSERT(prebuilt.taskData.size() == prebuilt.taskGraph.size());
    SI_TG_ASSERT(prebuilt.taskData.size() == prebuilt.exclusionGraph.size());
    SI_TG_VERBOSE(1, "  done\n");

    return true;
}

void print_compiled_graph(const CompiledTaskGraph &compiled)
{
    // debug
    internal::log_debug("COMPILED GRAPH\n");
    for (uint32_t groupId = 0; groupId < compiled.allGroups.size(); groupId++)
    {
        const CompiledTaskGraph::TaskGroup &group = compiled.allGroups[groupId];
        internal::log_debug("group #%i (%i)\n", groupId, int(group.subGroupsEnd - group.subGroupsStart));
        for (uint32_t subgroupId = group.subGroupsStart; subgroupId < group.subGroupsEnd; subgroupId++)
        {
            const CompiledTaskGraph::TaskSubGroup &subgroup = compiled.allSubGroups[subgroupId];
            internal::log_debug("  subgroup %02i (%3i) [", subgroupId - group.subGroupsStart, subgroup.tasksEnd - subgroup.tasksStart);
            for (int i = 0; i < 64; i++)
                internal::log_debug("%i", (subgroup.excludedMask >> uint64_t(i)) & uint64_t(1));
            internal::log_debug("]");
            for (uint32_t taskId = subgroup.tasksStart; taskId < subgroup.tasksEnd; taskId++)
            {
                const CompiledTaskGraph::Task &task = compiled.allTasks[taskId];
                internal::log_debug("%i{", int(taskId));
                if (task.subgraphDataIdx != -1)
                    internal::log_debug("sg:%i|%i ", compiled.subGraphData[task.subgraphDataIdx + 1], compiled.subGraphData[task.subgraphDataIdx + 2]);
                int deps = int(task.dependencies >> 32u);
                if (deps == 0 && !task.isPendingOnStart)
                    deps = 1;
                internal::log_debug("d:%i", deps);
                if (task.nextTasksStart != task.nextTasksEnd)
                {
                    internal::log_debug(" next:");
                    for (uint32_t i = task.nextTasksStart; i < task.nextTasksEnd; i++)
                        internal::log_debug(" %i", compiled.nextTaskIds[i]);
                }
                internal::log_debug("} ", int(taskId), deps);
            }
            internal::log_debug("\n");
        }
    }

    if (!compiled.subGraphData.empty())
        internal::log_debug("SUB-GRAPHS\n");
    for (uint32_t idx = 0; idx < compiled.subGraphData.size(); )
    {
        const int32_t id = idx;
        int32_t cnt = compiled.subGraphData[idx++];
        int32_t subgraphEntryTask = compiled.subGraphData[idx++];
        int32_t subgraphExitTask = compiled.subGraphData[idx++];
        idx++;
        internal::log_debug("  subgraph #%i (%i) [%i|%i]: ", id, cnt, subgraphEntryTask, subgraphExitTask);
        for (int32_t i = 0; i < cnt; i++)
            internal::log_debug(" %i", compiled.subGraphData[idx++]);
        internal::log_debug("\n");
    }
}

template<>
bool compile_task_graph(PrebuiltTaskGraph &prebuilt, CompiledTaskGraph *compiled_ptr, strategy::MergeSubgroups *c_state_ptr)
{
    CompiledTaskGraph &compiled = *compiled_ptr;
    strategy::MergeSubgroups &c_state = *c_state_ptr;
    compiled.isValid = false;

    SI_TG_VERBOSE(0, "compiling graph - merge subgroups strategy\n");
    using GroupData = strategy::MergeSubgroups::GroupData;
    using SubgroupData = strategy::MergeSubgroups::SubgroupData;
    using TaskData = strategy::MergeSubgroups::TaskData;
    using MergeState = strategy::MergeSubgroups::MergeState;

    // build groups
    SI_TG_VERBOSE(1, "  building groups\n");
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
    SI_TG_VERBOSE(2, "    added groups (%i)\n", int(c_state.groups.size()));

    // merge subgroups
    for (GroupData &group : c_state.groups)
    {
        group.subgroupCnt = group.subgroups.size();
        if (group.subgroupCnt < 64)
            continue;

        auto mergeSubgroups = [&] (uint64_t dst, uint64_t src)
        {
            SI_TG_ASSERT(src != dst);
            SI_TG_ASSERT(!group.subgroups[src].tasks.empty());
            SI_TG_ASSERT(!group.subgroups[dst].tasks.empty());
            group.subgroupCnt--;
            for (uint32_t t : group.subgroups[src].tasks)
                group.subgroups[dst].tasks.push_back(t);
            group.subgroups[src].tasks.clear();
            group.subgroupExclusionGraph.setConnectedBoth(src, dst, false);
            group.subgroupExclusionGraph.iterEdges(src, [&] (uint32_t v) {
                if (v == src)
                    return;
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
            for (uint32_t i = 0; i < edges1.size(); i++)
                value += internal::count_set_bits(edges1[i] & edges2[i]); // unified edges will be eliminated
            group.subgroupExclusionGraph.setConnectedBoth(sg1, sg2, isConnected);
            return value;
        };

        const auto calcPairSetValue = [&] (MergeState &state, int32_t sg1, int32_t sg2)
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

        [[maybe_unused]] const auto makePair = [&] (MergeState &state, uint32_t sg1, uint32_t sg2)
        {
            int valueChange = calcPairSetValue(state, sg1, sg2);
            state.totalValue += valueChange;
            if (state.sgPair[sg1] >= 0)
            {
                state.sgPair[state.sgPair[sg1]] = -1;
                state.pairCnt--;
            }
            if (state.sgPair[sg2] >= 0)
            {
                state.sgPair[state.sgPair[sg2]] = -1;
                state.pairCnt--;
            }
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


        SI_TG_VERBOSE(1, "  merging large group (%i)\n", int(group.subgroups.size()));
        c_state.subgroupsToMerge.reserve(group.subgroups.size());

        while (group.subgroupCnt > 64)
        {
            c_state.subgroupsToMerge.clear();
            for (uint32_t sg = 0; sg < group.subgroups.size(); sg++)
            {
                if (group.subgroups[sg].tasks.empty())
                    continue;
                c_state.subgroupsToMerge.push_back(sg);
            }
            std::sort(c_state.subgroupsToMerge.begin(), c_state.subgroupsToMerge.end(), [&] (uint32_t a, uint32_t b) {
                return group.subgroupExclusionGraph.getEdges(a).count() > group.subgroupExclusionGraph.getEdges(b).count();
            });
            while (int(group.subgroupCnt) - int(c_state.subgroupsToMerge.size()) / 2 < 64)
                c_state.subgroupsToMerge.pop_back();

            const auto runSinglePass = [&] (MergeState &state)
            {
                while (true)
                {
                    int valueChange = 0;
                    for (uint32_t sg1 : c_state.subgroupsToMerge)
                    {
                        int bestVal = -1;
                        int bestIdx = -1;
                        for (uint32_t sg2 : c_state.subgroupsToMerge)
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
                            makePair(state, sg1, bestIdx);
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
                group.subgroupExclusionGraph.iterEdges(sg1, [&] ([[maybe_unused]] uint32_t sg2) { SI_TG_ASSERT(!group.subgroups[sg2].tasks.empty()); });
            }
            SI_TG_VERBOSE(2, "    merged %i/%i subgroups, value: %i, remaining %i\n", baseState.pairCnt * 2, cntBeforeMerge, baseState.totalValue, group.subgroupCnt);
        }
    }

    // merge small groups
    SI_TG_VERBOSE(1, "  merging small groups (%i)\n", int(c_state.groups.size()));
    const int32_t MIN_GROUP_SIZE = 32;
    while (true)
    {
        int prevCandidateIdx = -1;
        bool anyMerged = false;
        for (uint32_t i = 0; i < c_state.groups.size(); i++)
        {
            if (prevCandidateIdx >= 0 && c_state.groups[i].subgroupCnt + c_state.groups[prevCandidateIdx].subgroupCnt <= 32)
            {
                GroupData &dst = c_state.groups[prevCandidateIdx];
                GroupData &src = c_state.groups[i];
                dst.subgroupExclusionGraph.resize(dst.subgroupExclusionGraph.size() + src.subgroupExclusionGraph.size());
                for (uint32_t sg1 = 0; sg1 < src.subgroups.size(); sg1++)
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
            else if ((prevCandidateIdx < 0 || c_state.groups[i].subgroupCnt < c_state.groups[prevCandidateIdx].subgroupCnt) && c_state.groups[i].subgroupCnt < MIN_GROUP_SIZE)
                prevCandidateIdx = i;
        }
        c_state.groups.erase(std::remove_if(c_state.groups.begin(), c_state.groups.end(), [&] (auto &g) { return g.subgroups.empty(); }), c_state.groups.end());
        if (!anyMerged)
            break;
    }
    SI_TG_VERBOSE(1, "    done (%i)\n", int(c_state.groups.size()));

    // cleanup subgroups & init masks
    SI_TG_VERBOSE(1, "  cleaning up\n");
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
            SI_TG_ASSERT(int32_t(i) == group.subgroups[i].subgroupIdx);
    }

    // write to compiled
    SI_TG_VERBOSE(1, "  writing to compiled graph\n");
    c_state.allTasks.resize(prebuilt.taskData.size());
    for (uint32_t i = 0; i < prebuilt.taskData.size(); i++)
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
    for (uint32_t groupId = 0; groupId < c_state.groups.size(); groupId++)
    {
        GroupData &group = c_state.groups[groupId];
        uint64_t &initialPendingSubgroups = compiled.allGroups[groupId].initialPending;

        compiled.allGroups[groupId].subGroupsStart = compiled.allSubGroups.size();
        for (SubgroupData &subgroup : group.subgroups)
        {
            const uint32_t globalSubgroupId = compiled.allSubGroups.size();
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

    SI_TG_VERBOSE(1, "  done\n");
    compiled.isValid = true;
    return true;
}

}