#pragma once

#include <type_traits>

namespace sie
{

template<typename EnumT>
struct FlagEnum
{
    using EnumType = EnumT;
    using FlagsType = std::underlying_type_t<EnumT>;

    FlagEnum() = default;
    FlagEnum(EnumT val) : value(FlagsType(val)) {}
    explicit FlagEnum(FlagsType val) : value(val) {}

    FlagsType getValue() const { return value; }
    explicit operator FlagsType() const { return value; }
    bool test(EnumT e) { return bool(value & FlagsType(e)); }
    bool testAny(FlagEnum v) { return bool(value & v.value); }
    bool testAll(FlagEnum v) { return (value & v.value) == v.value; }

    void setFlag(EnumT e, bool set = true)
    {
        if (set)
            value |= FlagsType(e);
        else
            value &= ~FlagsType(e);
    }
    FlagEnum &operator|=(EnumT e) { setFlag(e); return *this; }

private:
    FlagsType value = 0;
};

template<typename EnumT>
FlagEnum<EnumT> operator|(FlagEnum<EnumT> lhs, FlagEnum<EnumT> rhs) { return FlagEnum<EnumT>(lhs.getValue() | rhs.getValue()); }
template<typename EnumT>
FlagEnum<EnumT> operator|(EnumT lhs, FlagEnum<EnumT> rhs) { return FlagEnum<EnumT>(lhs) | rhs; }
template<typename EnumT>
FlagEnum<EnumT> operator|(FlagEnum<EnumT> lhs, EnumT rhs) { return lhs | FlagEnum<EnumT>(rhs); }
template<typename EnumT>
FlagEnum<EnumT> operator|(EnumT lhs, EnumT rhs) { return FlagEnum<EnumT>(lhs) | FlagEnum<EnumT>(rhs); }

}
