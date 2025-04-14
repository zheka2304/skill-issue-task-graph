# Skill Issue Task-Graph

A lightweight, easy to integrate and highly customizable task graph library I've made in my free time. Intended to be used as a task scheduler for games, but not limited to this.

Features:
- Task ordering
- Resource usage declaration for tasks
- Running multiple invocations of one task, parallel execution when possible, determine count of invocations at runtime
- Sub-graphs with an option to determine number of executions at runtime, to enable control flow
- [Planned] Thread affinity mask for tasks
- Ahead-of-time task graph compilation

Note that this is not a production-ready solution, but rather a project, I've made for fun. It must be properly tested and benchmarked before using it for real.

## Installation

1. Copy `include` and `src` anywhere in your project
2. Add `include` directory to your include path
3. Build all files in `src`
4. Customize if needed in `task_graph_config.h` and `task_graph_config.cpp`

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