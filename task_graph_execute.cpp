#include "task_graph_execute.h"

#include <cassert>
#include "logger.h"
#include "optick.h"


namespace sie
{

using logger::debug;

void ThreadedTaskGraphExecutor::prepareForExecution(int thread_num)
{
    threadCtxArray.resize(0);
    threadCtxArray.resize(thread_num);
    for (int i = 0; i < threadCtxArray.size(); i++)
    {
        threadCtxArray[i].executor = this;
        threadCtxArray[i].threadId = i;
    }
    CompiledTaskGraph & __restrict graph = *graphPtr;
    for (CompiledTaskGraph::Task &task : graph.allTasks)
    {
        task.state.store(task.isPendingOnStart ? CompiledTaskGraph::Task::STATE_PENDING : CompiledTaskGraph::Task::STATE_NONE, std::memory_order_relaxed);
        uint64_t deps = task.dependencies.load(std::memory_order_relaxed);
        deps >>= uint64_t(32u);
        deps <<= uint64_t(32u);
        task.dependencies.store(deps, std::memory_order_relaxed);
    }
    /*
    for (int i = 0; i < graph.allTasks.size(); i++)
        if (graph.allTasks[i].isPendingOnStart)
            debug("exec", "task pending on start %i (subgroup=%i)", i, graph.allTasks[i].subGroupId);
    */
    for (int groupId = 0; groupId < graph.allGroups.size(); groupId++)
    {
        graph.allGroupsState[groupId].pending.store(graph.allGroups[groupId].initialPending, std::memory_order_relaxed);
        /*
        iter_set_bits(graph.allGroups[groupId].initialPending, [&] (int bit_idx) {
            debug("exec", "subgroup pending on start %i (group=%i)", graph.allGroups[groupId].subGroupsStart + bit_idx, groupId);
        });
        */
    }
    curTimedEventIdx.store(0, std::memory_order_relaxed);
}

ThreadedTaskGraphExecutor::ThreadResult ThreadedTaskGraphExecutor::doThread(int thread_id)
{
    OPTICK_EVENT()
    CompiledTaskGraph & __restrict graph = *graphPtr;
    const uint32_t maxFailedGroups = 8;
    const uint32_t maxFailedSubGroups = 32;

    ThreadCtx & __restrict ctx = threadCtxArray[thread_id];
    ctx.failedGroups = 0;
    while (ctx.failedGroups < maxFailedGroups)
    {
        ctx.addEvent<Event::THREAD_START_GROUP>(1, ctx.groupId);
        ctx.failedSubgroups = 0;
        const uint32_t subGroupsStartIdx = graph.allGroups[ctx.groupId].subGroupsStart;
        uint64_t executingMask = graph.allGroupsState[ctx.groupId].executing.load(std::memory_order_relaxed);
        while (ctx.failedSubgroups < maxFailedSubGroups)
        {
            uint64_t pending = graph.allGroupsState[ctx.groupId].pending.load(std::memory_order_relaxed);
            if (pending == 0)
            {
                ctx.addEvent<Event::THREAD_NOTHING_PENDING>(1, ctx.groupId);
                // end this group - nothing is pending
                ctx.failedSubgroups = maxFailedSubGroups;
                break;
            }
            iter_set_bits(pending, [&] (uint32_t bit_idx) {
                uint32_t subgroupId = subGroupsStartIdx + bit_idx;
                bool owned = tryEnterSubgroup(ctx, ctx.groupId, subgroupId, executingMask, false);
                if (!owned)
                {
                    // only try acquiring var task, if this group is executing
                    if ((executingMask >> uint64_t(bit_idx)) & uint64_t(1u))
                    {
                        const auto [varTaskCnt, varTaskId] = tryAcquireVarTask(ctx, ctx.groupId, subgroupId);
                        if (varTaskCnt > 0 && doVarTask(ctx, subgroupId, varTaskId, varTaskCnt - 1))
                            owned = true;
                    }
                }
                if (!owned)
                {
                    ctx.failedSubgroups++;
                    return;
                }
                ctx.failedSubgroups = 0;
                owned = doSubGroup(ctx, ctx.groupId, subgroupId);
                if (owned)
                    leaveSubgroup(ctx, ctx.groupId, subgroupId);
            });
        }
        if (ctx.failedSubgroups == maxFailedSubGroups)
        {
            ctx.failedGroups++;
            ctx.groupId++;
            ctx.groupId %= graph.allGroups.size();
            continue;
        }
    }

    uint8_t minState = CompiledTaskGraph::Task::STATE_DONE;
    for (const CompiledTaskGraph::Task &task : graph.allTasks)
        minState = std::min<uint8_t>(minState, task.state.load(std::memory_order_relaxed));
    if (minState == CompiledTaskGraph::Task::STATE_DONE)
    {
        ctx.addEvent<Event::THREAD_EXIT>(1);
        return ThreadResult::ALL_DONE;
    }
    if (minState == CompiledTaskGraph::Task::STATE_EXECUTING)
    {
        ctx.addEvent<Event::THREAD_EXIT>(1);
        return ThreadResult::EXIT;
    }
    ctx.addEvent<Event::THREAD_WAIT>(1);
    return ThreadResult::WAIT;
}

bool ThreadedTaskGraphExecutor::tryEnterSubgroup(ThreadCtx & __restrict ctx, uint32_t group_id, uint32_t subgroup_id, uint64_t &executing, bool loop)
{
    ctx.addEvent<Event::SUBGROUP_ENTER_ATTEMPT>(1, subgroup_id);
    CompiledTaskGraph & __restrict graph = *graphPtr;
    const uint64_t mask = graph.allSubGroups[subgroup_id].excludedMask;
    CompiledTaskGraph::TaskGroup &group = graph.allGroups[group_id];
    CompiledTaskGraph::TaskGroupState &groupState = graph.allGroupsState[group_id];
    const uint64_t indexInGroup = subgroup_id - group.subGroupsStart;
    const uint64_t subgroupBit = uint64_t(1) << indexInGroup;
    while ((executing & mask) == 0)
    {
        uint64_t newExecuting = executing | subgroupBit;
        ctx.addEvent<Event::SUBGROUP_ENTER_CAS>(1, subgroup_id);
        if (groupState.executing.compare_exchange_strong(executing, newExecuting, std::memory_order_acq_rel))
        {
            ctx.addEvent<Event::SUBGROUP_ENTER>(1, subgroup_id);
            return true;
        }
        if (!loop)
            break;
    }
    return false;
}

std::pair<uint32_t, uint32_t> ThreadedTaskGraphExecutor::tryAcquireVarTask(ThreadCtx & __restrict ctx, uint32_t group_id, uint32_t subgroup_id)
{
    CompiledTaskGraph & __restrict graph = *graphPtr;
    CompiledTaskGraph::TaskSubGroupState& subgroup = graph.allSubGroupsState[subgroup_id];
    uint64_t curVarTask = subgroup.curVarTask.load(std::memory_order_relaxed);
    while (curVarTask != 0)
    {
        ctx.addEvent<Event::TASK_VAR_ACQUIRE_CAS>(1, subgroup_id);
        const uint32_t varTaskCnt = uint32_t(curVarTask);
        const uint32_t varTaskIdx = uint32_t(curVarTask >> 32u);
        const uint32_t newVarTaskCnt = varTaskCnt - 1;
        const uint32_t newVarTaskIdx = newVarTaskCnt ? varTaskIdx : 0;
        const uint64_t newVarTask = uint64_t(newVarTaskCnt) | (uint64_t(newVarTaskIdx) << uint64_t(32u));
        if (subgroup.curVarTask.compare_exchange_strong(curVarTask, newVarTask, std::memory_order_acq_rel))
        {
            ctx.addEvent<Event::TASK_VAR_ACQUIRE>(1, varTaskIdx);
            return {varTaskCnt, varTaskIdx};
        }
    }
    return {0, ~0u};
}

void ThreadedTaskGraphExecutor::leaveSubgroup(ThreadCtx & __restrict ctx, uint32_t group_id, uint32_t subgroup_id)
{
    ctx.addEvent<Event::SUBGROUP_LEAVE>(1, subgroup_id);
    CompiledTaskGraph & __restrict graph = *graphPtr;
    CompiledTaskGraph::TaskGroup &group = graph.allGroups[group_id];
    const uint64_t indexInGroup = subgroup_id - group.subGroupsStart;
    const uint64_t subgroupBit = uint64_t(1) << indexInGroup;
    graph.allGroupsState[group_id].executing.fetch_and(~subgroupBit, std::memory_order_acq_rel);
}

bool ThreadedTaskGraphExecutor::doSubGroup(ThreadCtx & __restrict ctx, uint32_t group_id, uint32_t subgroup_id)
{
    CompiledTaskGraph & __restrict graph = *graphPtr;
    CompiledTaskGraph::TaskGroup &group = graph.allGroups[group_id];
    CompiledTaskGraph::TaskGroupState &groupState = graph.allGroupsState[group_id];
    const uint64_t indexInGroup = subgroup_id - group.subGroupsStart;
    const uint64_t subgroupBit = uint64_t(1) << indexInGroup;

    bool anyTasks = true;
    CompiledTaskGraph::TaskSubGroup& subgroup = graph.allSubGroups[subgroup_id];
    while (anyTasks)
    {
        groupState.pending.fetch_and(~subgroupBit, std::memory_order_acq_rel); // before reading task state
        anyTasks = false;
        for (uint32_t taskId = subgroup.tasksStart; taskId < subgroup.tasksEnd; taskId++)
        {
            CompiledTaskGraph::Task &task = graph.allTasks[taskId];
            if (task.state.load(std::memory_order_relaxed) != CompiledTaskGraph::Task::STATE_PENDING)
                continue;
            task.state.store(CompiledTaskGraph::Task::STATE_EXECUTING, std::memory_order_relaxed);
            anyTasks = true;
            // (optionally) wake other threads

            int taskCnt = 1;
            void* userData = task.taskUserData;
            if (task.varTask)
                taskCnt = task.varTask(userData);
            if (taskCnt < 2 || !task.allowToRunInParallelWithItself)
            {
                ctx.addEvent<Event::TASK_EXECUTE_START>(taskCnt, taskId);
                // not variadic or can't parallel with self
                if (taskCnt > 1)
                {
                    for (int i = taskCnt - 1; i >= 0; i--)
                        task.task(userData, i);
                }
                else if (taskCnt == 1) task.task(userData, 0);
                ctx.addEvent<Event::TASK_EXECUTE_END>(taskCnt, taskId);
                afterTaskDone(ctx, taskId);
            }
            else
            {
                // wind up variadic task
                CompiledTaskGraph::TaskSubGroupState &subgroupState = graph.allSubGroupsState[subgroup_id];
                assert(subgroupState.remainingVarTasks.load(std::memory_order_relaxed) == 0);
                subgroupState.remainingVarTasks.fetch_add(taskCnt, std::memory_order_relaxed);
                uint64_t expected = 0;
                subgroupState.curVarTask.compare_exchange_strong(expected, (uint64_t(taskId) << uint64_t(32u)) | uint64_t(taskCnt), std::memory_order_acq_rel);
                assert(expected == 0);
                groupState.pending.fetch_or(subgroupBit, std::memory_order_acq_rel);
                ctx.addEvent<Event::TASK_VAR_WIND_UP>(1, taskId); // after setting task state
                return false; // not owned
            }
        }
    }
    return true; // still owned
}

bool ThreadedTaskGraphExecutor::doVarTask(ThreadCtx & __restrict ctx, uint32_t subgroup_id, uint32_t task_id, uint32_t var_task_idx)
{
    CompiledTaskGraph & __restrict graph = *graphPtr;
    CompiledTaskGraph::Task &task = graph.allTasks[task_id];
    ctx.addEvent<Event::TASK_VAR_EXECUTE>(1, task_id, var_task_idx);
    ctx.addEvent<Event::TASK_EXECUTE_START>(1, task_id);
    task.task(task.taskUserData, int(var_task_idx));
    ctx.addEvent<Event::TASK_EXECUTE_END>(1, task_id);

    const int64_t remaining = graph.allSubGroupsState[subgroup_id].remainingVarTasks.fetch_sub(1, std::memory_order_acq_rel);
    assert(remaining > 0);
    bool owned = remaining == 1;
    if (owned)
        afterTaskDone(ctx, task_id);
    return owned;
}

bool ThreadedTaskGraphExecutor::afterTaskDone(ThreadCtx & __restrict ctx, uint32_t task_id)
{
    CompiledTaskGraph & __restrict graph = *graphPtr;
    CompiledTaskGraph::Task &task = graph.allTasks[task_id];
    bool anyTasksFromThisSubGroupArePending = false;

    std::vector<uint64_t> &subGroupMasksToExecuteNext = ctx.subGroupMasksToExecuteNext;
    subGroupMasksToExecuteNext.clear();
    for (auto nextTaskId : task.nextTasks)
    {
        CompiledTaskGraph::Task &nextTask = graph.allTasks[nextTaskId];
        if (nextTask.dependencies.load(std::memory_order_relaxed) != 0) // has more than 1 dependency
        {
            uint64_t deps = nextTask.dependencies.fetch_add(1, std::memory_order_acq_rel) + 1;
            uint32_t cur = uint32_t(deps);
            uint32_t target = uint32_t(deps >> uint64_t(32));
            ctx.addEvent<Event::PENDING_INC_DEPENDENCY>(1, nextTaskId, task_id, cur, target);
            if (cur != target)
                continue; // not all dependencies met yet
        }
        assert(nextTask.state.load(std::memory_order_relaxed) == CompiledTaskGraph::Task::STATE_NONE);
        nextTask.state.store(CompiledTaskGraph::Task::STATE_PENDING, std::memory_order_relaxed);
        ctx.addEvent<Event::PENDING_ADD_TASK>(1, task_id, nextTaskId);

        if (nextTask.subGroupId == task.subGroupId)
        {
            anyTasksFromThisSubGroupArePending = true;
        }
        else
        {
            subGroupMasksToExecuteNext.resize(graph.allGroups.size(), 0);
            uint32_t nextSubgroupIndexInGroup = nextTask.subGroupId - graph.allGroups[nextTask.groupId].subGroupsStart;
            subGroupMasksToExecuteNext[nextTask.groupId] |= uint64_t(1u) << uint64_t(nextSubgroupIndexInGroup);
        }
    }

    for (uint32_t groupId = 0; groupId < subGroupMasksToExecuteNext.size(); groupId++)
    {
        if (subGroupMasksToExecuteNext[groupId] != 0)
        {
            ctx.addEvent<Event::PENDING_ADD_GROUP>(1, groupId);
            graph.allGroupsState[groupId].pending.fetch_or(subGroupMasksToExecuteNext[groupId], std::memory_order_acq_rel); // after setting task state
        }
    }

    task.state.store(CompiledTaskGraph::Task::STATE_DONE, std::memory_order_relaxed);
    return anyTasksFromThisSubGroupArePending;
}


// TaskGraphExecutor

void SimpleThreadPool::windUp(int count)
{
    shutdown();
    running = true;
    threads.reserve(count);
    for (int i = 0; i < count; i++)
        threads.emplace_back(exec, this, i);
}

void SimpleThreadPool::wakeAll()
{
    condVar.notify_all();
}

void SimpleThreadPool::waitAll()
{
    wakeAll();
    for (std::thread& t: threads)
        t.join();
    threads.clear();
}

void SimpleThreadPool::shutdown()
{
    running = false;
    waitAll();
}

SimpleThreadPool::~SimpleThreadPool()
{ shutdown(); }

void SimpleThreadPool::exec(SimpleThreadPool* self, int thread_id)
{
    char threadName[128];
    sprintf_s(threadName, 128, "WorkerThread_%i", thread_id);
    OPTICK_THREAD(threadName)
    sie::logger::debug("worker", "startup %i", thread_id);
    std::vector<TaskFnPtr> taskQueue;
    while (self->running)
    {
        const ThreadedTaskGraphExecutor::ThreadResult result = self->executor->doThread(thread_id);
        if (result != ThreadedTaskGraphExecutor::ThreadResult::WAIT)
            break;
        // todo: yield

        // std::unique_lock lock(self->condVarMutex);
        // self->condVar.wait(lock, [] { return true; });
    }
    sie::logger::debug("worker", "shutdown %i", thread_id);
}

}