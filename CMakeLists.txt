cmake_minimum_required(VERSION 3.25)
project(ecs_scheduler)

set(CMAKE_CXX_STANDARD 20)

add_executable(ecs_scheduler main.cpp thread_pool.cpp logger.cpp task_graph_compile.cpp task_graph_execute.cpp)
