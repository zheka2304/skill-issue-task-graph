#include "task_graph_execute.h"

#include <cassert>
#include "logger.h"
#include "optick.h"


namespace si::tg
{

using sie::logger::debug;

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
    for (int32_t idx = 0; idx < graph.subGraphData.size(); )
    {
        int32_t cnt = graph.subGraphData[idx++];
        idx++;
        idx++;
        graph.subGraphData[idx++] = -1; // reset remaining
        idx += cnt;
    }
    for (int groupId = 0; groupId < graph.allGroups.size(); groupId++)
        graph.allGroupsState[groupId].pending.store(graph.allGroups[groupId].initialPending, std::memory_order_relaxed);
#if SI_TG_ENABLE_DEBUG_TIMED_EVENTS
    curTimedEventIdx.store(0, std::memory_order_relaxed);
#endif
}

ThreadedTaskGraphExecutor::ThreadResult ThreadedTaskGraphExecutor::doThread(int thread_id, const WakeThreadsCallback &wake_threads)
{
    ThreadCtx & __restrict ctx = threadCtxArray[thread_id];
    if (wake_threads)
    {
        OPTICK_EVENT("enter_thread")
        while (true)
        {
            uint32_t state;
            while ((state = ctx.ownershipLock.state.load(std::memory_order_relaxed)) == ThreadOwnershipLock::STATE_OWNED)
                ;
            if (ctx.ownershipLock.state.compare_exchange_strong(state, ThreadOwnershipLock::STATE_OWNED, std::memory_order_acq_rel))
                break;
        }
    }

    CompiledTaskGraph & __restrict graph = *graphPtr;
    const uint32_t maxFailedGroups = graph.allGroups.size() + 1;
    const uint32_t maxFailedSubGroups = 64;

    const uint64_t thisThreadMaskBit = uint64_t(1) << uint64_t(thread_id);
    ctx.failedGroups = 0;
    while (ctx.failedGroups < maxFailedGroups)
    {
        ctx.failedSubgroups = 0;
        ctx.addEvent<Event::THREAD_START_GROUP>(1, ctx.groupId);
        ctx.executingMask = graph.allGroupsState[ctx.groupId].executing.load(std::memory_order_relaxed);
        while (ctx.failedSubgroups < maxFailedSubGroups)
        {
            if (!ctx.isSubgroupOwned && !doGroupUntilSubgroupEnter(ctx, true))
            {
                ctx.failedSubgroups = maxFailedSubGroups;
                break; // nothing pending
            }
            if (!ctx.isSubgroupOwned)
                continue;
            if (wake_threads)
            {
                uint64_t wakeMask = gatherThreadsToWake();
                if (wakeMask)
                    wake_threads(wakeMask);
            }
            OPTICK_EVENT("do_subgroup")
            ctx.isSubgroupOwned = doSubGroup(ctx, ctx.groupId, ctx.subgroupId);
            if (ctx.isSubgroupOwned)
                leaveSubgroup(ctx, ctx.groupId, ctx.subgroupId);
            ctx.isSubgroupOwned = false;
        }
        if (ctx.failedSubgroups >= maxFailedSubGroups)
        {
            ctx.failedGroups++;
            ctx.groupId++;
            ctx.groupId %= graph.allGroups.size();
            continue;
        }
    }
    SI_TG_ASSERT(!ctx.isSubgroupOwned);

    uint8_t minState = CompiledTaskGraph::Task::STATE_DONE;
    for (const CompiledTaskGraph::Task &task : graph.allTasks)
        minState = std::min<uint8_t>(minState, task.state.load(std::memory_order_relaxed));
    if (minState == CompiledTaskGraph::Task::STATE_DONE)
    {
        ctx.addEvent<Event::THREAD_EXIT>(1);
        ctx.ownershipLock.state.store(ThreadOwnershipLock::STATE_NONE, std::memory_order_release);
        return ThreadResult::ALL_DONE;
    }
    if (minState == CompiledTaskGraph::Task::STATE_EXECUTING)
    {
        ctx.addEvent<Event::THREAD_EXIT>(1);
        ctx.ownershipLock.state.store(ThreadOwnershipLock::STATE_NONE, std::memory_order_release);
        return ThreadResult::EXIT;
    }
    ctx.addEvent<Event::THREAD_WAIT>(1);
    if (wake_threads)
        ctx.ownershipLock.state.store(ThreadOwnershipLock::STATE_WAITING, std::memory_order_release);
    return ThreadResult::WAIT;
}

