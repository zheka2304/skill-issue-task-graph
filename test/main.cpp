#include <iostream>
#include <atomic>
#include <vector>
#include <cassert>
#include <deque>

#include "optick.h"
#include "taskgraph/task_graph_compile.h"
#include "taskgraph/task_graph_execute.h"
#include "taskgraph/simple_thread_pool.h"

#define PERF_TEST_MODE 1


using namespace si::tg;

struct TestResource
{
    static constexpr uint64_t USAGE_WRITE = uint64_t(1) << uint64_t(32);
    std::atomic<uint64_t> usage = 0;

    TestResource() = default;
    TestResource(const TestResource &) {}
    void validate(bool valid, const char *place) const { SI_TG_ASSERT_FMT(valid, " %s", place); }
    void beginRead() { validate(usage.fetch_add(1, std::memory_order_relaxed) < USAGE_WRITE, "beginRead"); }
    void beginWrite() { validate(usage.fetch_add(USAGE_WRITE, std::memory_order_relaxed) == 0, "beginWrite"); }
    void endWrite() { validate(usage.fetch_sub(USAGE_WRITE, std::memory_order_relaxed) == USAGE_WRITE, "endWrite"); }
    void endRead() { validate(usage.fetch_sub(1, std::memory_order_relaxed) < USAGE_WRITE, "endRead"); }
};

struct TestTask
{
    TaskId localId;
    int graphIdx;
    struct TestTaskGraph *graph;

    std::vector<int> resRead;
    std::vector<int> resWrite;
    std::vector<int> tasksNext;
    std::vector<int> tasksPrev;
    int varTaskMin = 1;
    int varTaskMax = 1;

    int varTaskCount = -1;
    std::atomic<uint64_t> executedTimes = 0;

    void doTask(int idx);
    int getVarTaskCnt();

    TaskData getTaskData()
    {
        TaskData data;
        data.userData = this;
        data.taskFn = +[] (void *self, int idx) { static_cast<TestTask *>(self)->doTask(idx); };
        data.taskVarFn = +[] (void *self) { return static_cast<TestTask *>(self)->getVarTaskCnt(); };
        return data;
    }

    void validate()
    {
        SI_TG_ASSERT(varTaskCount >= 0);
#if !PERF_TEST_MODE
        SI_TG_ASSERT(varTaskCount == int(executedTimes));
#endif
    }

    void reset()
    {
        varTaskCount = -1;
        executedTimes = 0;
    }
};

struct TestTaskGraph
{
    int seed = 0;
    std::atomic<int> stub;

    std::vector<TestResource> resources;
    std::deque<TestTask> tasks;
    std::deque<TaskGraph> taskGraphs;
    TaskGraph rootGraph;

    PrebuiltTaskGraph prebuild;
    CompiledTaskGraph compiled;

    int rnd() { return rand(); }
    int rnd(int a) { return rnd() % a; }
    int rnd(int a, int b) { return a >= b ? a : rnd() % (b - a) + a; }
    float rndF() { return float(rand()) / float(1 + RAND_MAX); }
    float rndF(float a) { return rnd() * a; }
    float rndF(float a, float b) { return rnd() * (b - a) + a; }

    void compile()
    {
        const bool result = prebuild_task_graph(prebuild, rootGraph);
        SI_TG_ASSERT(result);
        strategy::MergeSubgroupsState state;
        compile_task_graph(compiled, prebuild, state);
    }

    void print() { print_compiled_graph(compiled); }

    void execute(SimpleThreadPool &pool, bool use_wait)
    {
        for (TestTask &task : tasks)
            task.reset();
        pool.execute(&compiled, use_wait);
        for (TestTask &task : tasks)
            task.validate();
    }
};

void TestTask::doTask(int idx)
{
#if !PERF_TEST_MODE
    OPTICK_EVENT("task");
    for (int resId : resWrite)
        graph->resources[resId].beginWrite();
    for (int resId : resRead)
        graph->resources[resId].beginRead();
    executedTimes.fetch_add(1, std::memory_order_relaxed);

    volatile int x = 0;
    volatile int y = 0;
    for (int i = 0; i < 1000; i++)
        x = y;
    (void) x;

    for (int resId : resRead)
        graph->resources[resId].endRead();
    for (int resId : resWrite)
        graph->resources[resId].endWrite();
#endif
}

int TestTask::getVarTaskCnt()
{
    return (varTaskCount = graph->rnd(varTaskMin, varTaskMax));
}


struct RandomSubGraphParams
{
    int taskCountMin = 0;
    int taskCountMax = -1;
    int varTaskMin = 1;
    int varTaskMax = 2;

    int connMin = 0;
    int connMax = 0;

    int resMin = 0;
    int resMax = 0;
    float resLocking = 0.;
    int resStart = -1;
    int resEnd = -1;
};

