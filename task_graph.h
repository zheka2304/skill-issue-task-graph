#pragma once

#include <cstdint>
#include <vector>

namespace sie
{

struct TaskGraph;
using TaskFnPtr = void (*)(void);

struct TaskGraph
{
    static constexpr uint32_t INVALID_ID = ~0u;

    struct ResourceRef { uint64_t id: 63, write: 1; };

    struct TaskNode
    {
        void addNext(uint32_t id);
        void addPrev(uint32_t id);

    public:
        TaskFnPtr fn;

        //
        bool visited = false;
        uint32_t assignedQueueId = INVALID_ID;

        std::vector<uint32_t> prevTasks;
        std::vector<uint32_t> nextTasks;
        std::vector<TaskGraph::ResourceRef> resources;
    };

public:
    uint32_t addTask();
    void setNext(uint32_t task, uint32_t next);
    void addResource(uint32_t task, uint64_t resId, bool write);

public:
    std::vector<TaskNode> allNodes;
};

}