bool ThreadedTaskGraphExecutor::doGroupUntilSubgroupEnter(ThreadCtx & __restrict ctx, bool allow_var_tasks)
{
    // OPTICK_EVENT("do_group_next")
    CompiledTaskGraph & __restrict graph = *graphPtr;
    const uint32_t subGroupsStartIdx = graph.allGroups[ctx.groupId].subGroupsStart;
    uint64_t pending = graph.allGroupsState[ctx.groupId].pending.load(std::memory_order_relaxed);
    if (pending == 0)
    {
        ctx.addEvent<Event::THREAD_NOTHING_PENDING>(1, ctx.groupId);
        return false; // move to next group
    }
    internal::BitIter bitIter(pending);
    while (bitIter.step())
    {
        ctx.subgroupId = subGroupsStartIdx + bitIter.idx();
        ctx.isSubgroupOwned = tryEnterSubgroup(ctx, ctx.groupId, ctx.subgroupId, ctx.executingMask, false);
        if (!ctx.isSubgroupOwned && allow_var_tasks)
        {
            // only try acquiring var task, if this group is executing
            if ((ctx.executingMask >> uint64_t(bitIter.idx())) & uint64_t(1u))
            {
                const auto [varTaskCnt, varTaskId] = tryAcquireVarTask(ctx, ctx.groupId, ctx.subgroupId);
                if (varTaskCnt > 0 && doVarTask(ctx, ctx.subgroupId, varTaskId, varTaskCnt - 1))
                    ctx.isSubgroupOwned = true;
                //if (varTaskCnt > 2)
                //    wake_threads(getWakeThreadMask(varTaskCnt - 2));
            }
        }
        if (!ctx.isSubgroupOwned)
        {
            ctx.failedSubgroups++;
            continue;
        }
        ctx.failedGroups = 0;
        ctx.failedSubgroups = 0;
        break;
    }
    return true;
}

