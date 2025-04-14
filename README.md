# Skill Issue Task Graph

A lightweight, easy to integrate and highly customizable task graph library I've made in my free time. Intended to be used as a task scheduler for games, but not limited to this.

Features:
- Task ordering
- Resource usage declaration for tasks
- Running multiple invocations of one task, parallel execution when possible, determine count of invocations at runtime
- Sub-graphs with an option to determine number of executions at runtime, to enable control flow
- Ahead-of-time task graph compilation
- Putting threads to sleep and waking them
- [Planned] Thread affinity mask for tasks

Note that this is not a production-ready solution, but rather a project, I've made for fun. It must be properly tested and benchmarked before using it for real.

## Installation

1. Copy `include` and `src` anywhere in your project
2. Add `include` directory to your include path
3. Build all files in `src`
4. Customize if needed in `task_graph_config.h` and `task_graph_config.cpp`

## Benchmarks

I'm too lazy to benchmark it properly. *In a synthetic test with 1000-2000 empty tasks with 0-10 random connections each, with up to 8 threads, it usually runs as fast as similar graph in [taskflow](https://github.com/taskflow/taskflow) or faster, but likely in some other cases it might struggle.* If you are really that interested, you can test it yourself on some scenarios, that closer to real-life usage.

The whole point here is not the performance, but rather having more features, main one being resource usage declaration and resolution.

## Feature Usage

### Running Simple Graph

Create graph and declare tasks:
```c++
#include <taskgraph/task_graph_compile.h>
#include <taskgraph/task_graph_execute.h>

si::tg::TaskGraph taskGraph;
si::tg::TaskId task1 = taskGraph.addTask({ .taskFn = +[] (void*, int) { std::cout << "task1\n"; } });
si::tg::TaskId task2 = taskGraph.addTask({ .taskFn = +[] (void*, int) { std::cout << "task2\n"; } });
si::tg::TaskId task3 = taskGraph.addTask({ .taskFn = +[] (void*, int) { std::cout << "task3\n"; } });
taskGraph.setNext(task1, task3);
taskGraph.setNext(task2, task3);
```

NOTE: First argument of type `void*` is user data pointer. It can be passed via `userData` field.

Compile graph:
```c++
si::tg::CompiledTaskGraph compiledGraph;
if (!si::tg::build_and_compile_graph<si::tg::strategy::MergeSubgroups>(taskGraph, &compiledGraph))
    return; // error
```

Use simple thread pool, that comes with this library to execute it
```c++
si::tg::SimpleThreadPool threadPool(/* threads */ 4);
pool.executeAndWait(&compiledGraph, /* sleep when idle */ true);
```

### Adding Resources

Resources are passed as 64-bit integer IDs, and usage type. Resources put constrains on which tasks can run in parallel.

Resource usage types:
- `ResourceUsage::Shared` - aka `Read` - multiple tasks with `Shared` usage can run in parallel
- `ResourceUsage::Locking` - aka `Write` - task with this usage can't run in parallel to any other tasks, that use this resource (both `Shared` and `Locking`)
- `ResourceUsage::NotUsed` - signal, that this task does not use this resource (basically removes resource usage, also is one of possible results of `getResourceUsage`)

```c++
const uint64_t RESOURCE_A = 0;
const uint64_t RESOURCE_B = 1;
const uint64_t RESOURCE_C = 2;
taskGraph.setResourceUsage(task1, RESOURCE_A, si::tg::ResourceUsage::Locking);
taskGraph.setResourceUsage(task1, RESOURCE_B, si::tg::ResourceUsage::Shared);
taskGraph.setResourceUsage(task2, RESOURCE_B, si::tg::ResourceUsage::Locking);
taskGraph.setResourceUsage(task3, RESOURCE_C, si::tg::ResourceUsage::Shared);
```

### Running Task Several Times

Following code will execute the task 10 times. Second function will be called at some point before executing the task, after all tasks, ordered before this one, are done, and must return number of invocations for the task (can be 0). In case task does not have any resources with `Locking` usage, it can run in parallel with itself.

```c++
taskGraph.addTask({
    .taskFn = +[] (void*, int idx) { std::cout << "task invocation " << idx << "\n"; },
    .taskNumFn = +[] (void*) { return 10; }
});
```

NOTE: Common pattern here is to add a task, preceding the main task, that uses some resources to prepare data for the main task, that will run multiple times, and then use the prepared data, to determine, how much times it must run.

### Sub-Graphs

Sub-graphs are the feature, that serves 2 main purposes:
- Better ordering - update stages can be packed into sub-graphs and ordered again each other
- Control flow - entire sub-graphs can be skipped or executed several times

Adding sub-graph can be done as simple as:

