#pragma once

#include <atomic>
#include <thread>
#include <condition_variable>

#include "task_graph.h"


namespace sie
{

struct CompiledTaskGraph
{
    struct TaskGroup
    {
        std::atomic<uint64_t> pending = 0;
        std::atomic<uint64_t> executing = 0;

        uint32_t subGroupsStart;
        uint32_t subGroupsEnd;
        // 24 bytes
        char _falseSharingPad[128 - 24];

        TaskGroup() = default;
        TaskGroup(const TaskGroup &rhs) { memcpy(this, &rhs, offsetof(TaskGroup, _falseSharingPad)); }
    };

    struct TaskSubGroup
    {
        std::atomic<uint64_t> curVarTask = 0;
        std::atomic<int64_t> remainingVarTasks = 0;
        uint64_t excludedMask = 0;

        uint32_t tasksStart;
        uint32_t tasksEnd;
        // 32 bytes
        char _falseSharingPad[64 - 32];

        TaskSubGroup() = default;
        TaskSubGroup(const TaskSubGroup &rhs) { memcpy(this, &rhs, offsetof(TaskSubGroup, _falseSharingPad)); }
    };

    struct Task
    {
        static constexpr uint8_t STATE_NONE = 0;
        static constexpr uint8_t STATE_PENDING = 1;
        static constexpr uint8_t STATE_EXECUTING = 2;
        static constexpr uint8_t STATE_DONE = 3;
        std::atomic<uint8_t> state = STATE_NONE;
        bool isPendingOnStart = false;

        std::atomic<uint64_t> dependencies = 0;
        std::vector<uint32_t> nextTasks;

        uint32_t groupId;
        uint32_t subGroupId;

        TaskFnPtr task;
        VarTaskFnPtr varTask;
        bool allowToRunInParallelWithItself;

        Task() = default;
        Task(Task &&rhs)
        {
            memcpy(this, &rhs, sizeof(Task));
            new (&rhs) Task(); // reset
        }
    };

    std::vector<TaskGroup> allGroups;
    std::vector<TaskSubGroup> allSubGroups;
    std::vector<Task> allTasks;

    void doThread();
    bool checkAllTasksDone();
    bool checkAllTasksExecutingOrDone();
    bool tryEnterSubgroup(uint32_t group_id, uint32_t subgroup_id, bool loop);
    std::pair<uint32_t, uint32_t> tryAcquireVarTask(uint32_t group_id, uint32_t subgroup_id);
    void leaveSubgroup(uint32_t group_id, uint32_t subgroup_id);
    bool doSubGroup(uint32_t group_id, uint32_t subgroup_id);
    bool doVarTask(uint32_t subgroup_id, uint32_t task_id, uint32_t var_task_idx);
    bool afterTaskDone(uint32_t task_id);
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