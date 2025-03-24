#include "task_graph.h"


namespace sie
{

uint32_t TaskGraph::addTask()
{
    uint32_t id = allNodes.size();
    allNodes.emplace_back();
    return id;
}

void TaskGraph::setNext(uint32_t task, uint32_t next)
{
    allNodes[task].nextTasks.push_back(next);
    allNodes[next].prevTasks.push_back(task);
}

void TaskGraph::addResource(uint32_t task, uint64_t resId, bool write)
{
    allNodes[task].resources.push_back(ResourceRef{resId, uint64_t(write)});
}

void TaskGraph::TaskNode::addNext(uint32_t id)
{
    if (std::find(nextTasks.begin(), nextTasks.end(), id) == nextTasks.end())
        nextTasks.push_back(id);
}

void TaskGraph::TaskNode::addPrev(uint32_t id)
{
    if (std::find(prevTasks.begin(), prevTasks.end(), id) == prevTasks.end())
        prevTasks.push_back(id);
}
}