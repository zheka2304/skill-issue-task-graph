#pragma once

#include <vector>
#include <string>
#include <thread>
#include "generic_callback_list.h"
#include "logger.h"

namespace sie::logger
{

struct LogFileHandler
{
    LogFileHandler(std::string filename, int buffer_size);
    ~LogFileHandler();
    LogFileHandler(const LogFileHandler&) = delete;
    LogFileHandler& operator=(const LogFileHandler&) = delete;

    void messageV(LogLevel level, LogFlags flags, const char* tag, const char* fmt, va_list list);
    void message(LogLevel level, LogFlags flags, const char* tag, const char* fmt, ...);
    void flush();
    void close();

    using FlushCbPtr = void (*) (void* user_data, const char *str, int size);
    using MsgCbPtr = void (*) (void* user_data, LogLevel level, LogFlags flags, const char *tag, const char *msg);
    void addMsgCb(MsgCbPtr cb, void* user_data);
    void removeMsgCb(MsgCbPtr cb, void* user_data);
    void addFlushCb(FlushCbPtr cb, void* user_data);
    void removeFlushCb(FlushCbPtr cb, void* user_data);

private:
    int allocateBufferNoLock(int size);
    void writeStrToFileNoLock(const char* str, int size);
    void writeStrNoLock(LogLevel level, LogFlags flags, const char* str, int size, bool new_line);
    void writeMsgNoLock(LogLevel level, LogFlags flags, const char* tag, const char *prefix, int prefix_size, const char* str);
    void flushNoLock();

    std::string filename;
    FILE* file = nullptr;

    int bufferAllocated = 0;
    int bufferUsed = 0;
    bool lastMsgInline = false;
    char *buffer = nullptr;
    std::mutex lock;

    GenericCallbackList<MsgCbPtr, void*> msgCbs;
    GenericCallbackList<FlushCbPtr, void*> flushCbs;
};

LogFileHandler *get_default_log_handler();

}