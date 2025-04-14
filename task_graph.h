#pragma once

#include <cstdint>
#include <vector>
#include <cstring>
#include <cassert>
#include <span>
#include "logger.h"

template<typename F>
static uint64_t iter_set_bits(uint64_t word, F &&f)
{
    uint64_t cnt = 0;
    uint64_t idx = 0;
    while (word != 0)
    {
        uint64_t s = __builtin_ctzll(word);
        idx += s;
        word >>= s;
        assert(bool(word & 1u));
        f(idx);
        idx++;
        word >>= 1;
        cnt++;
    }
    return cnt;
}

template<typename Span, typename F>
static uint64_t iter_set_bits_span(const Span &words, F &&f)
{
    uint64_t cnt = 0;
    uint64_t base = 0;
    for (int i = 0; i < words.size(); i++)
    {
        uint64_t word = const_cast<uint64_t &>(words[i]);
        uint64_t idx = base;
        while (word != 0)
        {
            uint64_t s = __builtin_ctzll(word);
            idx += s;
            word >>= s;
            assert(bool(word &1u));
            f(idx);
            idx++;
            word >>= 1;
            cnt++;
        }
        base += 64;
    }
    return cnt;
}

template<typename F, typename Span, typename ...Spans>
static uint64_t iter_set_bits_span_var(F &&f, Span span, Spans ...spans)
{
    uint64_t cnt = 0;
    uint64_t base = 0;
    uint64_t size = span.size();
    for (uint64_t i = 0; i < size; i++)
    {
        uint64_t idx = 0;
        uint64_t word = span[i] | (spans[i] | ...);
        while (word != 0)
        {
            uint64_t s = __builtin_ctzll(word);
            idx += s;
            word >>= s;
            assert(bool(word & 1u));
            f(base + idx, bool((const_cast<uint64_t&>(span[i]) >> idx) & 1u), bool((const_cast<uint64_t&>(spans[i]) >> idx) & 1u)...);
            idx++;
            word >>= 1;
            cnt++;
        }
        base += 64;
    }
    return cnt;
}

static uint64_t count_set_bits(uint64_t word)
{
    return __builtin_popcountll(word);
}

struct BaseGraph
{
    static constexpr uint32_t INVALID_ID = ~0u;

    int getVertexCount() const { return vertexCnt; }

    struct EdgesRef
    {
        EdgesRef() = default;
        explicit EdgesRef(uint64_t *data, int cnt) : data(data, cnt) {}
        template<typename F>
        void iter(F &&f) { iter_set_bits_span(data, std::forward<F>(f)); }
        std::span<uint64_t> getSpan() const { return data; }
        int count() const
        {
            int cnt = 0;
            for (uint64_t w : data)
                cnt += count_set_bits(w);
            return cnt;
        }

        bool operator==(const EdgesRef &rhs) const
        {
            if (data.size() != rhs.data.size())
                return false;
            bool r = true;
            for (int i = 0; i < data.size(); i++)
                r &= data[i] == rhs.data[i];
            return r;
        }

        bool operator<(const EdgesRef &rhs) const
        {
            if (data.size() != rhs.data.size())
                return data.size() < rhs.data.size();
            for (int i = 0; i < data.size(); i++)
                if (data[i] != rhs.data[i])
                    return data[i]<= rhs.data[i];
            return false;
        }
    private:
        std::span<uint64_t> data;
    };

    void resize(uint32_t newSize)
    {
        if (vertexCnt == newSize)
            return;
        uint32_t oldWordCnt = (vertexCnt + 63u) / 64;
        uint32_t newWordCnt = (newSize + 63u) / 64;
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
        uint64_t &word = connections[v1 * wordCnt + v2 / 64u];
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
        uint64_t word = connections[v1 * wordCnt + v2 / 64u];
        uint64_t bit = uint64_t(1) << uint64_t(v2 % 64u);
        return (word & bit) != 0;
    }

    EdgesRef getEdges(uint32_t v1) { return EdgesRef(connections.data() + v1 * wordCnt, wordCnt); }
    template<typename F>
    void iterEdges(uint32_t v1, F &&f) { getEdges(v1).iter(std::forward<F>(f)); }

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
        for (uint32_t idx = 0; idx < vertexCnt; idx++)
            setFlag(idx, flag, set);
    }

    bool getFlag(uint32_t idx, uint32_t flag) const
    {
        idx = idx * vertexFlagCount + flag;
        return bool(vertexFlags[idx / 64u] & (uint64_t(1) << uint64_t(idx % 64u)));
    }

    BaseGraph() = default;
    BaseGraph(const BaseGraph &rhs) = delete;
    void operator=(const BaseGraph &rhs) = delete;
    BaseGraph(BaseGraph &&rhs) { *this = std::move(rhs); }
    void copyFrom(const BaseGraph &rhs)
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

private:
    std::vector<uint64_t> connections;
    uint32_t vertexCnt = 0;
    uint64_t wordCnt = 0;
    std::vector<uint64_t> vertexFlags;
    uint32_t vertexFlagCount = 0;
};

template<typename F>
void base_graph_dfs(BaseGraph &graph, uint32_t v, uint32_t visited_flag, F &&f)
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

namespace sie
{

struct TaskGraph;
struct CompiledTaskGraph;
using TaskFnPtr = void (*)(void*, int);
using VarTaskFnPtr = int (*)(void*);

struct TaskGraph
{
    static constexpr uint32_t INVALID_ID = ~0u;

    struct ResourceRef { uint64_t id: 63, write: 1; };

    struct TaskNode
    {
        void addNext(uint32_t id);
        void removeNext(uint32_t id);

    public:
        // stable
        TaskFnPtr fn;
        VarTaskFnPtr varFn;
        void* userData;
        std::vector<uint32_t> nextTasks;
        std::vector<TaskGraph::ResourceRef> resources;
    };

    // Tasks, executed in sequence. Dependencies are needed only to start
    struct Fiber
    {
        std::vector<uint32_t> dependencies;
        std::vector<uint32_t> tasks;
    };

public:
    uint32_t addTask();
    void setTaskData(uint32_t task, TaskFnPtr fn, VarTaskFnPtr var_fn, void* data);
    void setNext(uint32_t task, uint32_t next);
    void addResource(uint32_t task, uint64_t resId, bool write);

    bool validateAndNormalize(CompiledTaskGraph &compiled);

public:
    std::vector<TaskNode> allNodes;
    std::vector<Fiber> allFibers;
    std::vector<uint32_t> entryFiberIds;
};

}

template<>
struct std::hash<BaseGraph::EdgesRef>
{
    size_t operator()(const BaseGraph::EdgesRef &val) const
    {
        size_t seed = val.getSpan().size();
        for (uint64_t x : val.getSpan())
        {
            x = ((x >> 16) ^ x) * 0x45d9f3b;
            x = ((x >> 16) ^ x) * 0x45d9f3b;
            x = (x >> 16) ^ x;
            seed ^= x + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};

