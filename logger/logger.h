#pragma once

#include <cstdarg>
#include <cstdint>
#include "flag_enum.h"


namespace sie::logger
{

enum class LogLevel : uint8_t
{
    VERBOSE = 0,
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

enum class LogFlag : uint16_t
{
    NONE = 0u,
    NO_PREFIX = 1u << 0u,
    TIME = 1u << 1u,
    SYSTEM_LOG = 1u << 2u,
    NO_FMT = 1u << 3u,
    INLINE = 1u << 4u
};
using LogFlags = FlagEnum<LogFlag>;

void init_default_log_handler(const char *filename);
void shutdown_default_log_handler();
void flush_default_log();

void log_message(LogLevel level, const char* tag, const char* fmt, ...);
void log_message_inline(LogLevel level, const char* tag, const char* fmt, ...);
void log_message_va(LogLevel level, LogFlags flags, const char* tag, const char* fmt, va_list list);

void verbose(const char* tag, const char* message, ...);
void debug(const char* tag, const char* message, ...);
void debug_inline(const char* tag, const char* message, ...);
void message(const char* tag, const char* message, ...);
void info(const char* tag, const char* message, ...);
void error(const char* tag, const char* message, ...);
void fatal(const char* tag, const char* message, ...);

}