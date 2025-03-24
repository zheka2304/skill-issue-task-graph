#include "task_graph_execute.h"

#include <cassert>
#include "logger.h"

namespace sie
{

using logger::debug;


// TaskExecutionState

bool CompiledTaskGraph::TaskExecutionState::setPendingAndPreCheck()
{
    return (state.fetch_or(PENDING_BIT) & ~PENDING_BIT) == 0;
}

bool CompiledTaskGraph::TaskExecutionState::tryExec()
{
    while (true)
    {
        uint32_t curState = state.load();
        if ((curState & ~PENDING_BIT) != 0)
            return false;
        if (state.compare_exchange_strong(curState, curState | EXECUTING_BIT))
            return true;
    }
}

void CompiledTaskGraph::TaskExecutionState::endExec()
{
    state.fetch_or(DONE_BIT);
    state.fetch_and(~EXECUTING_BIT);
}

bool CompiledTaskGraph::TaskExecutionState::checkDone()
{
    return bool(state.load(std::memory_order_relaxed) & DONE_BIT);
}

bool CompiledTaskGraph::TaskExecutionState::tryLock()
{
    while (true)
    {
        uint32_t curState = state.load();
        if (bool(curState & EXECUTING_BIT))
            return false;
        if (state.compare_exchange_strong(curState, curState + 1))
            return true;
    }
}

bool CompiledTaskGraph::TaskExecutionState::unlockAndCheckPending()
{
    uint32_t fetched = state.fetch_sub(1);
    if ((fetched & ~PENDING_BIT) != 0) // if done or executing - it is not pending
        return false;
    return bool(state & PENDING_BIT) && !(state & (EXECUTING_BIT | DONE_BIT));
}


// SignalTreeNodeLeaf

void CompiledTaskGraph::SignalTreeNodeLeaf::initBeforeStart()
{
    taskQueuePos = 0;
    requirements.store(requirements.load(std::memory_order_relaxed) & ~uint64_t(0xffffffffu),
                       std::memory_order_relaxed);
}


// CompiledTaskGraph

uint32_t CompiledTaskGraph::nextPowOf2(uint32_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

template<typename T>
bool CompiledTaskGraph::selectNodeImpl(T& node)
{
    if constexpr (T::is_leaf)
    {
        return node.counter.exchange(0) != 0;
    } else
    {
        uint64_t expected = node.counter.load(std::memory_order_acquire);
        while (expected != 0)
        {
            if (node.counter.compare_exchange_strong(expected, expected - 1))
                return true;
        }
        return false;
    }
}

bool CompiledTaskGraph::executeQueueWhileCan(uint32_t thisQueueId)
{
    auto& queue = queueNodes[thisQueueId].taskQueue;
    uint32_t& queuePos = queueNodes[thisQueueId].taskQueuePos;

    std::vector<uint32_t> taskIds;
    while (queuePos < queue.size())
    {
        taskIds.push_back(queue[queuePos]);
        while (!taskIds.empty())
        {
            TaskNode& task = allTasks[taskIds.back()];
            taskIds.pop_back();
            if (!task.state.setPendingAndPreCheck())
                continue;
            bool allLocked = true;
            for (int i = 0; i < task.lockedTasks.size(); i++)
            {
                if (!allTasks[task.lockedTasks[i]].state.tryLock())
                {
                    allLocked = false;
                    int taskIdToExecNext = -1;
                    for (int j = i - 1; j >= 0; j--)
                    {
                        if (allTasks[task.lockedTasks[j]].state.unlockAndCheckPending())
                            taskIds.push_back(task.lockedTasks[j]);
                    }
                }
            }
            if (!allLocked)
                continue;
            if (task.state.tryExec())
            {
                task.task();
                task.state.endExec();
                setTreeNode(task.queueId); // set execution to continue for task queue
                for (uint32_t nextQueueId: task.nextQueues)
                    setTreeNodeRequirement(nextQueueId); // startup queues, that must run after this task
            }
            for (uint32_t taskId: task.lockedTasks)
                if (allTasks[taskId].state.unlockAndCheckPending())
                    taskIds.push_back(taskId);
        }

        uint32_t oldPos = queuePos;
        while (allTasks[queue[queuePos]].state.checkDone())
            queuePos++;
        if (oldPos == queuePos)
            return false; // unable to continue
    }
    return true;
}

uint32_t CompiledTaskGraph::selectTreeNode()
{
    if (!selectNodeImpl(treeRootNode))
        return TaskGraph::INVALID_ID;
    uint32_t nodeId = 0;
    uint32_t offset = 0;
    uint32_t offsetAdd = 2;
    for (uint32_t i = 0; i < treeDepthMinusTwo; i++)
    {
        if (!selectNodeImpl(treeMiddleNodes[offset + nodeId]))
        {
            nodeId++;
            // debug("graph", "  select 1 (%i)", int(offset + nodeId));
            const bool r = selectNodeImpl(treeMiddleNodes[offset + nodeId]);
            assert(r);
        }
        // else
        //     debug("graph", "  select 0 (%i)", int(offset + nodeId));
        nodeId <<= 1;
        offset += offsetAdd;
        offsetAdd <<= 1;
    }
    if (nodeId >= queueNodes.size())
    {
        debug("graph", "  out of bounds (%i)", int(nodeId));
        return TaskGraph::INVALID_ID;
    }
    if (!selectNodeImpl(queueNodes[nodeId]))
    {
        nodeId++;
        if (nodeId >= queueNodes.size())
        {
            debug("graph", "  out of bounds (%i)", int(nodeId));
            return TaskGraph::INVALID_ID;
        }
        // debug("graph", "  leaf 1 (%i)", int(nodeId));
        const bool r = selectNodeImpl(queueNodes[nodeId]);
        assert(r);
    }
    // else
    //     debug("graph", "  leaf 0 (%i)", int(nodeId));
    return nodeId;
}

void CompiledTaskGraph::setTreeNode(uint32_t nodeId)
{
    if (queueNodes[nodeId].counter.fetch_or(1, std::memory_order_relaxed) != 0)
        return; // already set
    nodeId >>= 1;
    uint32_t offset = treeMiddleNodes.size() - treeLeafHalfCount;
    uint32_t offsetSub = treeLeafHalfCount >> 1;
    for (uint32_t i = 0; i < treeDepthMinusTwo; i++)
    {
        treeMiddleNodes[offset + nodeId].counter.fetch_add(1, std::memory_order_acquire);
        nodeId >>= 1;
        offset -= offsetSub;
        offsetSub >>= 1;
    }
    treeRootNode.counter.fetch_add(1, std::memory_order_relaxed);
}

void CompiledTaskGraph::setTreeNodeRequirement(uint32_t nodeId)
{
    uint64_t packed = queueNodes[nodeId].requirements.fetch_add(1);
    uint32_t cur = uint32_t(packed);
    uint32_t req = uint32_t(packed >> 32);
    if (cur == req)
        setTreeNode(nodeId);
}

void CompiledTaskGraph::initBeforeStart()
{
    for (SignalTreeNodeLeaf& node: queueNodes)
        node.initBeforeStart();
}

void CompiledTaskGraph::rebuildTree()
{
    treeLeafHalfCount = nextPowOf2(queueNodes.size()) >> 1;
    treeDepthMinusTwo = 0;
    uint32_t nonLeavesSize = 0;
    for (uint32_t layerCnt = treeLeafHalfCount; layerCnt > 0; layerCnt >>= 1)
        treeDepthMinusTwo++, nonLeavesSize += layerCnt;
    treeDepthMinusTwo--;
    treeMiddleNodes.clear();
    treeMiddleNodes.resize(nonLeavesSize - 1);
    treeRootNode.counter = 0;
    sie::logger::debug("graph", "tree rebuild done: leaves=%i(%i) non-leaves=%i layers=%i", int(queueNodes.size()),
                       int(treeLeafHalfCount * 2), int(treeMiddleNodes.size() + 1), int(treeDepthMinusTwo + 2));
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
        while (true)
        {
            if (uint32_t queueId = self->graph->selectTreeNode(); queueId != TaskGraph::INVALID_ID)
            {
                sie::logger::debug("graph", "  execute %i from thread %i", int(queueId), thread_id);
                self->graph->executeQueueWhileCan(queueId);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else
                break;
        }
        std::unique_lock lock(self->condVarMutex);
        self->condVar.wait(lock, [] { return true; });
    }
    //workerThreadId = -1;
    sie::logger::debug("worker", "shutdown %i", thread_id);
}

}