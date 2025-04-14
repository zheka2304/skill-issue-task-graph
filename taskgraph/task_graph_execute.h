#pragma once

#include <atomic>
#include "task_graph.h"


namespace si::tg
{

struct CompiledTaskGraph
{
    struct TaskGroup
    {
        uint64_t initialPending = 0;
        uint32_t subGroupsStart = 0;
        uint32_t subGroupsEnd = 0;
    };

    struct TaskGroupState
    {
        std::atomic<uint64_t> pending = 0;
        std::atomic<uint64_t> executing = 0;
        // 16 bytes
        char _falseSharingPad[128 - 16];

        TaskGroupState() = default;
        TaskGroupState(const TaskGroupState &rhs) { memcpy(this, &rhs, offsetof(TaskGroupState, _falseSharingPad)); }
    };

    struct TaskSubGroup
    {
        uint64_t excludedMask = 0;
        uint32_t tasksStart = 0;
        uint32_t tasksEnd = 0;
    };

    struct TaskSubGroupState
    {
        std::atomic<uint64_t> curVarTask = 0;
        std::atomic<int64_t> remainingVarTasks = 0;
        // 16 bytes
        char _falseSharingPad[64 - 16];

        TaskSubGroupState() = default;
        TaskSubGroupState(const TaskSubGroupState &rhs) { memcpy(this, &rhs, offsetof(TaskSubGroupState, _falseSharingPad)); }
    };

    struct Task
    {
        static constexpr uint8_t STATE_NONE = 0;
        static constexpr uint8_t STATE_PENDING = 1;
        static constexpr uint8_t STATE_EXECUTING = 2;
        static constexpr uint8_t STATE_DONE = 3;
        std::atomic<uint8_t> state = STATE_NONE;
        bool isPendingOnStart = false;
        bool allowToRunInParallelWithItself = false;

        std::atomic<uint64_t> dependencies = 0;
        uint32_t nextTasksStart;
        uint32_t nextTasksEnd;

        uint32_t groupId;
        uint32_t subGroupId;
        int32_t subgraphDataIdx = -1;

        TaskData taskData;


        Task() = default;
        Task(Task &&rhs)
        {
            memcpy(this, &rhs, sizeof(Task));
            new (&rhs) Task(); // reset
        }
    };

    Vector<TaskGroup> allGroups;
    Vector<TaskGroupState> allGroupsState;
    Vector<TaskSubGroup> allSubGroups;
    Vector<TaskSubGroupState> allSubGroupsState;
    Vector<Task> allTasks;
    Vector<uint32_t> nextTaskIds;
    // - task count
    // - subgraph entry task
    // - subgraph exit task
    // - remaining count
    // - [task id]
    Vector<int32_t> subGraphData;
};


struct ThreadedTaskGraphExecutor
{
    CompiledTaskGraph *graphPtr;

    enum class ThreadResult : uint8_t
    {
        WAIT = 0,
        EXIT,
        ALL_DONE
    };

    void prepareForExecution(int thread_num);
    void setWakeCallback(int thread_id, WakeThreadsCallback wake_callback);
    ThreadResult doThread(int thread_id);
    void validateAllDone() const;

    enum class Event : uint8_t
    {
        TASK_EXECUTE_START = 0,
        TASK_EXECUTE_END,
        TASK_VAR_ACQUIRE_CAS,
        TASK_VAR_ACQUIRE,
        TASK_VAR_WIND_UP,
        TASK_VAR_EXECUTE,
        SUBGROUP_ENTER_ATTEMPT,
        SUBGROUP_ENTER_CAS,
        SUBGROUP_ENTER,
        SUBGROUP_LEAVE,
        PENDING_INC_DEPENDENCY,
        PENDING_ADD_TASK,
        PENDING_ADD_GROUP,
        SUBGRAPH_ENTER,
        SUBGRAPH_RESTART,
        SUBGRAPH_EXIT,
        SUBGRAPH_SKIP,
        THREAD_WAIT,
        THREAD_EXIT,
        THREAD_START_GROUP,
        THREAD_NOTHING_PENDING,

