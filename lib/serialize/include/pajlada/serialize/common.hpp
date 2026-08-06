#pragma once

#include <rapidjson/document.h>

#include <cmath>

#ifndef PAJLADA_REPORT_ERROR
#define PAJLADA_REPORT_ERROR(x) \
    if (x != nullptr) {         \
        *x = true;              \
    }
#endif

#define PAJLADA_ROUNDING_METHOD_ROUND 0
#define PAJLADA_ROUNDING_METHOD_CEIL 1
#define PAJLADA_ROUNDING_METHOD_FLOOR 2

// valid values: ceil/floor/round
// default: round
#ifndef PAJLADA_ROUNDING_METHOD
#define PAJLADA_ROUNDING_METHOD PAJLADA_ROUNDING_METHOD_ROUND
#endif

namespace pajlada {

namespace detail {

template <typename T1, typename T2>
typename std::enable_if<std::is_integral<T1>::value, T1>::type
Round(T2 value)
{
#if PAJLADA_ROUNDING_METHOD == PAJLADA_ROUNDING_METHOD_ROUND
    return static_cast<T1>(round(value));
#elif PAJLADA_ROUNDING_METHOD == PAJLADA_ROUNDING_METHOD_CEIL
    return static_cast<T1>(ceil(value));
#elif PAJLADA_ROUNDING_METHOD == PAJLADA_ROUNDING_METHOD_FLOOR
    return static_cast<T1>(floor(value));
#else
    static_assert(
        "Invalid rounding method selected in PAJLADA_ROUNDING_METHOD");
#endif
}

template <
    typename Type, typename RJValue = rapidjson::Value,
    typename std::enable_if<std::is_integral<Type>::value>::type * = nullptr>
Type
GetNumber(const RJValue &value)
{
    if (value.IsDouble()) {
        return Round<Type>(value.GetDouble());
    }
    if (value.IsFloat()) {
        return Round<Type>(value.GetFloat());
    }
    if (value.IsInt()) {
        return value.GetInt();
    }
    if (value.IsUint()) {
        return static_cast<Type>(value.GetUint());
    }
    if (value.IsInt64()) {
        return static_cast<Type>(value.GetInt64());
    }
    if (value.IsUint64()) {
        return static_cast<Type>(value.GetUint64());
    }

    return Type{};
}

}  // namespace detail

}  // namespace pajlada
