#pragma once

#include <taskgraph/task_graph_config.h>


namespace si::tg::internal
{

template<typename F>
uint64_t iter_set_bits(uint64_t word, F&& f, uint64_t offs = 0)
{
    uint64_t cnt = 0;
    uint64_t idx = 0;
    while (word != 0)
    {
#ifdef SI_TG_FIRST_SET_BIT
        uint64_t s = SI_TG_FIRST_SET_BIT(word);
        idx += s;
        word >>= s;
#else
        while (word != 0 && (word & 1u) == 0)
            idx++, word >>= 1;
#endif
        SI_TG_ASSERT(bool(word &1u));
        f(idx + offs);
        idx++;
        word >>= 1;
        cnt++;
    }
    return cnt;
}

inline uint64_t count_set_bits(uint64_t word)
{
#ifdef SI_TG_COUNT_SET_BITS
    return SI_TG_COUNT_SET_BITS(word);
#else
    uint64_t cnt = 0;
    while (word)
        cnt += (word & 1u), word >>= 1;
    return cnt;
#endif
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
    uint32_t size() const { return cnt; }
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

    uint32_t count() const;
    bool operator==(const BaseGraphEdgesRef &rhs) const;
    size_t hash() const;
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
    BaseGraph &operator=(BaseGraph &&rhs);
    void copyFrom(const BaseGraph &rhs);

    void reserve(int32_t newSize);
    void resize(int32_t newSize);
    void clear() { resize(0); }
    // vertex count
    size_t size() const { return vertexCnt; }

    void setConnected(uint32_t v1, uint32_t v2, bool set);
    void setConnectedBoth(uint32_t v1, uint32_t v2, bool set)
    {
        setConnected(v1, v2, set);
        setConnected(v2, v1, set);
    }
    bool isConnected(uint32_t v1, uint32_t v2) const;
    BaseGraphEdgesRef getEdges(uint32_t v1);
    template<typename F>
    void iterEdges(uint32_t v1, F &&f) const { const_cast<BaseGraph *>(this)->getEdges(v1).iter(std::forward<F>(f)); }

    void setFlagCount(uint32_t cnt);
    void setFlag(uint32_t idx, uint32_t flag, bool set);
    void setAllFlags(uint32_t flag, bool set);
    bool getFlag(uint32_t idx, uint32_t flag) const;

private:
    Vector<uint64_t> connections;
    size_t vertexCnt = 0;
    size_t wordCnt = 0;
    Vector<uint64_t> vertexFlags;
    size_t vertexFlagCount = 0;
};

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
        taskData(allocator), taskOrderGraph(allocator), taskResourceUsage(allocator) {}
    TaskGraph(const TaskGraph &) = delete;
    void operator=(const TaskGraph &) = delete;
    TaskGraph(TaskGraph &&) = default;
    TaskGraph &operator=(TaskGraph &&) = default;
    void copyFrom(const TaskGraph &rhs)
    {
        taskData = rhs.taskData;
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
    struct TaskDataExt
    {
        TaskData task;
        const TaskGraph *subGraph;
    };
    Vector<TaskDataExt> taskData;
    BaseGraph taskOrderGraph;
    struct TaskResourceUsageData
    {
        uint32_t task : 31;
        uint32_t lockingBit : 1;
    };
    FlatHashMultiMap<uint64_t, TaskResourceUsageData, ResourceIdHash> taskResourceUsage;
};

}