        NUM
    };
    static constexpr const char *EVENT_NAMES[] = {
        "TASK_EXECUTE_START",
        "TASK_EXECUTE_END",
        "TASK_VAR_ACQUIRE_CAS",
        "TASK_VAR_ACQUIRE",
        "TASK_VAR_WIND_UP",
        "TASK_VAR_EXECUTE",
        "SUBGROUP_ENTER_ATTEMPT",
        "SUBGROUP_ENTER_CAS",
        "SUBGROUP_ENTER",
        "SUBGROUP_LEAVE",
        "PENDING_INC_DEPENDENCY",
        "PENDING_ADD_TASK",
        "PENDING_ADD_GROUP",
        "SUBGRAPH_ENTER",
        "SUBGRAPH_RESTART",
        "SUBGRAPH_EXIT",
        "SUBGRAPH_SKIP",
        "THREAD_WAIT",
        "THREAD_EXIT",
        "THREAD_START_GROUP",
        "THREAD_NOTHING_PENDING",
    };
    static_assert(sizeof(EVENT_NAMES) / sizeof(char *) == int(Event::NUM));

    struct TimedEvent
    {
        Event event;
        int64_t timestamp;
        Array<uint32_t, 4> ids;
        bool operator<(const TimedEvent &rhs) const { return timestamp < rhs.timestamp; }
    };

    Array<int64_t, uint8_t(Event::NUM)> getEventCountStats() const;

    Vector<TimedEvent> getAllTimedEvents() const;

private:

    struct ThreadCtx
    {
        int threadId;

        uint32_t groupId = 0;
        uint32_t subgroupId = 0;
        bool isSubgroupOwned = false;

        uint32_t failedGroups = 0;
        uint32_t failedSubgroups = 0;
        uint64_t executingMask = 0;

        WakeThreadsCallback wakeCb;
        ThreadedTaskGraphExecutor *executor;

        Vector<uint64_t> subGroupMasksToExecuteNext;

#if SI_TG_ENABLE_DEBUG_STAT_EVENTS
        Array<int64_t, uint8_t(Event::NUM)> eventCnt = {0};
#endif
#if SI_TG_ENABLE_DEBUG_TIMED_EVENTS
        Vector<TimedEvent> timedEvents;
#endif

        char _falseSharingPad[128];

        ~ThreadCtx();

        template<Event Evt, typename ...Args>
        void addEvent(int64_t v, Args &&... args);
    };
    Vector<ThreadCtx> threadCtxArray;
    const char _falseSharingPad[128] = {0};
    std::atomic<uint64_t> sleepingThreadsMask = 0;
    std::atomic<bool> allDoneEventPending = false;
#if SI_TG_ENABLE_DEBUG_TIMED_EVENTS
    std::atomic<int64_t> curTimedEventIdx;
#endif

    bool doGroupUntilSubgroupEnter(ThreadCtx & __restrict ctx, bool allow_var_tasks);
    uint64_t gatherThreadsToWake();

    bool tryEnterSubgroup(ThreadCtx &ctx, uint32_t group_id, uint32_t subgroup_id, uint64_t &executing, bool loop);
    std::pair<uint32_t, uint32_t> tryAcquireVarTask(ThreadCtx &ctx, uint32_t group_id, uint32_t subgroup_id);
    void leaveSubgroup(ThreadCtx &ctx, uint32_t group_id, uint32_t subgroup_id);
    bool doSubGroup(ThreadCtx &ctx, uint32_t group_id, uint32_t subgroup_id);
    bool doVarTask(ThreadCtx &ctx, uint32_t subgroup_id, uint32_t task_id, uint32_t var_task_idx);
    void doSubGraphTask(ThreadCtx &ctx, uint32_t task_id);
    bool afterTaskDone(ThreadCtx &ctx, uint32_t task_id);
};

}