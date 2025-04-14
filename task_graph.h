#pragma once

#include <cstdint>
#include <vector>

namespace sie
{

struct TaskGraph;
struct CompiledTaskGraph;
using TaskFnPtr = void (*)(void);

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
        std::vector<uint32_t> nextTasks;
        std::vector<TaskGraph::ResourceRef> resources;

        // volatile
        bool visited = false;
        uint32_t fiberId = INVALID_ID;
        std::vector<uint32_t> prevTasks;
    };

    // Tasks, executed in sequence. Dependencies are needed only to start
    struct Fiber
    {
        std::vector<uint32_t> dependencies;
        std::vector<uint32_t> tasks;
    };

public:
    uint32_t addTask();
    void setNext(uint32_t task, uint32_t next);
    void addResource(uint32_t task, uint64_t resId, bool write);

    template<typename U, typename F>
    void traverseNodeSequence(std::vector<U> &stack, std::vector<uint32_t> &visited, uint32_t node, F &&f);

    bool validateAndNormalize();
    void buildFibers();
    void dumpToLog();
    bool compileTo(CompiledTaskGraph &graph);

public:
    std::vector<TaskNode> allNodes;
    std::vector<Fiber> allFibers;
    std::vector<uint32_t> entryFiberIds;
};

}
