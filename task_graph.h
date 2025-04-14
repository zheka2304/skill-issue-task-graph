#pragma once

#include <cstdint>
#include <vector>
#include <cstring>
#include <cassert>
#include <span>

#include "task_graph_config.h"


namespace si::tg::internal
{

// internal::BitIter iter(word);
// while (iter.step())
//   iter.idx();
struct BitIter
{
    explicit BitIter(uint64_t w) : word(w), idx_(-1) {}

    bool step()
    {
        if (idx_ >= 63)
            return false;
        idx_++;
        if ((word >> idx_) == 0)
            return false;
        idx_ += SI_TG_FIRST_SET_BIT(word >> idx_);
        return true;
    }
    uint64_t idx() const { return idx_ < 0 ? 0 : idx_; }

private:
    uint64_t word;
    int64_t idx_;
};

template<typename F>
uint64_t iter_set_bits(uint64_t word, F&& f, uint64_t offs = 0)
{
    uint64_t cnt = 0;
    uint64_t idx = 0;
    while (word != 0)
    {
        uint64_t s = SI_TG_FIRST_SET_BIT(word);
        idx += s;
        word >>= s;
        assert(bool(word &1u));
        f(idx + offs);
        idx++;
        word >>= 1;
        cnt++;
    }
    return cnt;
}

inline uint64_t count_set_bits(uint64_t word)
{
    return SI_TG_COUNT_SET_BITS(word);
}

}

namespace si::tg
{

struct BaseGraphEdgesRef
{
    BaseGraphEdgesRef() = default;
    explicit BaseGraphEdgesRef(uint64_t *data, uint32_t cnt) : data(data), cnt(cnt) {}
    uint64_t *begin() const { return data; }
    uint64_t *end() const { return data + cnt; }
    uint64_t size() const { return cnt; }
    uint64_t &operator[](uint32_t i) const { SI_TG_ASSERT(i < cnt); return data[i]; }

    template<typename F>
    void iter(F &&f) const
    {
        uint32_t offset = 0;
        for (uint64_t word : *this)
        {
            internal::iter_set_bits(word, f, offset);
            offset += 64u;
        }
    }

    uint32_t count() const
    {
        uint32_t r = 0;
        for (uint64_t w : *this)
            r += internal::count_set_bits(w);
        return r;
    }
    bool operator==(const BaseGraphEdgesRef &rhs) const
    {
        if (size() != rhs.size())
            return false;
        bool r = true;
        for (int i = 0; i < size(); i++)
            r &= data[i] == rhs.data[i];
        return r;
    }

    size_t hash() const
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
private:
    uint64_t *data;
    uint32_t cnt;
};

struct BaseGraph
{
    BaseGraph() = default;
    template<typename Allocator>
    explicit BaseGraph(const Allocator &allocator) : connections(allocator), vertexFlags(allocator) {}
    BaseGraph(const BaseGraph &rhs) = delete;
    void operator=(const BaseGraph &rhs) = delete;
    BaseGraph(BaseGraph &&rhs) { *this = std::move(rhs); }

    template<typename OtherT>
    void copyFrom(const OtherT &rhs)
    {
        connections = rhs.connections;
        vertexCnt = rhs.vertexCnt;
        wordCnt = rhs.wordCnt;
        vertexFlags = rhs.vertexFlags;
        vertexFlagCount = rhs.vertexFlagCount;
    }

    BaseGraph &operator=(BaseGraph &&rhs)
    {
        connections = std::move(rhs.connections);
        vertexCnt = std::exchange(rhs.vertexCnt, 0);
        wordCnt = std::exchange(rhs.wordCnt, 0);
        vertexFlags = std::move(rhs.vertexFlags);
        vertexFlagCount = std::exchange(rhs.vertexFlagCount, 0);
        return *this;
    }

    void clear() { resize(0); }


    // vertex count
    int32_t size() const { return vertexCnt; }

    void reserve(int newSize)
    {
        const uint32_t newWordCnt = uint32_t((uint32_t(newSize) + 63u) / 64u);
        vertexFlags.reserve(newSize * vertexFlagCount);
        connections.reserve(newSize * newWordCnt);
    }

