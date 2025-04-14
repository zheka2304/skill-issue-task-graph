#include "task_graph_execute.h"

#include <cassert>
#include "logger.h"
#include "optick.h"


#define SI_TG_PROFILE_INTERNAL(...) OPTICK_EVENT(__VA_ARGS__)
#define SI_TG_PROFILE_EXCESSIVE(...) // OPTICK_EVENT(__VA_ARGS__)

#define TP_VERBOSE(...) // debug("exec", __VA_ARGS__)
#define TP_SKIP_EXECUTION 0


namespace si::tg
{

using sie::logger::debug;

void ThreadedTaskGraphExecutor::prepareForExecution(int thread_num)
{
    SI_TG_PROFILE_INTERNAL("prepare_execution")
    sleepingThreadsMask.store(0, std::memory_order_relaxed);
    allDoneEventPending.store(true, std::memory_order_relaxed);
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
    sleepingThreadsMask.fetch_and(~(uint64_t(1) << uint64_t(thread_id)), std::memory_order_relaxed);

    CompiledTaskGraph & __restrict graph = *graphPtr;
    const uint32_t maxFailedGroups = graph.allGroups.size() + 1;
    const uint32_t maxFailedSubGroups = 64;

    ctx.failedGroups = 0;
    while (ctx.failedGroups < maxFailedGroups)
    {
        ctx.failedSubgroups = 0;
        ctx.addEvent<Event::THREAD_START_GROUP>(1, ctx.groupId);
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
            SI_TG_PROFILE_EXCESSIVE("do_subgroup")
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
    if (minState == CompiledTaskGraph::Task::STATE_DONE && allDoneEventPending.exchange(false, std::memory_order_relaxed))
    {
        ctx.addEvent<Event::THREAD_EXIT>(1);
        return ThreadResult::ALL_DONE;
    }
    if (minState >= CompiledTaskGraph::Task::STATE_EXECUTING)
    {
        ctx.addEvent<Event::THREAD_EXIT>(1);
        return ThreadResult::EXIT;
    }
    ctx.addEvent<Event::THREAD_WAIT>(1);
    if (wake_threads)
        sleepingThreadsMask.fetch_or(uint64_t(1) << uint64_t(thread_id), std::memory_order_relaxed);
    return ThreadResult::WAIT;
}

bool ThreadedTaskGraphExecutor::doGroupUntilSubgroupEnter(ThreadCtx & __restrict ctx, bool allow_var_tasks)
{
    // SI_TG_PROFILE_INTERNAL("do_group_next")
    CompiledTaskGraph & __restrict graph = *graphPtr;
    const uint32_t subGroupsStartIdx = graph.allGroups[ctx.groupId].subGroupsStart;
    uint64_t pending = graph.allGroupsState[ctx.groupId].pending.load(std::memory_order_relaxed);
    if (pending == 0)
    {
        ctx.addEvent<Event::THREAD_NOTHING_PENDING>(1, ctx.groupId);
        return false; // move to next group
    }
    ctx.executingMask = graph.allGroupsState[ctx.groupId].executing.load(std::memory_order_relaxed);
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

void ThreadedTaskGraphExecutor::validateAllDone() const
{
    for (const ThreadCtx &ctx : threadCtxArray)
        SI_TG_ASSERT(!ctx.isSubgroupOwned);
    for (const CompiledTaskGraph::Task &task : graphPtr->allTasks)
        SI_TG_ASSERT(task.state.load(std::memory_order_relaxed) == CompiledTaskGraph::Task::STATE_DONE);
}

uint64_t ThreadedTaskGraphExecutor::gatherThreadsToWake()
{
    const uint64_t sleepingMask = sleepingThreadsMask.load(std::memory_order_relaxed);
    if (sleepingMask == 0) // no sleeping threads
        return 0;
    SI_TG_PROFILE_EXCESSIVE("gatherThreadsToWake")
    CompiledTaskGraph & __restrict graph = *graphPtr;
    const uint32_t groupsCnt = graph.allGroups.size();
    int estimatedJobCnt = 0;
    for (uint32_t groupId = 0; groupId < groupsCnt; groupId++)
    {
        uint64_t pending = graph.allGroupsState[groupId].pending.load(std::memory_order_relaxed);
        if (pending == 0)
            continue;
        const uint32_t subgroupsStart = graph.allGroups[groupId].subGroupsStart;
        uint64_t executing = 0; // don't fetch current value, assume current jobs will finish
        internal::iter_set_bits(pending, [&] (uint64_t bit_idx) {
            uint32_t subgroupIdx = subgroupsStart + bit_idx;
            if (graph.allSubGroups[subgroupIdx].excludedMask & executing)
                return;
            estimatedJobCnt++;
            executing |= uint64_t(1) << bit_idx;
        });
    }
    int estimatedActiveThreads = int(threadCtxArray.size()) - int(internal::count_set_bits(sleepingMask));
    SI_TG_ASSERT(estimatedActiveThreads > 0);
    estimatedJobCnt -= estimatedActiveThreads;
    if (estimatedJobCnt <= 0)
        return 0;
    return sleepingMask;
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
    SI_TG_PROFILE_INTERNAL("do_subgroup");
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
            anyTasks = true;

            // subgraph task
            if (task.subgraphDataIdx != -1)
            {
                doSubGraphTask(ctx, taskId);
                continue;
            }

            task.state.store(CompiledTaskGraph::Task::STATE_EXECUTING, std::memory_order_relaxed);

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
                SI_TG_ASSERT(subgroupState.remainingVarTasks.load(std::memory_order_relaxed) == 0);
                subgroupState.remainingVarTasks.store(taskCnt, std::memory_order_relaxed);
                SI_TG_ASSERT(subgroupState.curVarTask.load(std::memory_order_relaxed) == 0);
                subgroupState.curVarTask.store((uint64_t(taskId) << uint64_t(32u)) | uint64_t(taskCnt), std::memory_order_release);
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
    SI_TG_ASSERT(remaining > 0);
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
    SI_TG_ASSERT(task_id == subgraphEntryTask || task_id == subgraphExitTask);
    SI_TG_ASSERT(!task.allowToRunInParallelWithItself);

    const auto resetTaskStateAndDeps = [&] (uint32_t sg_task_id, uint8_t expected_state)
    {
        CompiledTaskGraph::Task &subGraphTask = graph.allTasks[sg_task_id];
        SI_TG_ASSERT(sg_task_id != subgraphEntryTask);
        uint8_t curState = subGraphTask.state.load(std::memory_order_relaxed);
        SI_TG_ASSERT(curState == expected_state);
        uint64_t deps = subGraphTask.dependencies.load(std::memory_order_relaxed);
        deps >>= uint64_t(32u);
        deps <<= uint64_t(32u);
        subGraphTask.dependencies.store(deps, std::memory_order_relaxed);
        subGraphTask.state.store(CompiledTaskGraph::Task::STATE_NONE, std::memory_order_relaxed);
    };
    const auto resetSubgraphTasks = [&] {
        for (int i = 0; i < cnt; i++)
            resetTaskStateAndDeps(graph.subGraphData[idx + i], CompiledTaskGraph::Task::STATE_DONE);
        resetTaskStateAndDeps(subgraphExitTask, CompiledTaskGraph::Task::STATE_PENDING);
    };

    if (task_id == subgraphEntryTask)
    {
        SI_TG_PROFILE_INTERNAL("subgraph_entry");
        if (remaining < 0)
        {
            if (remaining == -2)
                resetSubgraphTasks();
            remaining = task.taskData.taskVarFn ? task.taskData.taskVarFn(task.taskData.userData) : 1;
            if (remaining > 0)
                ctx.addEvent<Event::SUBGRAPH_ENTER>(1, subgraphEntryTask);
            else
                ctx.addEvent<Event::SUBGRAPH_SKIP>(1, subgraphEntryTask);
            SI_TG_ASSERT(remaining >= 0);
        }
        graph.allTasks[subgraphEntryTask].state.store(CompiledTaskGraph::Task::STATE_DONE, std::memory_order_relaxed);
        if (remaining == 0) // subgraph is not required to run
        {
            SI_TG_ASSERT(0 && "not implemented");
            SI_TG_ASSERT(graph.allTasks[subgraphExitTask].state.load(std::memory_order_relaxed) == CompiledTaskGraph::Task::STATE_NONE);
            graph.allTasks[subgraphExitTask].state.store(CompiledTaskGraph::Task::STATE_DONE, std::memory_order_relaxed);
            afterTaskDone(ctx, subgraphExitTask);
        }
        else
        {
            remaining--;
            afterTaskDone(ctx, subgraphEntryTask);
        }
    }
    else
    {
        SI_TG_PROFILE_INTERNAL("subgraph_exit");
        SI_TG_ASSERT(remaining >= 0);
        SI_TG_ASSERT(task_id == subgraphExitTask);
        if (remaining == 0)
        {
            remaining = -2; // signal to reset both remaining and this graph before next start
            graph.allTasks[subgraphExitTask].state.store(CompiledTaskGraph::Task::STATE_DONE, std::memory_order_relaxed);
            ctx.addEvent<Event::SUBGRAPH_EXIT>(1, task_id);
            afterTaskDone(ctx, subgraphExitTask); // graph is done
            return;
        }
        ctx.addEvent<Event::SUBGRAPH_RESTART>(1, subgraphExitTask);
        SI_TG_ASSERT(graph.allTasks[subgraphEntryTask].state.load(std::memory_order_relaxed) == CompiledTaskGraph::Task::STATE_DONE);
        resetSubgraphTasks();

        // set entry task as pending
        CompiledTaskGraph::Task &entryTask = graph.allTasks[subgraphEntryTask];
        ctx.addEvent<Event::PENDING_ADD_TASK>(1, subgraphExitTask, subgraphEntryTask);
        entryTask.state.store(CompiledTaskGraph::Task::STATE_PENDING, std::memory_order_relaxed);
        const uint64_t nextSubgroupIndexInGroup = entryTask.subGroupId - graph.allGroups[entryTask.groupId].subGroupsStart;
        graph.allGroupsState[entryTask.groupId].pending.fetch_or(uint64_t(1) << nextSubgroupIndexInGroup, std::memory_order_acq_rel);
        ctx.addEvent<Event::PENDING_ADD_GROUP>(1, entryTask.groupId);
    }
}

bool ThreadedTaskGraphExecutor::afterTaskDone(ThreadCtx & __restrict ctx, uint32_t task_id)
{
    CompiledTaskGraph & __restrict graph = *graphPtr;
    CompiledTaskGraph::Task &task = graph.allTasks[task_id];
    bool anyTasksFromThisSubGroupArePending = false;

    task.state.store(CompiledTaskGraph::Task::STATE_DONE, std::memory_order_relaxed);

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
            SI_TG_ASSERT(cur <= target);
            if (cur != target)
                continue; // not all dependencies met yet
        }
        SI_TG_ASSERT(nextTask.state.load(std::memory_order_relaxed) == CompiledTaskGraph::Task::STATE_NONE);
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

    return anyTasksFromThisSubGroupArePending;
}


// TaskGraphExecutor

void SimpleThreadPool::windUpThreads(int thread_num)
{
    shutdownThreads();
    running = true;
    threads.reserve(thread_num);
    for (int i = 0; i < thread_num; i++)
        threads.emplace_back(exec, this, i);
    waitDone();
}

void SimpleThreadPool::shutdownThreads()
{
    running = false;
    if (threads.empty())
        return;
    wakeAll();
    for (std::thread& t: threads)
        t.join();
    threads.clear();
}

void SimpleThreadPool::execute(si::tg::CompiledTaskGraph* graph)
{
    doneEvent.word.store(0, std::memory_order_relaxed);
    executor.graphPtr = graph;
#if !TP_SKIP_EXECUTION
    executor.prepareForExecution(threads.size());
#endif
    wakeAll();
    waitDone();
#if !TP_SKIP_EXECUTION
    executor.validateAllDone();
#endif
}

void SimpleThreadPool::waitDone()
{
    TP_VERBOSE("all done - wait start");
    doneEvent.waitMask((uint64_t(1) << uint64_t(threads.size())) - 1);
    TP_VERBOSE("all done - wait end");
}

SimpleThreadPool::~SimpleThreadPool() { shutdownThreads(); }

thread_local int SimpleThreadPool::thisThreadId = -1;

void SimpleThreadPool::doThread(int thread_id)
{
    thisThreadId = thread_id;
    // sie::logger::debug("worker", "startup %i", thread_id);
    SI_TG_PROFILE_INTERNAL("worker_thread");
    while (running)
    {
        {
            // debug("exec", "[%i] idle started %u", thread_id, uint32_t(wakeEvent.wakeMask.load()));
            SI_TG_PROFILE_INTERNAL("thread_idle");
            TP_VERBOSE("[%i] thread wait", thread_id);
            doneEvent.wakeThread(thread_id);
            idleEvent.waitThread(thread_id);
            TP_VERBOSE("[%i] thread wake", thread_id);
            // debug("exec", "[%i] idle ended", thread_id);
        }
        while (running)
        {
            constexpr bool USE_WAIT = false;
            const auto wakeThreadsFn = [this](uint64_t mask) {
                SI_TG_PROFILE_EXCESSIVE("wake_threads");
                internal::iter_set_bits(mask, [&](uint32_t i) {
                    // sie::logger::debug("worker", "[%i] wake", int(i));
                });
                wakeEvent.wakeMask(mask);
            };

#if !TP_SKIP_EXECUTION
            bool end = false;
            constexpr int MAX_ATTEMPTS = 32;
            for (int i = 0; i < MAX_ATTEMPTS; i++)
            {
                const ThreadedTaskGraphExecutor::ThreadResult result = executor.doThread(thread_id,
                USE_WAIT ? WakeThreadsCallback(wakeThreadsFn) : WakeThreadsCallback());
                end = result != ThreadedTaskGraphExecutor::ThreadResult::WAIT;
                if (end)
                    break;
                // SI_TG_PROFILE_EXCESSIVE("thread_yield");
                // std::this_thread::yield();
            }

            if (end)
#endif
                break;

            if (USE_WAIT)
            {
                SI_TG_PROFILE_INTERNAL("thread_wait");
                // sie::logger::debug("worker", "[%i] wait start", int(thread_id));
                wakeEvent.waitThread(thread_id);
                // sie::logger::debug("worker", "[%i] wait end", int(thread_id));
            }
        }
    }
    // sie::logger::debug("worker", "shutdown %i", thread_id);
    thisThreadId = -1;
}

void SimpleThreadPool::wakeAll()
{
    SI_TG_ASSERT(doneEvent.word == 0);
    TP_VERBOSE("wake all");
    idleEvent.wakeMask((uint64_t(1) << uint64_t(threads.size())) - 1);
}

void SimpleThreadPool::exec(SimpleThreadPool* self, int thread_id)
{
    char threadName[128];
    sprintf_s(threadName, 128, "WorkerThread_%i", thread_id);
    OPTICK_THREAD(threadName)
    self->doThread(thread_id);
}

void SimpleThreadPool::CondVar::wakeMask(uint64_t mask)
{
    if (!mask)
        return;
    mutex.lock();
    word.fetch_or(mask, std::memory_order_relaxed);
    mutex.unlock();
    condVar.notify_all();
}

void SimpleThreadPool::CondVar::waitThread(int thread_id)
{
    waitMask(uint64_t(1) << uint64_t(thread_id));
}

void SimpleThreadPool::CondVar::waitMask(uint64_t mask)
{
    std::unique_lock lock(mutex);
    waitMaskImpl(lock, mask);
}

void SimpleThreadPool::CondVar::waitMaskImpl(std::unique_lock<std::mutex> &lock, uint64_t mask)
{
    condVar.wait(lock, [&] {
        uint64_t w = word.load(std::memory_order_relaxed);
        if ((w & mask) != mask)
            return false;
        while (!word.compare_exchange_strong(w, w & ~mask)) { SI_TG_ASSERT((w & mask) == mask); }
        return true;
    });
}

}