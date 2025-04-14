// The MIT License(MIT)
//
// Copyright(c) 2025 Eugene Smirnov
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

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