uint64_t ThreadedTaskGraphExecutor::gatherThreadsToWake()
{
    OPTICK_EVENT("doGatherSleepingThreads")
    uint64_t wakeThreadsMask = 0;
    bool keepGoing = true;
    const uint32_t groupsCnt = graphPtr->allGroups.size();
    while (keepGoing)
    {
        keepGoing = false;
        for (ThreadCtx &ctx : threadCtxArray)
        {
            if ((wakeThreadsMask >> uint64_t(ctx.threadId)) & 1u)
                continue;
            uint32_t state = ctx.ownershipLock.state.load(std::memory_order_relaxed);
            if (state != ThreadOwnershipLock::STATE_WAITING)
                continue;
            if (!ctx.ownershipLock.state.compare_exchange_strong(state, ThreadOwnershipLock::STATE_OWNED, std::memory_order_acq_rel))
                continue;
            for (uint32_t i = 0; i < groupsCnt && !ctx.isSubgroupOwned; i++)
            {
                ctx.failedSubgroups = 0;
                doGroupUntilSubgroupEnter(ctx, false);
                if (ctx.isSubgroupOwned)
                {
                    wakeThreadsMask |= uint64_t(1) << uint64_t(ctx.threadId);
                    keepGoing = true;
                    break;
                }
                ctx.groupId = (ctx.groupId + 1) % groupsCnt;
            }
            ctx.ownershipLock.state.store(ctx.isSubgroupOwned ? ThreadOwnershipLock::STATE_NONE : ThreadOwnershipLock::STATE_WAITING, std::memory_order_acq_rel);
        }
    }
    return wakeThreadsMask;
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

            // subgraph task
            if (task.subgraphDataIdx != -1)
            {
                doSubGraphTask(ctx, taskId);
                continue;
            }

            // (optionally) wake other threads

            int taskCnt = 1;
            void* userData = task.taskData.userData;
            if (task.taskData.taskVarFn)
                taskCnt = task.taskData.taskVarFn(userData);
            if (taskCnt < 2 || !task.allowToRunInParallelWithItself)
            {
                ctx.addEvent<Event::TASK_EXECUTE_START>(taskCnt, taskId);
                // not variadic or can't parallel with self
                if (taskCnt > 1)
                {
                    for (int i = taskCnt - 1; i >= 0; i--)
                        task.taskData.taskFn(userData, i);
                }
                else if (taskCnt == 1) task.taskData.taskFn(userData, 0);
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
    task.taskData.taskFn(task.taskData.userData, int(var_task_idx));
    ctx.addEvent<Event::TASK_EXECUTE_END>(1, task_id);

    const int64_t remaining = graph.allSubGroupsState[subgroup_id].remainingVarTasks.fetch_sub(1, std::memory_order_acq_rel);
    assert(remaining > 0);
    bool owned = remaining == 1;
    if (owned)
        afterTaskDone(ctx, task_id);
    return owned;
}

void ThreadedTaskGraphExecutor::doSubGraphTask(ThreadCtx& ctx, uint32_t task_id)
{
    CompiledTaskGraph & __restrict graph = *graphPtr;
    CompiledTaskGraph::Task &task = graph.allTasks[task_id];
    int32_t idx = task.subgraphDataIdx;
    int32_t cnt = graph.subGraphData[idx++];
    int32_t subgraphEntryTask = graph.subGraphData[idx++];
    int32_t subgraphExitTask = graph.subGraphData[idx++];
    int32_t &remaining = graph.subGraphData[idx++];
    assert(task_id == subgraphEntryTask || task_id == subgraphExitTask);
    assert(!task.allowToRunInParallelWithItself);

    const auto resetTaskStateAndDeps = [&] (uint32_t sg_task_id)
    {
        CompiledTaskGraph::Task &subGraphTask = graph.allTasks[sg_task_id];
        assert (subGraphTask.state.load(std::memory_order_relaxed) == CompiledTaskGraph::Task::STATE_DONE);
        uint64_t deps = subGraphTask.dependencies.load(std::memory_order_relaxed);
        deps >>= uint64_t(32u);
        deps <<= uint64_t(32u);
        subGraphTask.dependencies.store(deps, std::memory_order_relaxed);
        subGraphTask.state.store(CompiledTaskGraph::Task::STATE_NONE, std::memory_order_relaxed);
    };
    const auto resetSubgraphTasks = [&] {
        for (int i = 0; i < cnt; i++)
            resetTaskStateAndDeps(graph.subGraphData[idx + i]);
        resetTaskStateAndDeps(subgraphExitTask);
    };

    if (task_id == subgraphEntryTask)
    {
        OPTICK_EVENT("subgraph_entry");
        if (remaining < 0)
        {
            if (remaining == -2)
                resetSubgraphTasks();
            remaining = task.taskData.taskVarFn ? task.taskData.taskVarFn(task.taskData.userData) : 1;
            if (remaining > 0)
                ctx.addEvent<Event::SUBGRAPH_ENTER>(1, task_id);
            else
                ctx.addEvent<Event::SUBGRAPH_SKIP>(1, task_id);
            assert(remaining >= 0);
        }
        graph.allTasks[task_id].state.store(CompiledTaskGraph::Task::STATE_DONE, std::memory_order_relaxed);
        if (remaining == 0) // subgraph is not required to run
        {
            assert(task.state.load(std::memory_order_relaxed) == CompiledTaskGraph::Task::STATE_NONE);
            graph.allTasks[subgraphExitTask].state.store(CompiledTaskGraph::Task::STATE_DONE, std::memory_order_relaxed);
            afterTaskDone(ctx, task_id);
        }
        else
        {
            remaining--;
            afterTaskDone(ctx, task_id);
        }
    }
    else
    {
        OPTICK_EVENT("subgraph_exit");
        assert(remaining >= 0);
        graph.allTasks[task_id].state.store(CompiledTaskGraph::Task::STATE_DONE, std::memory_order_relaxed);
        if (remaining == 0)
        {
            remaining = -2; // signal to reset both remaining and this graph before next start
            ctx.addEvent<Event::SUBGRAPH_EXIT>(1, task_id);
            afterTaskDone(ctx, task_id); // graph is done
            return;
        }
        resetSubgraphTasks();

        // set entry task as pending
        CompiledTaskGraph::Task &entryTask = graph.allTasks[subgraphEntryTask];
        entryTask.state.store(CompiledTaskGraph::Task::STATE_PENDING, std::memory_order_relaxed);
        ctx.addEvent<Event::PENDING_ADD_TASK>(1, task_id, subgraphEntryTask);
        const uint64_t nextSubgroupIndexInGroup = entryTask.subGroupId - graph.allGroups[entryTask.groupId].subGroupsStart;
        graph.allGroupsState[entryTask.groupId].pending.fetch_or(uint64_t(1) << nextSubgroupIndexInGroup, std::memory_order_acq_rel);
        ctx.addEvent<Event::PENDING_ADD_GROUP>(1, entryTask.groupId);
        ctx.addEvent<Event::SUBGRAPH_RESTART>(1, task_id);
    }
}

bool ThreadedTaskGraphExecutor::afterTaskDone(ThreadCtx & __restrict ctx, uint32_t task_id)
{
    CompiledTaskGraph & __restrict graph = *graphPtr;
    CompiledTaskGraph::Task &task = graph.allTasks[task_id];
    bool anyTasksFromThisSubGroupArePending = false;

    std::vector<uint64_t> &subGroupMasksToExecuteNext = ctx.subGroupMasksToExecuteNext;
    subGroupMasksToExecuteNext.clear();
    for (uint32_t i = task.nextTasksStart; i < task.nextTasksEnd; i++)
    {
        uint32_t nextTaskId = graph.nextTaskIds[i];
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
            uint64_t nextSubgroupIndexInGroup = nextTask.subGroupId - graph.allGroups[nextTask.groupId].subGroupsStart;
            subGroupMasksToExecuteNext[nextTask.groupId] |= uint64_t(1u) << nextSubgroupIndexInGroup;
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
    wakeThreadsMask.store(0, std::memory_order_relaxed);
    for (int i = 0; i < count; i++)
        threads.emplace_back(exec, this, i);
}

void SimpleThreadPool::wakeAll()
{
    wakeThreadsMask.store(~uint64_t(0), std::memory_order_relaxed);
    condVar.notify_all();
}

void SimpleThreadPool::waitAll()
{
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
    // sie::logger::debug("worker", "startup %i", thread_id);
    OPTICK_EVENT("worker_thread");
    while (self->running)
    {
        constexpr bool USE_WAIT = true;
        const auto wakeThreadsFn = [self] (uint64_t mask) {
            OPTICK_EVENT("wake_threads");
            self->wakeThreadsMask.fetch_or(mask, std::memory_order_relaxed);
            internal::iter_set_bits(mask, [&] (uint32_t i) {
                // sie::logger::debug("worker", "[%i] wake", int(i));
            });
            self->condVar.notify_all();
        };

        const ThreadedTaskGraphExecutor::ThreadResult result = self->executor->doThread(thread_id, USE_WAIT ? WakeThreadsCallback(wakeThreadsFn) : WakeThreadsCallback());/*,);*/
        if (result == ThreadedTaskGraphExecutor::ThreadResult::ALL_DONE)
            self->wakeAll();
        if (result != ThreadedTaskGraphExecutor::ThreadResult::WAIT)
            break;

        if (USE_WAIT)
        {
            OPTICK_EVENT("thread_wait");
            //sie::logger::debug("worker", "[%i] wait start", int(thread_id));
            std::unique_lock lock(self->condVarMutex);
            uint64_t threadIndexBit = uint64_t(1u) << uint64_t(thread_id);
            self->condVar.wait(lock, [&] {
                return !self->running || bool(self->wakeThreadsMask.load(std::memory_order_relaxed) & threadIndexBit);
            });
            self->wakeThreadsMask.fetch_and(~threadIndexBit, std::memory_order_relaxed);
            //sie::logger::debug("worker", "[%i] wait end", int(thread_id));
        }
    }
    // sie::logger::debug("worker", "shutdown %i", thread_id);
}

}