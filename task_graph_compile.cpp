#include "task_graph.h"

#include <cassert>
#include <sstream>
#include "logger.h"
#include "task_graph_execute.h"

namespace sie
{

using logger::error;

// TaskNode

void TaskGraph::TaskNode::addNext(uint32_t id)
{
    if (std::find(nextTasks.begin(), nextTasks.end(), id) == nextTasks.end())
        nextTasks.push_back(id);
}

void TaskGraph::TaskNode::removeNext(uint32_t id)
{
    if (auto it = std::find(nextTasks.begin(), nextTasks.end(), id); it != nextTasks.end())
        nextTasks.erase(it);
}


// TaskGraph

uint32_t TaskGraph::addTask()
{
    uint32_t id = allNodes.size();
    allNodes.emplace_back();
    return id;
}

void TaskGraph::setNext(uint32_t task, uint32_t next)
{
    allNodes[task].addNext(next);
}

void TaskGraph::addResource(uint32_t task, uint64_t resId, bool write)
{
    allNodes[task].resources.push_back(ResourceRef{resId, uint64_t(write)});
}

bool TaskGraph::validateAndNormalize()
{
    bool isValid = true;

    // prepare
    for (uint32_t i = 0; i < allNodes.size(); i++)
    {
        allNodes[i].prevTasks.clear();
        allNodes[i].visited = false;
        allNodes[i].fiberId = INVALID_ID;
    }

    // validate no cycles
    std::vector<uint32_t> stack;
    std::vector<uint32_t> visited;
    for (uint32_t i = 0; i < allNodes.size(); i++)
    {
        visited.clear();
        traverseNodeSequence(stack, visited, i, [&] (uint32_t id) {
            if (std::find(stack.begin(), stack.end(), id) != stack.end())
            {
                error("graph", "node cycle detected! %i", id);
                assert(0);
                isValid = false;
            }
            return id;
        });
    }
    if (!isValid)
        return false;
    // normalize order
    for (uint32_t baseId = 0; baseId < allNodes.size(); baseId++)
    {
        visited.clear();
        traverseNodeSequence(stack, visited, baseId, [&] (uint32_t id) {
            if (stack.size() > 1)
                allNodes[baseId].removeNext(id);
            return id;
        });
    }

    // build prev list
    for (uint32_t i = 0; i < allNodes.size(); i++)
    {
        for (uint32_t next : allNodes[i].nextTasks)
            allNodes[next].prevTasks.push_back(i);
    }

    return isValid;
}

void TaskGraph::buildFibers()
{
    entryFiberIds.clear();
    allFibers.clear();
    for (uint32_t i = 0; i < allNodes.size(); i++)
        allNodes[i].fiberId = INVALID_ID;

    for (uint32_t i = 0; i < allNodes.size(); i++)
    {
        if (allNodes[i].fiberId != INVALID_ID)
            continue;
        // find base
        uint32_t base = i;
        while (true)
        {
            if (allNodes[base].prevTasks.size() != 1)
                break;
            uint32_t prev = allNodes[base].prevTasks.front();
            if (allNodes[prev].fiberId != INVALID_ID)
                break;
            base = prev;
        }
        Fiber fiber;
        fiber.dependencies = allNodes[base].prevTasks;
        uint32_t fiberId = allFibers.size();

        // find end and assign fiber
        uint32_t end = base;
        while (true)
        {
            allNodes[end].fiberId = fiberId;
            fiber.tasks.push_back(end);
            uint32_t next = INVALID_ID;
            for (uint32_t id : allNodes[end].nextTasks)
                if (allNodes[id].fiberId == INVALID_ID)
                    next = id;
            if (next == INVALID_ID)
                break;
            end = next;
        }

        if (fiber.dependencies.empty())
            entryFiberIds.push_back(fiberId);
        allFibers.push_back(std::move(fiber));
    }
}

template<typename U, typename F>
void TaskGraph::traverseNodeSequence(std::vector<U>& stack, std::vector<uint32_t>& visited, uint32_t node, F&& f)
{
    if (std::find(visited.begin(), visited.end(), node) != visited.end())
        return;
    stack.push_back(f(node));
    visited.push_back(node);
    auto tasks = allNodes[node].nextTasks;
    for (uint32_t next : tasks)
        traverseNodeSequence(stack, visited, next, f);
    stack.pop_back();
}

void TaskGraph::dumpToLog()
{
    std::vector<std::stringstream> rows;
    rows.resize(allFibers.size());

    struct FiberPrintData
    {
        uint32_t id = INVALID_ID;
        int pos;
        std::vector<uint32_t> deps;
        bool hadDeps;
    };
    std::vector<FiberPrintData> fibersToPrint;

    int baseStartPos = 0;
    for (uint32_t id = 0; id < allFibers.size(); id++)
    {
        fibersToPrint.push_back(FiberPrintData{id, 0, allFibers[id].dependencies, !allFibers[id].dependencies.empty()});
        rows[id] << "[ ";
        for (uint32_t t : allFibers[id].dependencies)
            rows[id] << t << " ";
        rows[id] << "] ";
        baseStartPos = std::max<int>(baseStartPos, rows[id].view().size());
    }
    for (FiberPrintData &f : fibersToPrint)
        f.pos = baseStartPos + 1;

    auto addSpace = [&] (uint32_t idx, int required_len)
    {
        auto &ss = rows[idx];
        while (ss.view().size() < required_len)
            ss << ' ';
    };

    while (true)
    {
        FiberPrintData fiber;
        for (auto it = fibersToPrint.begin(); it != fibersToPrint.end(); it++)
        {
            if (it->deps.empty())
            {
                fiber = std::move(*it);
                fibersToPrint.erase(it);
                break;
            }
        }
        if (fiber.id == INVALID_ID)
            break;
        addSpace(fiber.id, fiber.pos);

        bool first = true;
        for (uint32_t taskId : allFibers[fiber.id].tasks)
        {
            if (first && fiber.hadDeps)
                rows[fiber.id] << "|---> ";
            else if (!first)
                rows[fiber.id] << " --> ";
            first = false;
            rows[fiber.id] << taskId;

            for (FiberPrintData &f : fibersToPrint)
            {
                while (true)
                {
                    if (auto it = std::find(f.deps.begin(), f.deps.end(), taskId); it != f.deps.end())
                    {
                        f.deps.erase(it);
                        f.pos = std::max<int>(f.pos, rows[fiber.id].view().size() - 1);
                    }
                    else
                        break;
                }
            }
        }
    }

    for (auto &ss : rows)
    {
        auto s = ss.str();
        logger::debug("graph", "%s", s.c_str());
    }
    logger::flush_default_log();
}

bool TaskGraph::compileTo(sie::CompiledTaskGraph& graph)
{
    if (!validateAndNormalize())
        return false;
    buildFibers();

    graph.allTasks.clear();
    graph.allTasks.resize(allNodes.size());
    for (uint32_t taskId = 0; taskId < allNodes.size(); taskId++)
    {
        graph.allTasks[taskId].task = allNodes[taskId].fn;
        graph.allTasks[taskId].queueId = allNodes[taskId].fiberId;
    }

    graph.queueNodes.clear();
    graph.queueNodes.resize(allFibers.size());
    for (uint32_t fiberId = 0; fiberId < allFibers.size(); fiberId++)
    {
        graph.queueNodes[fiberId].taskQueue = allFibers[fiberId].tasks;
        graph.queueNodes[fiberId].setRequirementsCount(allFibers[fiberId].dependencies.size());
        for (uint32_t depId : allFibers[fiberId].tasks)
            graph.allTasks[depId].nextQueues.push_back(fiberId);
    }

    return true;
}

}