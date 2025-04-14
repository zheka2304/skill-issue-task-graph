#include "task_graph_execute.h"

#include <cassert>
#include "logger.h"


namespace sie
{

using logger::debug;

void CompiledTaskGraph::doThread()
{
    const uint32_t maxFailedGroups = 8;
    const uint32_t maxFailedSubGroups = 32;

    uint32_t groupId = 0;
    uint32_t failedGroups = 0;

    while (failedGroups < maxFailedGroups)
    {
        uint32_t failedSubgroups = 0;
        while (failedSubgroups < maxFailedSubGroups)
        {
            uint64_t pending = allGroups[groupId].pending.load(std::memory_order_relaxed);
            if (pending == 0)
            {
                // end this group - nothing is pending
                failedSubgroups = maxFailedSubGroups;
                break;
            }
            iter_set_bits(pending, [&] (uint32_t bit_idx) {
                uint32_t subgroupId = allGroups[groupId].subGroupsStart + bit_idx;
                bool owned = tryEnterSubgroup(groupId, subgroupId, false);
                if (!owned)
                {
                    const auto [varTaskCnt, varTaskId] = tryAcquireVarTask(groupId, subgroupId);
                    if (varTaskCnt > 0 && doVarTask(subgroupId, varTaskId, varTaskCnt - 1))
                        owned = true;
                }
                if (!owned)
                {
                    failedSubgroups++;
                    return;
                }
                failedSubgroups = 0;
                owned = doSubGroup(groupId, subgroupId);
                if (owned)
                    leaveSubgroup(groupId, subgroupId);
            });
        }
        if (failedSubgroups == maxFailedSubGroups)
        {
            failedGroups++;
            groupId++;
            groupId %= allGroups.size();
            continue;
        }
    }
}

bool CompiledTaskGraph::checkAllTasksDone()
{
    bool done = true;
    for (const Task &task : allTasks)
        done &= task.state.load(std::memory_order_relaxed) == Task::STATE_DONE;
    return done;
}

bool CompiledTaskGraph::checkAllTasksExecutingOrDone()
{
    bool done = true;
    for (const Task &task : allTasks)
    {
        uint8_t state = task.state.load(std::memory_order_relaxed);
        done &= state == Task::STATE_EXECUTING || state == Task::STATE_DONE;
    }
    return done;
}

bool CompiledTaskGraph::tryEnterSubgroup(uint32_t group_id, uint32_t subgroup_id, bool loop)
{
    const uint64_t mask = allSubGroups[subgroup_id].excludedMask;
    TaskGroup &group = allGroups[group_id];
    const uint64_t indexInGroup = subgroup_id - group.subGroupsStart;
    const uint64_t subgroupBit = uint64_t(1) << indexInGroup;
    uint64_t executing = group.executing.load(std::memory_order_relaxed);
    while ((executing & mask) == 0)
    {
        uint64_t newExecuting = executing | subgroupBit;
        if (group.executing.compare_exchange_strong(executing, newExecuting, std::memory_order_acq_rel))
            return true;
        if (!loop)
            break;
    }
    if ((executing & subgroupBit) == 0)
        group.pending.fetch_or(subgroupBit, std::memory_order_relaxed);
    return false;
}

std::pair<uint32_t, uint32_t> CompiledTaskGraph::tryAcquireVarTask(uint32_t group_id, uint32_t subgroup_id)
{
    TaskSubGroup& subgroup = allSubGroups[subgroup_id];
    uint64_t curVarTask = subgroup.curVarTask.load(std::memory_order_relaxed);
    while (curVarTask != 0)
    {
        const uint32_t varTaskCnt = uint32_t(curVarTask);
        const uint32_t varTaskIdx = uint32_t(curVarTask >> 32u);
        const uint32_t newVarTaskCnt = varTaskCnt - 1;
        const uint32_t newVarTaskIdx = newVarTaskCnt ? varTaskIdx : 0;
        const uint64_t newVarTask = uint64_t(newVarTaskCnt) | (uint64_t(newVarTaskIdx) << 32u);
        if (subgroup.curVarTask.compare_exchange_strong(curVarTask, newVarTask, std::memory_order_acq_rel))
            return {varTaskCnt, varTaskIdx};
    }
    return {0, ~0u};
}

void CompiledTaskGraph::leaveSubgroup(uint32_t group_id, uint32_t subgroup_id)
{
    TaskGroup &group = allGroups[group_id];
    const uint64_t indexInGroup = subgroup_id - group.subGroupsStart;
    const uint64_t subgroupBit = uint64_t(1) << indexInGroup;
    allGroups[group_id].executing.fetch_and(~subgroupBit, std::memory_order_acq_rel);
}

bool CompiledTaskGraph::doSubGroup(uint32_t group_id, uint32_t subgroup_id)
{
    TaskGroup &group = allGroups[group_id];
    const uint64_t indexInGroup = subgroup_id - group.subGroupsStart;
    const uint64_t subgroupBit = uint64_t(1) << indexInGroup;

    bool anyTasks = true;
    TaskSubGroup& subgroup = allSubGroups[subgroup_id];
    while (anyTasks)
    {
        group.pending.fetch_and(~subgroupBit, std::memory_order_relaxed);
        anyTasks = false;
        for (uint32_t taskId = subgroup.tasksStart; taskId < subgroup.tasksEnd; taskId++)
        {
            Task &task = allTasks[taskId];
            if (task.state.load(std::memory_order_relaxed) != Task::STATE_PENDING)
                continue;
            task.state.store(Task::STATE_EXECUTING, std::memory_order_relaxed);
            anyTasks = true;
            // (optionally) wake other threads

            int taskCnt = 1;
            if (task.varTask)
                taskCnt = task.varTask();
            if (taskCnt < 2 || !task.allowToRunInParallelWithItself)
            {
                // not variadic or can't parallel with self
                if (taskCnt > 1)
                {
                    for (int i = taskCnt - 1; i >= 0; i--)
                        task.task(i);
                }
                else if (taskCnt == 1) task.task(0);
                afterTaskDone(taskId);
            }
            else
            {
                // wind up variadic task
                assert(subgroup.remainingVarTasks.load(std::memory_order_relaxed) == 0);
                subgroup.remainingVarTasks.fetch_add(taskCnt, std::memory_order_relaxed);
                uint64_t expected = 0;
                subgroup.curVarTask.compare_exchange_strong(expected, (uint64_t(taskId) << 32u) | uint64_t(taskCnt), std::memory_order_acq_rel);
                assert(expected == 0);
                group.pending.fetch_or(subgroupBit, std::memory_order_relaxed);
                return false; // not owned
            }
        }
    }
    return true; // still owned
}

bool CompiledTaskGraph::doVarTask(uint32_t subgroup_id, uint32_t task_id, uint32_t var_task_idx)
{
    Task &task = allTasks[task_id];
    task.task(int(var_task_idx));

    const int64_t remaining = allSubGroups[subgroup_id].remainingVarTasks.fetch_sub(1, std::memory_order_acq_rel);
    assert(remaining > 0);
    bool owned = remaining == 1;
    if (owned)
        afterTaskDone(task_id);
    return owned;
}

bool CompiledTaskGraph::afterTaskDone(uint32_t task_id)
{
    Task &task = allTasks[task_id];
    bool anyTasksFromThisSubGroupArePending = false;

    // TODO: fixme
    std::vector<uint64_t> subGroupMasksToExecuteNext;
    for (auto nextTaskId : task.nextTasks)
    {
        Task &nextTask = allTasks[nextTaskId];
        if (nextTask.dependencies.load(std::memory_order_relaxed) != 0) // has more than 1 dependency
        {
            uint64_t deps = nextTask.dependencies.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (uint32_t(deps) != (deps >> uint64_t(32)))
                continue; // not all dependencies met yet
        }
        assert(nextTask.state.load(std::memory_order_relaxed) == Task::STATE_NONE);
        nextTask.state.store(Task::STATE_PENDING, std::memory_order_relaxed);

        if (nextTask.subGroupId == task.subGroupId)
        {
            anyTasksFromThisSubGroupArePending = true;
        }
        else
        {
            subGroupMasksToExecuteNext.resize(allGroups.size(), 0);
            uint32_t nextSubgroupIndexInGroup = nextTask.subGroupId - allGroups[nextTask.groupId].subGroupsStart;
            subGroupMasksToExecuteNext[nextTask.groupId] |= uint64_t(1u) << uint64_t(nextSubgroupIndexInGroup);
        }
    }

    for (uint32_t groupId = 0; groupId < subGroupMasksToExecuteNext.size(); groupId++)
    {
        if (subGroupMasksToExecuteNext[groupId] != 0)
            allGroups[groupId].pending.fetch_or(subGroupMasksToExecuteNext[groupId], std::memory_order_relaxed);
    }

    task.state.store(Task::STATE_DONE, std::memory_order_relaxed);
    return anyTasksFromThisSubGroupArePending;
}


// TaskGraphExecutor

void TaskGraphExecutor::windUp(int count)
{
    shutdown();
    running = true;
    threads.reserve(count);
    for (int i = 0; i < count; i++)
        threads.emplace_back(exec, this, i);
}

void TaskGraphExecutor::wakeAll()
{
    condVar.notify_all();
}

void TaskGraphExecutor::shutdown()
{
    running = false;
    wakeAll();
    for (std::thread& t: threads)
        t.join();
    threads.clear();
}

TaskGraphExecutor::~TaskGraphExecutor()
{ shutdown(); }

void TaskGraphExecutor::exec(TaskGraphExecutor* self, int thread_id)
{
    sie::logger::debug("worker", "startup %i", thread_id);
    //workerThreadId = thread_id;
    std::vector<TaskFnPtr> taskQueue;
    while (self->running)
    {
        self->graph->doThread();
        if (self->graph->checkAllTasksExecutingOrDone())
            break;
        // todo: yield

        // std::unique_lock lock(self->condVarMutex);
        // self->condVar.wait(lock, [] { return true; });
    }
    //workerThreadId = -1;
    sie::logger::debug("worker", "shutdown %i", thread_id);
}

}