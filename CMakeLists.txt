cmake_minimum_required(VERSION 3.25)
project(ecs_scheduler)


set(SRC_TASK_GRAPH
        src/task_graph_compile.cpp
        src/task_graph_execute.cpp
        src/task_graph_config.cpp
        src/simple_thread_pool.cpp)

set(SRC_LOGGER
        logger/logger.cpp)

set(SRC_OPTICK
        optick/optick_capi.cpp
        optick/optick_core.cpp
        optick/optick_gpu.cpp
        optick/optick_gpu.vulkan.cpp
        optick/optick_gpu.d3d12.cpp
        optick/optick_message.cpp
        optick/optick_miniz.cpp
        optick/optick_serialization.cpp
        optick/optick_server.cpp)


set(CMAKE_CXX_STANDARD 20)

add_compile_options(
        -O3
        -DSI_TG_USE_OPTICK
        -Wall -Wextra -Wpedantic -Werror
        -Wno-gnu-zero-variadic-macro-arguments
)

set_source_files_properties(${SRC_OPTICK} PROPERTIES COMPILE_FLAGS -Wno-error)

add_executable(ecs_scheduler
        ${SRC_OPTICK}
        ${SRC_LOGGER}
        ${SRC_TASK_GRAPH}
        main.cpp)

target_include_directories(ecs_scheduler PUBLIC optick include)

target_link_libraries(ecs_scheduler PUBLIC ws2_32 DbgHelp)