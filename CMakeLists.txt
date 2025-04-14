cmake_minimum_required(VERSION 3.25)

project(skill_issue_task_graph)


set(SRC_TASK_GRAPH
        src/task_graph_compile.cpp
        src/task_graph_execute.cpp
        src/task_graph_config.cpp
        src/simple_thread_pool.cpp)

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

set(SRC_TEST
        test/main.cpp)


set(CMAKE_CXX_STANDARD 20)

add_compile_options(
        -O3
        -fno-exceptions
        -DSI_TG_USE_OPTICK
        -Wall -Wextra -Wpedantic -Werror
        -Wno-gnu-zero-variadic-macro-arguments
)

set_source_files_properties(${SRC_OPTICK} ${SRC_TEST} PROPERTIES COMPILE_FLAGS -Wno-error)


add_executable(skill_issue_task_graph
        ${SRC_OPTICK}
        ${SRC_TASK_GRAPH}
        ${SRC_TEST})

target_include_directories(skill_issue_task_graph PUBLIC optick include)

target_link_libraries(skill_issue_task_graph PUBLIC ws2_32 DbgHelp)