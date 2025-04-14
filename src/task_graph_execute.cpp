#include <taskgraph/task_graph_execute.h>


namespace si::tg::internal
{

// internal::BitIter iter(word);
// while (iter.step())
//   iter.idx();
struct BitIter
{
    explicit BitIter(uint64_t w) : word(w), idx_(-1) {}

    bool step()
    {
        if (idx_ >= 63)
            return false;
        idx_++;
        if ((word >> idx_) == 0)
            return false;
#ifdef SI_TG_FIRST_SET_BIT
        idx_ += SI_TG_FIRST_SET_BIT(word >> idx_);
#else
        while (word && (((word >> idx_) & 1u) == 0))
            idx_++;
#endif
        return true;
    }

    uint64_t idx() const { return idx_ < 0 ? 0 : idx_; }
    void setWord(uint64_t w) { word = w; }

private:
    uint64_t word;
    int64_t idx_;
};

}

namespace si::tg
{

template<ThreadedTaskGraphExecutor::Event Evt, typename... Args>
void ThreadedTaskGraphExecutor::ThreadCtx::addEvent(int64_t v, Args&& ... args)
{
    (void) v;
    ((void) args, ...);
#if SI_TG_ENABLE_DEBUG_STAT_EVENTS
    eventCnt[int(Evt)] += v;
#endif
#if SI_TG_ENABLE_DEBUG_LOG_EVENTS
    switch(Evt)
    {
#define CASE_EVENT(X) case Event::X: break;
        SI_TG_DEBUG_LOG_SKIP_EVENTS_LIST(CASE_EVENT)
#undef CASE_EVENT
        default:
            if (sizeof...(args) == 0)
                internal::log_debug("[%i] %s\n", threadId, EVENT_NAMES[int(Evt)], int(args)...);
            else if (sizeof...(args) == 1)
                internal::log_debug("[%i] %s %i\n", threadId, EVENT_NAMES[int(Evt)], int(args)...);
            else if (sizeof...(args) == 2)
                internal::log_debug("[%i] %s %i %i\n", threadId, EVENT_NAMES[int(Evt)], int(args)...);
            else if (sizeof...(args) == 3)
                internal::log_debug("[%i] %s %i %i %i\n", threadId, EVENT_NAMES[int(Evt)], int(args)...);
            else if (sizeof...(args) == 4)
                internal::log_debug("[%i] %s %i %i %i %i\n", threadId, EVENT_NAMES[int(Evt)], int(args)...);
            break;
    }
#endif
#if SI_TG_ENABLE_DEBUG_TIMED_EVENTS
    switch(Evt)
    {
        case Event::NUM:
#define CASE_EVENT(X) case Event::X:
        SI_TG_DEBUG_LOG_SKIP_EVENTS_LIST(CASE_EVENT)
#undef CASE_EVENT
        {
            timedEvents.push_back(TimedEvent{ Evt, executor->curTimedEventIdx.fetch_add(1, std::memory_order_relaxed), uint32_t(args)... });
            break;
        }
    }
#endif
}

ThreadedTaskGraphExecutor::ThreadCtx::~ThreadCtx()
{
    SI_TG_ASSERT(!isSubgroupOwned);
}

uint32_t ThreadedTaskGraphExecutor::ThreadCtx::nextRnd()
{
    uint32_t x = rndSeed;
    if (x == 0)
        x++;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rndSeed = x;
}

void ThreadedTaskGraphExecutor::getAllTimedEvents([[maybe_unused]] Vector<TimedEvent> &all) const
{
#if SI_TG_ENABLE_DEBUG_TIMED_EVENTS
    for (const ThreadCtx &ctx : threadCtxArray)
            for (const TimedEvent &evt : ctx.timedEvents)
                all.push_back(evt);
        std::sort(all.begin(), all.end());
#endif
}

void ThreadedTaskGraphExecutor::getEventCountStats([[maybe_unused]] Array<int64_t, uint8_t(Event::NUM)> &stats) const
{
#if SI_TG_ENABLE_DEBUG_STAT_EVENTS
    for (const ThreadCtx &ctx : threadCtxArray)
            for (int i = 0; i < uint8_t(Event::NUM); i++)
                stats[i] += ctx.eventCnt[i];
#endif
}


// ThreadedTaskGraphExecutor - execution


void ThreadedTaskGraphExecutor::prepareForExecution(int thread_num)
{
    SI_TG_ASSERT(graphPtr);
    SI_TG_ASSERT(graphPtr->isValid);
    SI_TG_PROFILE_INTERNAL("prepare_execution")
    sleepingThreadsMask.store(0, std::memory_order_relaxed);
    threadCtxArray.resize(0);
    threadCtxArray.resize(thread_num);
    for (int i = 0; i < int(threadCtxArray.size()); i++)
    {
        threadCtxArray[i].executor = this;
        threadCtxArray[i].threadId = i;
    }
    CompiledTaskGraph & SI_TG_RESTRICT graph = *graphPtr;
    for (CompiledTaskGraph::Task &task : graph.allTasks)
    {
        task.state.store(task.isPendingOnStart ? CompiledTaskGraph::Task::STATE_PENDING : CompiledTaskGraph::Task::STATE_NONE, std::memory_order_relaxed);
        uint64_t deps = task.dependencies.load(std::memory_order_relaxed);
        deps >>= uint64_t(32u);
        deps <<= uint64_t(32u);
        task.dependencies.store(deps, std::memory_order_relaxed);
    }
    for (uint32_t idx = 0; idx < graph.subGraphData.size(); )
    {
        int32_t cnt = graph.subGraphData[idx++];
        idx++;
        idx++;
        graph.subGraphData[idx++] = -1; // reset remaining
        idx += uint32_t(cnt);
    }
    for (uint32_t groupId = 0; groupId < graph.allGroups.size(); groupId++)
        graph.allGroupsState[groupId].pending.store(graph.allGroups[groupId].initialPending, std::memory_order_relaxed);
#if SI_TG_ENABLE_DEBUG_TIMED_EVENTS
    curTimedEventIdx.store(0, std::memory_order_relaxed);
#endif
}

void ThreadedTaskGraphExecutor::setWakeCallback(int thread_id, si::tg::WakeThreadsCallback wake_callback)
{
    threadCtxArray[thread_id].wakeCb = std::move(wake_callback);
}

ThreadedTaskGraphExecutor::ThreadResult ThreadedTaskGraphExecutor::doThread(int thread_id)
{
    ThreadCtx & SI_TG_RESTRICT ctx = threadCtxArray[thread_id];
    const bool hasWakeCb = bool(ctx.wakeCb);
    if (hasWakeCb)
        sleepingThreadsMask.fetch_and(~(uint64_t(1) << uint64_t(thread_id)), std::memory_order_relaxed);

    CompiledTaskGraph & SI_TG_RESTRICT graph = *graphPtr;
    const uint32_t maxFailedGroups = graph.allGroups.size() + 1;
    const uint32_t maxFailedSubGroups = 64;

    ctx.numFailedGroups = 0;
    while (ctx.numFailedGroups < maxFailedGroups)
    {
        ctx.numFailedSubgroups = 0;
        ctx.addEvent<Event::THREAD_START_GROUP>(1, ctx.groupId);
        while (ctx.numFailedSubgroups < maxFailedSubGroups)
        {
            if (!ctx.isSubgroupOwned && !doGroupUntilSubgroupEnter(ctx, true))
            {
                ctx.numFailedSubgroups = maxFailedSubGroups;
                break; // nothing pending
            }
            if (!ctx.isSubgroupOwned)
                continue;
            if (hasWakeCb)
                gatherAndWakeThreads(ctx, -1);
            SI_TG_PROFILE_EXCESSIVE("do_subgroup")
            ctx.isSubgroupOwned = doSubGroup(ctx, ctx.groupId, ctx.subgroupId);
            if (ctx.isSubgroupOwned)
                leaveSubgroup(ctx, ctx.groupId, ctx.subgroupId);
            ctx.isSubgroupOwned = false;
        }
        if (ctx.numFailedSubgroups >= maxFailedSubGroups)
        {
            ctx.numFailedGroups++;
            ctx.groupId = ctx.nextRnd();
            ctx.groupId %= graph.allGroups.size();
            continue;
        }
    }
    SI_TG_ASSERT(!ctx.isSubgroupOwned);

    uint8_t minState = CompiledTaskGraph::Task::STATE_DONE;
    for (const CompiledTaskGraph::Task &task : graph.allTasks)
        minState = std::min<uint8_t>(minState, task.state.load(std::memory_order_relaxed));
    if (minState >= CompiledTaskGraph::Task::STATE_EXECUTING)
    {
        ctx.addEvent<Event::THREAD_EXIT>(1);
        return ThreadResult::EXIT;
    }
    ctx.addEvent<Event::THREAD_WAIT>(1);
    if (hasWakeCb)
        sleepingThreadsMask.fetch_or(uint64_t(1) << uint64_t(thread_id), std::memory_order_relaxed);
    return ThreadResult::WAIT;
}

bool ThreadedTaskGraphExecutor::doGroupUntilSubgroupEnter(ThreadCtx & SI_TG_RESTRICT ctx, bool allow_var_tasks)
{
    // SI_TG_PROFILE_INTERNAL("do_group_next")
    CompiledTaskGraph & SI_TG_RESTRICT graph = *graphPtr;
    const uint32_t subGroupsStartIdx = graph.allGroups[ctx.groupId].subGroupsStart;
    CompiledTaskGraph::TaskGroupState & SI_TG_RESTRICT groupState = graph.allGroupsState[ctx.groupId];
    uint64_t pending = groupState.pending.load(std::memory_order_relaxed);
    if (pending == 0)
    {
        ctx.addEvent<Event::THREAD_NOTHING_PENDING>(1, ctx.groupId);
        return false; // move to next group
    }
    ctx.executingMask = groupState.executing.load(std::memory_order_relaxed);
    internal::BitIter bitIter(pending);
    while (bitIter.step())
    {
        ctx.subgroupId = subGroupsStartIdx + bitIter.idx();
        ctx.isSubgroupOwned = tryEnterSubgroup(ctx, ctx.groupId, ctx.subgroupId, ctx.executingMask, true);
        if (!ctx.isSubgroupOwned && allow_var_tasks)
        {
            // only try acquiring var task, if this group is executing
            if ((ctx.executingMask >> uint64_t(bitIter.idx())) & uint64_t(1u))
            {
                const auto [varTaskCnt, varTaskId] = tryAcquireVarTask(ctx, ctx.groupId, ctx.subgroupId);
                if (varTaskCnt > 2 && ctx.wakeCb)
                    gatherAndWakeThreads(ctx, varTaskCnt - 2);
                if (varTaskCnt > 0 && doVarTask(ctx, ctx.subgroupId, varTaskId, varTaskCnt - 1))
                    ctx.isSubgroupOwned = true;
            }
        }
        if (!ctx.isSubgroupOwned)
        {
            ctx.numFailedSubgroups++;
            continue;
        }
        ctx.numFailedGroups = 0;
        ctx.numFailedSubgroups = 0;
        break;
    }
    return true;
}

void ThreadedTaskGraphExecutor::validateAllDone() const
{
    for ([[maybe_unused]] const ThreadCtx &ctx : threadCtxArray)
        SI_TG_ASSERT(!ctx.isSubgroupOwned);
    for ([[maybe_unused]] const CompiledTaskGraph::Task &task : graphPtr->allTasks)
        SI_TG_ASSERT(task.state.load(std::memory_order_relaxed) == CompiledTaskGraph::Task::STATE_DONE);
}

uint64_t ThreadedTaskGraphExecutor::gatherAndWakeThreads(ThreadCtx & SI_TG_RESTRICT cur_ctx, int need_count)
{
    uint64_t sleepingMask = sleepingThreadsMask.load(std::memory_order_relaxed);
    if (sleepingMask == 0) // no sleeping threads
        return 0;
    if (need_count < 0)
    {
        SI_TG_PROFILE_EXCESSIVE("gatherThreadsToWake")
        CompiledTaskGraph& SI_TG_RESTRICT graph = *graphPtr;
        const uint32_t groupsCnt = graph.allGroups.size();
        int estimatedJobCnt = 0;
        for (uint32_t groupId = 0; groupId < groupsCnt; groupId++)
        {
            uint64_t pending = graph.allGroupsState[groupId].pending.load(std::memory_order_relaxed);
            if (pending == 0)
                continue;
            const uint32_t subgroupsStart = graph.allGroups[groupId].subGroupsStart;
            uint64_t executing = 0; // don't fetch current value, assume current jobs will finish
            internal::iter_set_bits(pending, [&](uint64_t bit_idx) {
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
        need_count = estimatedJobCnt;
    }

    if (need_count > 1)
        need_count++;

    sleepingMask = sleepingThreadsMask.load(std::memory_order_relaxed);
    while (sleepingMask)
    {
        internal::BitIter bitIter(sleepingMask);
        uint64_t wakeBits = 0;
        int wakeCnt = 0;
        while (bitIter.step() && wakeCnt < need_count)
        {
            wakeBits = bitIter.idx() + 1;
            wakeCnt++;
        }
        uint64_t wakeMask = sleepingMask & ((uint64_t(1) << wakeBits) - 1);
        if (sleepingThreadsMask.compare_exchange_strong(sleepingMask, sleepingMask & ~wakeMask, std::memory_order_relaxed))
        {
            cur_ctx.wakeCb(wakeMask);
            return wakeMask;
        }
    }
    return 0;
}

bool ThreadedTaskGraphExecutor::tryEnterSubgroup(ThreadCtx & SI_TG_RESTRICT ctx, uint32_t group_id, uint32_t subgroup_id, uint64_t &executing, bool loop)
{
    ctx.addEvent<Event::SUBGROUP_ENTER_ATTEMPT>(1, subgroup_id);
    CompiledTaskGraph & SI_TG_RESTRICT graph = *graphPtr;
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

ThreadedTaskGraphExecutor::VarTaskIdAndCount ThreadedTaskGraphExecutor::tryAcquireVarTask(ThreadCtx & SI_TG_RESTRICT ctx, uint32_t /* group_id */, uint32_t subgroup_id)
{
    CompiledTaskGraph & SI_TG_RESTRICT graph = *graphPtr;
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
            return VarTaskIdAndCount{varTaskCnt, varTaskIdx};
        }
    }
    return VarTaskIdAndCount{0, ~0u};
}

void ThreadedTaskGraphExecutor::leaveSubgroup(ThreadCtx & SI_TG_RESTRICT ctx, uint32_t group_id, uint32_t subgroup_id)
{
    ctx.addEvent<Event::SUBGROUP_LEAVE>(1, subgroup_id);
    CompiledTaskGraph & SI_TG_RESTRICT graph = *graphPtr;
    CompiledTaskGraph::TaskGroup &group = graph.allGroups[group_id];
    const uint64_t indexInGroup = subgroup_id - group.subGroupsStart;
    const uint64_t subgroupBit = uint64_t(1) << indexInGroup;
    graph.allGroupsState[group_id].executing.fetch_and(~subgroupBit, std::memory_order_acq_rel);
}

bool ThreadedTaskGraphExecutor::doSubGroup(ThreadCtx & SI_TG_RESTRICT ctx, uint32_t group_id, uint32_t subgroup_id)
{
    SI_TG_PROFILE_INTERNAL("do_subgroup");
    CompiledTaskGraph & SI_TG_RESTRICT graph = *graphPtr;
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
            if (task.taskData.taskNumFn)
                taskCnt = task.taskData.taskNumFn(userData);
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

bool ThreadedTaskGraphExecutor::doVarTask(ThreadCtx & SI_TG_RESTRICT ctx, uint32_t subgroup_id, uint32_t task_id, uint32_t var_task_idx)
{
    CompiledTaskGraph & SI_TG_RESTRICT graph = *graphPtr;
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
    CompiledTaskGraph & SI_TG_RESTRICT graph = *graphPtr;
    CompiledTaskGraph::Task &task = graph.allTasks[task_id];
    int32_t idx = task.subgraphDataIdx;
    uint32_t cnt = uint32_t(graph.subGraphData[idx++]);
    uint32_t subgraphEntryTask = uint32_t(graph.subGraphData[idx++]);
    uint32_t subgraphExitTask = uint32_t(graph.subGraphData[idx++]);
    int32_t &remaining = graph.subGraphData[idx++];
    SI_TG_ASSERT(task_id == subgraphEntryTask || task_id == subgraphExitTask);
    SI_TG_ASSERT(!task.allowToRunInParallelWithItself);

    const auto resetTaskStateAndDeps = [&] (uint32_t sg_task_id, [[maybe_unused]] bool expect_done)
    {
        CompiledTaskGraph::Task &subGraphTask = graph.allTasks[sg_task_id];
        SI_TG_ASSERT(sg_task_id != subgraphEntryTask);
        [[maybe_unused]] uint8_t curState = subGraphTask.state.load(std::memory_order_relaxed);
        SI_TG_ASSERT(!expect_done || curState == CompiledTaskGraph::Task::STATE_DONE);
        uint64_t deps = subGraphTask.dependencies.load(std::memory_order_relaxed);
        deps >>= uint64_t(32u);
        deps <<= uint64_t(32u);
        subGraphTask.dependencies.store(deps, std::memory_order_relaxed);
        subGraphTask.state.store(CompiledTaskGraph::Task::STATE_NONE, std::memory_order_relaxed);
    };
    const auto resetSubgraphTasks = [&] {
        for (uint32_t i = 0; i < cnt; i++)
            resetTaskStateAndDeps(graph.subGraphData[idx + i], true);
        resetTaskStateAndDeps(subgraphExitTask, false);
    };

    if (task_id == subgraphEntryTask)
    {
        SI_TG_PROFILE_INTERNAL("subgraph_entry");
        const bool needReset = remaining == -2;
        if (remaining < 0)
        {
            remaining = task.taskData.taskNumFn ? task.taskData.taskNumFn(task.taskData.userData) : 1;
            if (remaining > 0)
                ctx.addEvent<Event::SUBGRAPH_ENTER>(1, subgraphEntryTask);
            else
                ctx.addEvent<Event::SUBGRAPH_SKIP>(1, subgraphEntryTask);
            SI_TG_ASSERT(remaining >= 0);
        }
        if (remaining == 0) // subgraph is not required to run
        {
            remaining = -2; // signal to reset both remaining and this graph before next start
            for (uint32_t i = 0; i < cnt; i++)
            {
                uint32_t sgTaskId = graph.subGraphData[idx + i];
                SI_TG_ASSERT(graph.allTasks[sgTaskId].state.load(std::memory_order_relaxed) == CompiledTaskGraph::Task::STATE_NONE);
                graph.allTasks[sgTaskId].state.store(CompiledTaskGraph::Task::STATE_DONE, std::memory_order_relaxed);
            }
            graph.allTasks[subgraphEntryTask].state.store(CompiledTaskGraph::Task::STATE_DONE, std::memory_order_relaxed);
            SI_TG_ASSERT(graph.allTasks[subgraphExitTask].state.load(std::memory_order_relaxed) == CompiledTaskGraph::Task::STATE_NONE);
            graph.allTasks[subgraphExitTask].state.store(CompiledTaskGraph::Task::STATE_DONE, std::memory_order_relaxed);
            afterTaskDone(ctx, subgraphExitTask);
        }
        else
        {
            remaining--;
            if (needReset)
                resetSubgraphTasks();
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

bool ThreadedTaskGraphExecutor::afterTaskDone(ThreadCtx & SI_TG_RESTRICT ctx, uint32_t task_id)
{
    CompiledTaskGraph & SI_TG_RESTRICT graph = *graphPtr;
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



}