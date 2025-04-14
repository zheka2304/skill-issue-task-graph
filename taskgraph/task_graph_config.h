#pragma once

#include <cstdint>
#include <array>
#include <vector>
#include <unordered_map>
#include <functional>


namespace si::tg::internal
{
void log_debug(const char *str, ...);
void log_error(const char *str, ...);
void assert_handler(const char* fmt, ...);
}

#define SI_TG_ASSERT_MESSAGE_BUILD0(FILE, LINE) "ASSERT FAILED " #FILE ":" #LINE ": "
#define SI_TG_ASSERT_MESSAGE_BUILD(FILE, LINE) SI_TG_ASSERT_MESSAGE_BUILD0(FILE, LINE)
#define SI_TG_ASSERT_FMT(E, FMT, ...) do { if (!(E)) { ::si::tg::internal::assert_handler(SI_TG_ASSERT_MESSAGE_BUILD(__FILE__, __LINE__) #E FMT, ##__VA_ARGS__); } } while (0)
#define SI_TG_ASSERT(E) SI_TG_ASSERT_FMT(E, "")

#define SI_TG_ENABLE_DEBUG_STAT_EVENTS 0

#define SI_TG_ENABLE_DEBUG_LOG_EVENTS 0
#define SI_TG_DEBUG_LOG_SKIP_EVENTS_LIST(X) \
    X(SUBGROUP_ENTER_ATTEMPT)               \
    X(SUBGROUP_ENTER_CAS)                   \
    X(PENDING_INC_DEPENDENCY)               \
    X(THREAD_WAIT)                          \
    X(THREAD_NOTHING_PENDING)               \
    X(THREAD_START_GROUP)                   \
    X(THREAD_EXIT)

#define SI_TG_ENABLE_DEBUG_TIMED_EVENTS 0
#define SI_TG_DEBUG_TIMED_EVENTS_LIST(X) \
    X(TASK_EXECUTE_START)                \
    X(TASK_EXECUTE_END)

#ifdef __clang__
#define SI_TG_FIRST_SET_BIT __builtin_ctzll
#define SI_TG_COUNT_SET_BITS __builtin_popcountll
#endif

#define SI_TG_PROFILE_THREAD(...) // OPTICK_EVENT(__VA_ARGS__)
#define SI_TG_PROFILE_INTERNAL(...) // OPTICK_EVENT(__VA_ARGS__)
#define SI_TG_PROFILE_EXCESSIVE(...) // OPTICK_EVENT(__VA_ARGS__)


namespace si::tg
{
template<typename T, size_t N>
using Array = std::array<T, N>;

template<typename T>
using Vector = std::vector<T>;

template<typename T, size_t N, bool AllowOverflow>
using FixedVector = std::vector<T>;

template<typename K, typename V, typename Hash>
using FlatHashMultiMap = std::unordered_multimap<K, V, Hash>;

using WakeThreadsCallback = std::function<void(uint64_t mask)>;
}