TaskId add_random_subgraph(TestTaskGraph &test_graph, RandomSubGraphParams params)
{
    TaskGraph &graph = test_graph.taskGraphs.emplace_back();
    const int tasksOffset = int(test_graph.tasks.size());

    const int taskCount = test_graph.rnd(params.taskCountMin, params.taskCountMax);
    for (int i = 0; i < taskCount; i++)
    {
        TestTask &task = test_graph.tasks.emplace_back();
        task.graph = &test_graph;
        task.graphIdx = int(test_graph.taskGraphs.size() - 1);
        task.localId = graph.addTask(task.getTaskData());
        task.varTaskMin = params.varTaskMin;
        task.varTaskMax = params.varTaskMax;
    }

    if (!test_graph.resources.empty())
        for (int i = 0; i < taskCount; i++)
        {
            TestTask &task = test_graph.tasks[i + tasksOffset];
            int resCnt = test_graph.rnd(params.resMin, params.resMax);
            for (int j = 0; j < resCnt && resCnt < 1024; j++)
            {
                const int resId = test_graph.rnd(params.resStart < 0 ? 0 : params.resStart, params.resEnd < 0 ? int(test_graph.resources.size()) : params.resEnd);
                if (graph.getResourceUsage(task.localId, uint64_t(resId)) != ResourceUsage::NOT_USED)
                {
                    resCnt++;
                    continue;
                }
                const ResourceUsage resUsage = test_graph.rndF() < params.resLocking ? ResourceUsage::LOCKING : ResourceUsage::SHARED;
                if (resUsage == ResourceUsage::LOCKING)
                    task.resWrite.push_back(resId);
                else
                    task.resRead.push_back(resId);
                graph.setResourceUsage(task.localId, uint64_t(resId), resUsage);
            }
        }

    for (int i = 0; i < taskCount - 1; i++)
    {
        TestTask &task = test_graph.tasks[i + tasksOffset];
        const int connCnt = test_graph.rnd(params.connMin, params.connMax);
        for (int j = 0; j < connCnt; j++)
        {
            const int nextIdx = test_graph.rnd(tasksOffset + i + 1, int(test_graph.tasks.size()));
            const TaskId nextId = test_graph.tasks[nextIdx].localId;
            graph.setNext(task.localId, nextId);
            task.tasksNext.push_back(nextIdx);
            test_graph.tasks[nextIdx].tasksPrev.push_back(i + tasksOffset);
        }
    }

    return test_graph.rootGraph.addSubGraphTask({}, &graph);
}


void execute_task_graph(SimpleThreadPool &pool, TestTaskGraph &graph, bool use_wait)
{
    printf("[exec] start\n");

    std::vector<uint64_t> times;
    [[maybe_unused]] Array<int64_t, uint8_t(si::tg::ThreadedTaskGraphExecutor::Event::NUM)> stats = {0};

    for (int i = 0; i < 100; i++)
    {
        OPTICK_FRAME("MainThread");
        OPTICK_EVENT()
        const auto beginTime = std::chrono::high_resolution_clock::now();
        graph.execute(pool, use_wait);
        const auto endTime = std::chrono::high_resolution_clock::now();
        times.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - beginTime).count());
        pool.executor.getEventCountStats(stats);
    }

    {
        uint64_t maxNS = 0;
        uint64_t minNS = ~uint64_t(0);
        uint64_t totalNS = 0;
        for (uint64_t t : times)
        {
            maxNS = std::max(maxNS, t);
            minNS = std::min(minNS, t);
            totalNS += t;
        }
        printf("[exec] time: max=%.3lfms min=%.3lfms avg=%.3lfms\n", double(maxNS) * 1e-6, double(minNS) * 1e-6, double(totalNS) / double(times.size()) * 1e-6);
    }
#if SI_TG_ENABLE_DEBUG_STAT_EVENTS
    printf("[validate] stats:");
    for (int i = 0; i < int(ThreadedTaskGraphExecutor::Event::NUM); i++)
        printf("[validate]   %s %lli", ThreadedTaskGraphExecutor::EVENT_NAMES[i], stats[i]);
#endif
}

int main()
{
    OPTICK_START_CAPTURE();

    SimpleThreadPool pool;
    pool.windUpThreads(4);

    srand(1234);
    TASK_GRAPH_VERBOSE_LEVEL = 0;
    for (int i = 0; i < 100; i++)
    {
        printf("[exec] build\n");
        TestTaskGraph graph;
        graph.resources.resize(100);
        add_random_subgraph(graph, {
                .taskCountMin = 1000,
                .varTaskMin = 0,
                .varTaskMax = 5,
                .connMin = 0,
                .connMax = 10,
                .resMin = 0,
                .resMax = 10,
                .resLocking = 0.1f,
        });
        graph.compile();
        execute_task_graph(pool, graph, true);
    }

    pool.shutdownThreads();
    OPTICK_STOP_CAPTURE();
    OPTICK_SAVE_CAPTURE("../test-capture.opt");
    return 0;
}
