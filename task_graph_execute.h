#pragma once

#include <atomic>
#include <thread>
#include <condition_variable>

#include "task_graph.h"


namespace sie
{

struct CompiledTaskGraph
{
    struct TaskExecutionState
    {
        static constexpr uint32_t DONE_BIT = 0x80000000u;
        static constexpr uint32_t EXECUTING_BIT = 0x40000000u;
        static constexpr uint32_t PENDING_BIT = 0x20000000u;
        std::atomic<uint32_t> state = 0;

        bool setPendingAndPreCheck();
        bool tryExec();
        void endExec();
        bool checkDone();
        bool tryLock();
        bool unlockAndCheckPending();
    };

    struct TaskNode
    {
        TaskFnPtr task;
        uint32_t queueId;
        std::vector<uint32_t> nextQueues; // queues to start after this task is executed

        TaskExecutionState state;
        std::vector<int> lockedTasks;
    };

    // signal nodes

    struct SignalTreeNodeRoot
    {
        static constexpr bool is_root = true;
        static constexpr bool is_leaf = false;

        std::atomic<uint64_t> counter = 0;
        uint8_t falseSharingPad[128 - 8];

        SignalTreeNodeRoot() = default;
        SignalTreeNodeRoot(SignalTreeNodeRoot&& rhs) : counter(rhs.counter.load(std::memory_order_relaxed))
        {}
    };

    struct SignalTreeNodeMiddle
    {
        static constexpr bool is_root = false;
        static constexpr bool is_leaf = false;
        std::atomic<uint64_t> counter = 0;
        uint8_t falseSharingPad[64 - 8];

        SignalTreeNodeMiddle() = default;
        SignalTreeNodeMiddle(SignalTreeNodeMiddle&& rhs) : counter(rhs.counter.load(std::memory_order_relaxed))
        {}
    };

    struct SignalTreeNodeLeaf
    {
        static constexpr bool is_root = false;
        static constexpr bool is_leaf = true;

        std::atomic<uint64_t> counter = 0;
        std::atomic<uint64_t> requirements = 0;
        std::vector<uint32_t> taskQueue;
        uint32_t taskQueuePos = 0;

        SignalTreeNodeLeaf() = default;
        SignalTreeNodeLeaf(SignalTreeNodeLeaf&& rhs) :
                counter(rhs.counter.load(std::memory_order_relaxed)),
                requirements(rhs.requirements.load(std::memory_order_relaxed)),
                taskQueue(std::move(rhs.taskQueue)),
                taskQueuePos(std::exchange(rhs.taskQueuePos, 0u))
        {}

        void initBeforeStart();
    };

    SignalTreeNodeRoot treeRootNode;
    std::vector<SignalTreeNodeMiddle> treeMiddleNodes;
    std::vector<SignalTreeNodeLeaf> queueNodes;

    uint32_t treeDepthMinusTwo = 0;
    uint32_t treeLeafHalfCount = 0;
    std::vector<TaskNode> allTasks;

    static uint32_t nextPowOf2(uint32_t v);

    void rebuildTree();

    void initBeforeStart();

    void setTreeNodeRequirement(uint32_t nodeId);

    void setTreeNode(uint32_t nodeId);

    template<typename T>
    bool selectNodeImpl(T& node);

    uint32_t selectTreeNode();

    bool executeQueueWhileCan(uint32_t thisQueueId);
};

struct TaskGraphExecutor
{
    CompiledTaskGraph* graph;

    TaskGraphExecutor() = default;
    ~TaskGraphExecutor();
    TaskGraphExecutor(const TaskGraphExecutor&) = delete;
    TaskGraphExecutor& operator=(const TaskGraphExecutor&) = delete;

    void windUp(int count);
    void wakeAll();
    void shutdown();

private:
    static void exec(TaskGraphExecutor* self, int thread_id);

private:
    std::vector<std::thread> threads;
    std::condition_variable condVar;
    std::mutex condVarMutex;
    bool running = false;
};

}