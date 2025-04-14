#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <cassert>

#define SI_TG_ASSERT assert
#define SI_TG_ASSERT_FMT(E, ...) assert(E)

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

}
