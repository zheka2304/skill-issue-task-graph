#include <taskgraph/task_graph_config.h>

#include <cassert>
#include <cstdarg>
#include <cstdio>

namespace si::tg
{
int TASK_GRAPH_VERBOSE_LEVEL = 3;
}

namespace si::tg::internal
{

void assert_handler(const char *str, ...)
{
    va_list args;
    va_start(args, str);
    vfprintf(stderr, str, args);
    putc('\n', stderr);
    va_end(args);
    fflush(stderr);
    assert(0);
}

void log_debug(const char *str, ...)
{
    va_list args;
    va_start(args, str);
    vfprintf(stdout, str, args);
    va_end(args);
}

void log_error(const char *str, ...)
{
    va_list args;
    va_start(args, str);
    vfprintf(stderr, str, args);
    va_end(args);
}

}