    void resize(int newSize)
    {
        SI_TG_ASSERT(newSize >= 0);
        if (vertexCnt == newSize)
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

    void setConnected(uint32_t v1, uint32_t v2, bool set)
    {
        SI_TG_ASSERT(v1 < vertexCnt && v2 < vertexCnt);
        uint64_t &word = connections[uint32_t(v1) * wordCnt + uint32_t(v2) / 64u];
        uint64_t bit = uint64_t(1) << uint64_t(v2 % 64u);
        if (set)
            word |= bit;
        else
            word &= ~bit;
    }

    void setConnectedBoth(uint32_t v1, uint32_t v2, bool set)
    {
        setConnected(v1, v2, set);
        setConnected(v2, v1, set);
    }

    bool isConnected(uint32_t v1, uint32_t v2) const
    {
        SI_TG_ASSERT(v1 < vertexCnt && v2 < vertexCnt);
        uint64_t word = connections[v1 * wordCnt + v2 / 64u];
        uint64_t bit = uint64_t(1) << uint64_t(v2 % 64u);
        return (word & bit) != 0;
    }

    BaseGraphEdgesRef getEdges(uint32_t v1)
    {
        SI_TG_ASSERT(v1 < vertexCnt);
        return BaseGraphEdgesRef(connections.data() + v1 * wordCnt, wordCnt);
    }

    template<typename F>
    void iterEdges(uint32_t v1, F &&f) const { const_cast<BaseGraph *>(this)->getEdges(v1).iter(std::forward<F>(f)); }

    void setFlagCount(uint32_t cnt)
    {
        if (vertexFlagCount != cnt)
        {
            vertexFlags.clear();
            vertexFlags.resize(vertexCnt * cnt, 0);
            vertexFlagCount = cnt;
        }
    }

    void setFlag(uint32_t idx, uint32_t flag, bool set)
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

    void setAllFlags(uint32_t flag, bool set)
    {
        SI_TG_ASSERT(flag < vertexFlagCount);
        for (uint32_t idx = 0; idx < vertexCnt; idx++)
            setFlag(idx, flag, set);
    }

    bool getFlag(uint32_t idx, uint32_t flag) const
    {
        SI_TG_ASSERT(idx < vertexCnt);
        SI_TG_ASSERT(flag < vertexFlagCount);
        idx = idx * vertexFlagCount + flag;
        return bool(vertexFlags[idx / 64u] & (uint64_t(1) << uint64_t(idx % 64u)));
    }

private:
    Vector<uint64_t> connections;
    uint32_t vertexCnt = 0;
    uint32_t wordCnt = 0;
    Vector<uint64_t> vertexFlags;
    uint32_t vertexFlagCount = 0;
};

template<typename F, typename BaseGraphT>
void base_graph_dfs(BaseGraphT &graph, uint32_t v, uint32_t visited_flag, F &&f)
{
    if (graph.getFlag(v, visited_flag))
        return;
    graph.setFlag(v, visited_flag, true);
    f(v, [v, &graph, visited_flag, &f] {
        graph.iterEdges(v, [&] (uint32_t vv) {
            base_graph_dfs(graph, vv, visited_flag, f);
        });
    });
}

struct TaskId
{
    static constexpr uint32_t INVALID_ID_VAL = ~uint32_t(0);

    TaskId() = default;
    explicit TaskId(uint32_t id) : id(id) {}
    bool operator==(TaskId rhs) const { return id == rhs.id; }
    bool operator!=(TaskId rhs) const { return id != rhs.id; }
    bool operator<(TaskId rhs) const { return id < rhs.id; }
    explicit operator bool() const { return valid(); }
    bool valid() const { return id != INVALID_ID_VAL; }
    uint32_t value() const { return id; }

private:
    uint32_t id = INVALID_ID_VAL;
};

struct ResourceIdHash
{
    size_t operator()(uint64_t x) const { return x; }
};

enum class ResourceUsage : uint8_t
{
    NOT_USED = 0,
    SHARED,
    LOCKING
};

struct TaskData
{
    using VarTaskFnPtr = int (*)(void*);
    using TaskFnPtr = void (*)(void*, int);

    TaskFnPtr taskFn = nullptr;
    VarTaskFnPtr taskVarFn = nullptr;
    void* userData = nullptr;

    bool valid() const { return taskFn != nullptr || taskVarFn != nullptr; }
};

struct TaskGraph
{
    TaskGraph() = default;
    template<typename Allocator>
    explicit TaskGraph(const Allocator &allocator) :
        taskData(allocator), taskSubGraph(allocator), taskOrderGraph(allocator), taskResourceUsage(allocator) {}
    TaskGraph(const TaskGraph &) = delete;
    void operator=(const TaskGraph &) = delete;
    TaskGraph(TaskGraph &&) = default;
    TaskGraph &operator=(TaskGraph &&) = default;
    void copyFrom(const TaskGraph &rhs)
    {
        taskData = rhs.taskData;
        taskSubGraph = rhs.taskSubGraph;
        taskResourceUsage = rhs.taskResourceUsage;
        taskOrderGraph.copyFrom(rhs.taskOrderGraph);
    }

    TaskId addTask(const TaskData &data = {});
    TaskId addSubGraphTask(const TaskData &data, const TaskGraph *graph);
    void setTaskData(TaskId task_id, const TaskData &data);
    void setNext(TaskId task_id, TaskId next_id, bool set = true);
    void setResourceUsage(TaskId task_id, uint64_t resource_id, ResourceUsage usage);
    ResourceUsage getResourceUsage(TaskId task_id, uint64_t resource_id) const;
    void clear();

public:
    Vector<TaskData> taskData;
    Vector<const TaskGraph *> taskSubGraph;
    BaseGraph taskOrderGraph;
    struct TaskResourceUsageData
    {
        uint32_t task : 31;
        uint32_t lockingBit : 1;
    };
    FlatHashMultiMap<uint64_t, TaskResourceUsageData, ResourceIdHash> taskResourceUsage;
};


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