```c++
si::tg::TaskGraph taskGraph;
si::tg::TaskGraph subGraph1;
si::tg::TaskGraph subGraph2;
si::tg::TaskGraph subGraph3;
// ... add other tasks to each one
si::tg::TaskId subGraph1Id = taskGraph.addSubGraphTask(&subGraph1);
si::tg::TaskId subGraph2Id = taskGraph.addSubGraphTask(&subGraph2);
si::tg::TaskId subGraph3Id = taskGraph.addSubGraphTask(&subGraph3);
taskGraph.setNext(subGraph1Id, subGraph3Id); // order all tasks in subGraph1 before subGraph3
taskGraph.setNext(subGraph12Id, subGraph3Id); // same for subGraph2. subGraph1 and subGraph2 can run in parallel
```

NOTE: Sub-graphs are kept as pointers internally. Managing lifetimes of each subgraph is entirely user's responsibility. Until sub-graph is compiled, all added sub-graphs must be not be destroyed or relocated.

`taskNumFn` can be specified for sub-graphs (different invocations of sub-graph will always run in sequence, not in parallel):
```c++
taskGraph.addSubGraphTask(&subGraph1, {.taskNumFn = (void*) { return 10; }});
```

### Integrating into Custom Thread Pool

Create an executor
```c++
si::tg::ThreadedTaskGraphExecutor executor;
executor.graphPtr = &compiledGraph;
```

Before running threads
```c++
executor.prepareForExecution(THREAD_NUM);
```

In worker thread:
```c++
while (true)
{
    // execute for given thread, until there is anything to execute
    // threadId = [0; THREAD_NUM)
    const auto result = executor.doThread(threadId);
    if (result == si::tg::ThreadResult::DONE)
        // this thread has finished all tasks it could, exit
        return;
    // otherwise - repeat
}
```

Additionally, you can implement waiting when thread is idle:
```c++
executor.setWakeCallback(threadId, [] (uint64_t wake_mask) {
    // N-th set bit in wake_mask means you should wake
    // thread with index N. This is just a recommendation, you can ignore it or wake
    // other sleeping thread.
});
while (true)
{
    // do several attempts
    for (int i = 0; i < 4; i++)
        if (executor.doThread(threadId) == si::tg::ThreadResult::DONE)
            return;
    // wait until woken
}
```

## Execution Algorithm

Compiled graph has 3 levels of hierarchy - groups, subgroups and individual tasks

1. Group - tasks split between groups in a way, that it is guaranteed, that any pair of tasks from different groups can share resources (they either have shared usage or can't run in parallel due to ordering).
2. Subgroup - group contains up to 64 subgroups. 64 is chosen, because commonly it is the largest atomic word. Each subgroup has a 64-bit mask, that determines, which subgroups from the same group it can't run in parallel with.
3. Task - a single task belongs to subgroup

Compilation is done in 2 steps:
1. Pre-building and normalizing graph
    - recursively resolve and merge all sub-graphs, generate sub-graph entry and exit tasks
    - build task exclusion graph from resource usage and ordering
    - normalize ordering by eliminating redundant connections
2. Compiling graph - this step is a tricky part. Ideally you want each subgroup to have only 1 task, but there is a requirement of max of 64 subgroups. So trade-offs must be made. Different compilation strategies can be implemented:
    - Merge strategy - this is the only one currently made. It progressively merges pairs of subgroups to minimize number of edges in resulting exclusion graph. Each pass reduces the number of subgroups by half, but it is still pretty slow.
    - [Planned] Ordering strategy - opposite approach to merge strategy - gather groups of 64 tasks each and then order all tasks between these groups, so they can never execute in parallel.

Execution algorithm is best described via pseudocode (not complete):
```
prepareForExecution():
    - allocate and init enough thread contexts
    reset all tasks:
        - reset fulfilled dependency count
        - reset state to NONE or PENDING for tasks with no dependencies
        - set bits in pending masks for subgroups, that contain pending tasks

doThread():
    - get context for this thread
    - randomly shuffle group indices to lower contention
 
    while we have not failed all groups in a row:
        - load pending and executing subgroups masks for current group
        for each set bit in pending mask:
            try acquiring subgroup (CAS loop):
                - get subgroup exclusion mask
                - compare it against previously loaded executing mask
                - in case they are exclusive, we've failed
            if acquiring subgroup failed and it is in execution mask:
                - try atomically acquiring variable count task from this subgroup
                - in case of success, execute this task
                - atomically decrement number of variable count tasks for this subgroup
                if it was the last task - we are now owning the subgroup:
                    - markTaskDone()
                    - subgroup is now owned by this thread
            if failed to acquire subgroup:
                - add to failed subgroups counter and continue
                - continue to the next pending subgroup
            execute tasks in subgroup:
                - go through tasks, skip all, that are not pending
                if this is normal task:
                    - just execute it
                    - mark task done
                    - restart this subgroup execution
                if this is variable count task, that can run in parallel with itself:
                    - setup this subgroup variable count task data
                    - return, without resetting executing bit for this subgroup
            - reset failed groups and subgroups counters
        if failed subgroup count is large enough:
            - increment failed groups count
            - move to next group in randomly shuffled list

markTaskDone(task):
    - set task state to done
    for all next tasks:
        if next task has more than 1 dependency:
            - increment fulfilled dependency count by 1
            if not all dependencies yet fulfilled:
                - continue
        - set next task state to pending
        - set bit to next task's group pending mask
```