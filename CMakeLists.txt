cmake_minimum_required(VERSION 3.25)
project(ecs_scheduler)

set(CMAKE_CXX_STANDARD 20)

add_executable(ecs_scheduler
        taskgraph/task_graph_compile.cpp
        taskgraph/task_graph_execute.cpp
        taskgraph/task_graph_config.cpp
        taskgraph/simple_thread_pool.cpp

        main.cpp
        logger/logger.cpp

        optick/optick_capi.cpp
        optick/optick_core.cpp
        optick/optick_gpu.cpp
        optick/optick_gpu.vulkan.cpp
        optick/optick_gpu.d3d12.cpp
        optick/optick_message.cpp
        optick/optick_miniz.cpp
        optick/optick_serialization.cpp
        optick/optick_server.cpp
)

target_include_directories(ecs_scheduler PUBLIC optick)
target_link_libraries(ecs_scheduler PUBLIC ws2_32 DbgHelp)