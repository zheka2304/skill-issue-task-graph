#pragma once

#include <atomic>
#include <thread>
#include <condition_variable>
#include <functional>

#include "task_graph.h"


namespace sie
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

        std::atomic<uint64_t> dependencies = 0;
        std::vector<uint32_t> nextTasks;

        uint32_t groupId;
        uint32_t subGroupId;

        TaskFnPtr task;
        VarTaskFnPtr varTask;
        void* taskUserData;
        bool allowToRunInParallelWithItself;

        Task() = default;
        Task(Task &&rhs)
        {
            memcpy(this, &rhs, sizeof(Task));
            new (&rhs) Task(); // reset
        }
    };

    std::vector<TaskGroup> allGroups;
    std::vector<TaskGroupState> allGroupsState;
    std::vector<TaskSubGroup> allSubGroups;
    std::vector<TaskSubGroupState> allSubGroupsState;
    std::vector<Task> allTasks;
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
    ThreadResult doThread(int thread_id, const std::function<void(int)> &wake_threads);

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
        std::array<uint32_t, 1> ids;
        bool operator<(const TimedEvent &rhs) const { return timestamp < rhs.timestamp; }
    };

    std::array<int64_t, uint8_t(Event::NUM)> getEventCountStats() const
    {
        std::array<int64_t, uint8_t(Event::NUM)> result = {0};
        for (const ThreadCtx &ctx : threadCtxArray)
            for (int i = 0; i < uint8_t(Event::NUM); i++)
                result[i] += ctx.eventCnt[i];
        return result;
    }

    std::vector<TimedEvent> getAllTimedEvents() const
    {
        std::vector<TimedEvent> all;
        for (const ThreadCtx &ctx : threadCtxArray)
            for (const TimedEvent &evt : ctx.timedEvents)
                all.push_back(evt);
        std::sort(all.begin(), all.end());
        return all;
    }

private:

    struct ThreadCtx
    {
        int threadId;
        uint32_t groupId = 0;
        uint32_t failedGroups = 0;
        uint32_t failedSubgroups = 0;
        ThreadedTaskGraphExecutor *executor;

        std::vector<uint64_t> subGroupMasksToExecuteNext;

        std::array<int64_t, uint8_t(Event::NUM)> eventCnt = {0};
        std::vector<TimedEvent> timedEvents;

        char _falseSharingPad[128];

        template<Event Evt, typename ...Args>
        void addEvent(int64_t v, Args &&... args)
        {
            if (false)
            if (Evt != Event::SUBGROUP_ENTER_ATTEMPT &&
                Evt != Event::THREAD_WAIT &&
                Evt != Event::THREAD_NOTHING_PENDING &&
                Evt != Event::THREAD_START_GROUP)
            {
                if (sizeof...(args) == 0)
                    logger::debug("exec", "[%i] %s", threadId, EVENT_NAMES[int(Evt)], int(args)...);
                else if (sizeof...(args) == 1)
                    logger::debug("exec", "[%i] %s %i", threadId, EVENT_NAMES[int(Evt)], int(args)...);
                else if (sizeof...(args) == 2)
                    logger::debug("exec", "[%i] %s %i %i", threadId, EVENT_NAMES[int(Evt)], int(args)...);
                else if (sizeof...(args) == 3)
                    logger::debug("exec", "[%i] %s %i %i %i", threadId, EVENT_NAMES[int(Evt)], int(args)...);
                else if (sizeof...(args) == 4)
                    logger::debug("exec", "[%i] %s %i %i %i %i", threadId, EVENT_NAMES[int(Evt)], int(args)...);
            }
            eventCnt[int(Evt)] += v;
            if constexpr (Evt == Event::TASK_EXECUTE_START || Evt == Event::TASK_EXECUTE_END)
                timedEvents.push_back(TimedEvent{ Evt, executor->curTimedEventIdx.fetch_add(1, std::memory_order_relaxed), args... });
        }
    };
    std::vector<ThreadCtx> threadCtxArray;
    std::atomic<int64_t> curTimedEventIdx;

    bool tryEnterSubgroup(ThreadCtx &ctx, uint32_t group_id, uint32_t subgroup_id, uint64_t &executing, bool loop);
    std::pair<uint32_t, uint32_t> tryAcquireVarTask(ThreadCtx &ctx, uint32_t group_id, uint32_t subgroup_id);
    void leaveSubgroup(ThreadCtx &ctx, uint32_t group_id, uint32_t subgroup_id);
    bool doSubGroup(ThreadCtx &ctx, uint32_t group_id, uint32_t subgroup_id);
    bool doVarTask(ThreadCtx &ctx, uint32_t subgroup_id, uint32_t task_id, uint32_t var_task_idx);
    bool afterTaskDone(ThreadCtx &ctx, uint32_t task_id);
};


struct SimpleThreadPool
{
    ThreadedTaskGraphExecutor* executor;

    SimpleThreadPool() = default;
    ~SimpleThreadPool();
    SimpleThreadPool(const SimpleThreadPool&) = delete;
    SimpleThreadPool& operator=(const SimpleThreadPool&) = delete;

    void windUp(int count);
    void wakeAll();
    void waitAll();
    void shutdown();

private:
    static void exec(SimpleThreadPool* self, int thread_id);

private:
    std::vector<std::thread> threads;
    std::condition_variable condVar;
    std::mutex condVarMutex;
    std::atomic<int64_t> wakeThreads = 0;
    bool running = false;
};

}