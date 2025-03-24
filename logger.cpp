#include "log_handler.h"

#include <cstring>
#include <memory>
#include <chrono>
#include <mutex>


namespace sie::logger
{

std::unique_ptr<LogFileHandler> default_log_handler = nullptr;
int log_handler_max_stack_alloc = 4096;

LogFileHandler* get_default_log_handler() { return default_log_handler.get(); }

void init_default_log_handler(const char *filename)
{
    shutdown_default_log_handler();
    default_log_handler.reset(new LogFileHandler(filename, 4096));
    // print some info and force file to open
    info("Log", "init log file: %s", filename);
}

void shutdown_default_log_handler()
{
    if (default_log_handler)
        info("Log", "flushing and shutting down logger...");
    flush_default_log();
    default_log_handler.reset();
}

void log_message(LogLevel level, const char* tag, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_message_va(level, {}, tag, fmt, args);
}

void log_message_inline(LogLevel level, const char* tag, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_message_va(level, LogFlag::NONE | LogFlag::INLINE, tag, fmt, args);
}

void log_message_va(LogLevel level, LogFlags flags, const char *tag, const char *fmt, va_list list)
{
    if (default_log_handler)
        default_log_handler->messageV(level, flags | LogFlag::SYSTEM_LOG, tag, fmt, list);
}

void flush_default_log()
{
    if (default_log_handler)
        default_log_handler->flush();
}

LogFileHandler::LogFileHandler(std::string filename, int buffer_size) : filename(std::move(filename))
{
    bufferAllocated = buffer_size;
    buffer = static_cast<char *>(malloc(bufferAllocated));
    bufferUsed = 0;
}

LogFileHandler::~LogFileHandler()
{
    close();
}

int LogFileHandler::allocateBufferNoLock(int required_size)
{
    if (bufferUsed + required_size > bufferAllocated)
    {
        flushNoLock();
        return std::min<int>(required_size, bufferAllocated);
    }
    return required_size;
}

void LogFileHandler::writeStrToFileNoLock(const char *str, int size)
{
    if (file == nullptr && !filename.empty())
        file = fopen(filename.c_str(), "wa");
    if (file != nullptr)
    {
        fwrite(str, 1, size, file);
        fflush(file);
    }
    flushCbs.invokeAll(str, size);
}

void LogFileHandler::flushNoLock()
{
    if (bufferUsed == 0)
        return;
    writeStrToFileNoLock(buffer, bufferUsed);
    bufferUsed = 0;
}

void LogFileHandler::writeStrNoLock(LogLevel level, LogFlags flags, const char* str, int size, bool new_line)
{
    const int required = size + int(new_line);
    if (required == 0)
        return;
    if (flags.test(LogFlag::SYSTEM_LOG))
    {
        FILE *F = int(level) >= int(LogLevel::ERROR) ? stderr : stdout;
        fwrite(str, 1, size, F);
        if (new_line)
            fputc('\n', F);
        fflush(F);
    }
    const int available = allocateBufferNoLock(required);
    if (available == required)
    {
        memcpy(buffer + bufferUsed, str, size);
        bufferUsed += size;
        if (new_line) buffer[bufferUsed++] = '\n';
    }
    else
    {
        // buffer is flushed, if it has overflown
        writeStrToFileNoLock(str, size);
        if (new_line) writeStrNoLock(level, flags, "\n", 1, false); // write new line separately
    }
}

void LogFileHandler::writeMsgNoLock(LogLevel level, LogFlags flags, const char* tag, const char* prefix, int prefix_size, const char* str)
{
    // skip prefix for subsequent inline messages
    bool isInline = flags.test(LogFlag::INLINE);
    const int originalPrefixSize = prefix_size;
    if (isInline && lastMsgInline)
    {
        flags |= LogFlag::NO_PREFIX;
        prefix_size = 0;
    }
    if (lastMsgInline && !isInline) writeStrNoLock(level, flags, "\n", 1, false); // write new line
    lastMsgInline = isInline;

    msgCbs.invokeAll(level, flags, tag, str);

    // fast path - single line
    const char* s = strchr(str, '\n');
    if (s == nullptr)
    {
        // faster path - single buffer for prefix and string
        if (prefix + prefix_size == str)
            writeStrNoLock(level, flags, prefix, prefix_size + strlen(str), !isInline);
        else
        {
            writeStrNoLock(level, flags, prefix, prefix_size, false);
            writeStrNoLock(level, flags, str, strlen(str), !isInline);
        }
        return;
    }
    // slow path - multiline
    writeStrNoLock(level, flags, prefix, prefix_size, false);
    writeStrNoLock(level, flags, str, s - str, true);
    do
    {
        // prefix
        const int available = allocateBufferNoLock(originalPrefixSize);
        for (int i = 0; i < available; i++)
            buffer[bufferUsed + i] = ' ';
        writeStrNoLock(level, flags, buffer + bufferUsed, available, false);
        s++; // skip new line
        const char* next = strchr(s, '\n');
        writeStrNoLock(level, flags, s, next != nullptr ? next - s : strlen(s), next != nullptr || !isInline);
        s = next;
    } while (s != nullptr);
}

static void stack_buff_write_str(char* buffer, int max_size, int *pos, const char* str)
{
    if (*pos >= max_size) return;
    int len = strlen(str);
    len = std::min<int>(len, max_size - 1 - *pos);
    memcpy(buffer + *pos, str, len);
    *pos += len;
}

void LogFileHandler::message(LogLevel level, LogFlags flags, const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    messageV(level, flags, tag, fmt, args);
}

void LogFileHandler::messageV(LogLevel level, LogFlags flags, const char *tag, const char *fmt, va_list list)
{
    constexpr int STACK_BUFFER_SIZE = 1024;
    char stackBuff[STACK_BUFFER_SIZE];
    int stackBuffPos = 0;

    // format tag & stuff
    if (!flags.test(LogFlag::NO_PREFIX))
    {
        constexpr int FMT_BUFFER_SIZE = 128;
        char fmtBuff[FMT_BUFFER_SIZE];

        // time
        if (flags.test(LogFlag::TIME))
        {
            const float time = 0.0;
            snprintf(fmtBuff, FMT_BUFFER_SIZE, "%2.2f ", time);
            stack_buff_write_str(stackBuff, STACK_BUFFER_SIZE - 16, &stackBuffPos, fmtBuff);
        }
        // level
        {
            constexpr char CHAR_BY_LEVEL[] = {'V', 'D', 'I', 'W', 'E', 'F'};
            stackBuff[stackBuffPos++] = '[';
            stackBuff[stackBuffPos++] = CHAR_BY_LEVEL[int(level)];
            stackBuff[stackBuffPos++] = ']';
        }
        // tag
        if (tag)
        {
            stackBuff[stackBuffPos++] = ' ';
            stack_buff_write_str(stackBuff, STACK_BUFFER_SIZE - 16, &stackBuffPos, tag);
            stackBuff[stackBuffPos++] = ':';
        }
        stackBuff[stackBuffPos++] = ' ';
    }

    // no-fmt case, just print fmt as message
    if (flags.test(LogFlag::NO_FMT))
    {
        lock.lock();
        writeMsgNoLock(level, flags, tag, stackBuff, stackBuffPos, fmt);
        lock.unlock();
        return;
    }

    // write tag & stuff
    int required = vsnprintf(stackBuff + stackBuffPos, STACK_BUFFER_SIZE - stackBuffPos, fmt, list);
    if (required < 0)
        return; // failed for some reason

    // fast path - message fits into the stack buffer
    if (required <= STACK_BUFFER_SIZE - stackBuffPos)
    {
        lock.lock();
        writeMsgNoLock(level, flags, tag, stackBuff, stackBuffPos, stackBuff + stackBuffPos);
        lock.unlock();
        return;
    }

    // slow path - message does not fit into the stack buffer
    bool useStackAlloc = required < log_handler_max_stack_alloc;
    char *buff = static_cast<char *>(useStackAlloc ? alloca(required + 1) : malloc(required + 1));
    vsnprintf(buff, required + 1, fmt, list);
    lock.lock();
    writeMsgNoLock(level, flags, tag, stackBuff, stackBuffPos, buff);
    lock.unlock();
    if (!useStackAlloc) free(buff);
}

void LogFileHandler::flush()
{
    lock.lock();
    flushNoLock();
    lock.unlock();
}

void LogFileHandler::close()
{
    lock.lock();
    flushNoLock();
    if (file != nullptr)
    {
        fclose(file);
        file = nullptr;
    }
    lastMsgInline = false;
    lock.unlock();
}

void LogFileHandler::addMsgCb(LogFileHandler::MsgCbPtr cb, void *user_data)
{
    std::unique_lock<std::mutex> l(lock);
    msgCbs.add(cb, user_data);
}

void LogFileHandler::removeMsgCb(LogFileHandler::MsgCbPtr cb, void *user_data)
{
    std::unique_lock<std::mutex> l(lock);
    msgCbs.remove(cb, user_data);
}

void LogFileHandler::addFlushCb(LogFileHandler::FlushCbPtr cb, void *user_data)
{
    std::unique_lock<std::mutex> l(lock);
    flushCbs.add(cb, user_data);
}

void LogFileHandler::removeFlushCb(LogFileHandler::FlushCbPtr cb, void *user_data)
{
    std::unique_lock<std::mutex> l(lock);
    flushCbs.add(cb, user_data);
}

#define HZ_LOG_MESSAGE_IMPL(level) \
    va_list args; \
    va_start(args, message);    \
    log_message_va(level, {}, tag, message, args);
void verbose(const char* tag, const char* message, ...) { HZ_LOG_MESSAGE_IMPL(LogLevel::VERBOSE); }
void debug(const char* tag, const char* message, ...) { HZ_LOG_MESSAGE_IMPL(LogLevel::DEBUG); }
void debug_inline(const char* tag, const char* message, ...)
{
    va_list args;
    va_start(args, message);
    log_message_va(LogLevel::DEBUG, LogFlag::INLINE, tag, message, args);
}
void message(const char* tag, const char* message, ...) { HZ_LOG_MESSAGE_IMPL(LogLevel::INFO); }
void info(const char* tag, const char* message, ...) { HZ_LOG_MESSAGE_IMPL(LogLevel::INFO); }
void error(const char* tag, const char* message, ...) { HZ_LOG_MESSAGE_IMPL(LogLevel::ERROR); }
void fatal(const char* tag, const char* message, ...) { HZ_LOG_MESSAGE_IMPL(LogLevel::FATAL); flush_default_log(); }
#undef HZ_LOG_MESSAGE_IMPL

}