#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cassert>

void assert_handler(const char *fmt);
#define ASSERT_STRING_BUILD0(FILE, LINE) "ASSERT FAILED " #FILE ":" #LINE ": "
#define ASSERT_STRING_BUILD(FILE, LINE) ASSERT_STRING_BUILD0(FILE, LINE)

#define SI_TG_ASSERT(E) do { if (!(E)) { assert_handler(ASSERT_STRING_BUILD(__FILE__, __LINE__) #E); } } while (0)
#define SI_TG_ASSERT_FMT(E, ...) SI_TG_ASSERT(E)

#define SI_TG_ENABLE_DEBUG_STAT_EVENTS 0
#define SI_TG_ENABLE_DEBUG_TIMED_EVENTS 0

#ifdef __clang__
#define SI_TG_FIRST_SET_BIT __builtin_ctzll
#define SI_TG_COUNT_SET_BITS __builtin_popcountll
#endif

namespace si::tg
{

template<typename T>
using Vector = std::vector<T>;

template<typename T, int N, bool AllowOverflow>
using FixedVector = std::vector<T>;

template<typename K, typename V, typename Hash>
using FlatHashMultiMap = std::unordered_multimap<K, V, Hash>;

using WakeThreadsCallback = std::function<void(uint64_t mask)>;

}
