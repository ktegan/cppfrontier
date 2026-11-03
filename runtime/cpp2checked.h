//  Checked integer arithmetic for the `checked-arithmetic` experiment.
//
//  `cpp2::checked_add(a, b)` and the other helpers below compute an integer
//  result in the type C++ gives `a + b` (the usual arithmetic conversions)
//  and report an `arithmetic_safety` contract violation when the mathematical
//  result does not fit that type. The compound forms store into their first
//  operand and check against its type. `cpp2::wrapping_add(a, b)` and the
//  other `wrapping_*` helpers compute the same type's result modulo 2^N, for
//  arithmetic that is meant to wrap.

#ifndef CPP2CHECKED_H_INCLUDED
#define CPP2CHECKED_H_INCLUDED

#include "cpp2util.h"

#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

//  The overflow builtins of GCC and Clang; defining CPP2_CHECKED_HAS_BUILTINS
//  as 0 selects the portable checks instead.
#ifndef CPP2_CHECKED_HAS_BUILTINS
    #if defined(__GNUC__) || defined(__clang__)
        #define CPP2_CHECKED_HAS_BUILTINS 1
    #else
        #define CPP2_CHECKED_HAS_BUILTINS 0
    #endif
#endif

namespace cpp2 {

auto inline arithmetic_safety = contract_group(
    [](CPP2_MESSAGE_PARAM msg CPP2_SOURCE_LOCATION_PARAM)noexcept {
        report_and_terminate("Arithmetic safety", msg CPP2_SOURCE_LOCATION_ARG);
    }
);

namespace impl {

//  The portable checks compute the mathematical result exactly, as a sign and
//  a magnitude, and then test whether it fits `R`.
struct exact final {
    bool negative = false;
    std::uintmax_t magnitude = 0;
    //  The magnitude overflowed `std::uintmax_t`.
    bool overflow = false;
};

template<std::integral T>
constexpr auto exact_of(T value) noexcept -> exact {
    if constexpr (std::is_signed_v<T>) {
        if (value < 0) return {true, std::uintmax_t{0} - static_cast<std::uintmax_t>(value)};
    }
    return {false, static_cast<std::uintmax_t>(value)};
}

constexpr auto exact_add(exact a, exact b) noexcept -> exact {
    if (a.negative == b.negative) {
        const auto sum = a.magnitude + b.magnitude;
        return {a.negative && sum != 0, sum, sum < a.magnitude};
    }
    if (a.magnitude >= b.magnitude) {
        return {a.negative && a.magnitude != b.magnitude, a.magnitude - b.magnitude};
    }
    return {b.negative, b.magnitude - a.magnitude};
}

constexpr auto exact_negate(exact a) noexcept -> exact {
    return {!a.negative && a.magnitude != 0, a.magnitude};
}

constexpr auto exact_mul(exact a, exact b) noexcept -> exact {
    const auto product = a.magnitude * b.magnitude;
    const auto overflow = a.magnitude != 0 && product / a.magnitude != b.magnitude;
    return {a.negative != b.negative && product != 0, product, overflow};
}

//  Stores `value` into `result` when it fits `R`; returns whether it does not.
template<std::integral R>
constexpr auto store_overflows(exact value, R& result) noexcept -> bool {
    using limits = std::numeric_limits<R>;
    if (value.overflow) return true;
    if (value.negative) {
        if constexpr (std::is_signed_v<R>) {
            if (value.magnitude > std::uintmax_t{0} - static_cast<std::uintmax_t>(limits::min())) {
                return true;
            }
            result = static_cast<R>(
                static_cast<std::intmax_t>(std::uintmax_t{0} - value.magnitude));
            return false;
        }
        return true;
    }
    if (value.magnitude > static_cast<std::uintmax_t>(limits::max())) return true;
    result = static_cast<R>(value.magnitude);
    return false;
}

//  Each `*_overflows` computes `a op b` into `result` and returns whether the
//  mathematical value does not fit `R`. The builtins take no `bool` result.

template<std::integral R, std::integral A, std::integral B>
constexpr auto add_overflows(A a, B b, R& result) noexcept -> bool {
#if CPP2_CHECKED_HAS_BUILTINS
    if constexpr (!std::same_as<R, bool>) return __builtin_add_overflow(a, b, &result);
#endif
    return store_overflows(exact_add(exact_of(a), exact_of(b)), result);
}

template<std::integral R, std::integral A, std::integral B>
constexpr auto sub_overflows(A a, B b, R& result) noexcept -> bool {
#if CPP2_CHECKED_HAS_BUILTINS
    if constexpr (!std::same_as<R, bool>) return __builtin_sub_overflow(a, b, &result);
#endif
    return store_overflows(exact_add(exact_of(a), exact_negate(exact_of(b))), result);
}

template<std::integral R, std::integral A, std::integral B>
constexpr auto mul_overflows(A a, B b, R& result) noexcept -> bool {
#if CPP2_CHECKED_HAS_BUILTINS
    if constexpr (!std::same_as<R, bool>) return __builtin_mul_overflow(a, b, &result);
#endif
    return store_overflows(exact_mul(exact_of(a), exact_of(b)), result);
}

} // namespace impl

template<std::integral A, std::integral B>
constexpr auto checked_add(A a, B b CPP2_SOURCE_LOCATION_PARAM_WITH_DEFAULT)
    -> decltype(a + b)
{
    auto result = decltype(a + b){};
    if (impl::add_overflows(a, b, result)) {
        arithmetic_safety.report_violation("integer addition overflow" CPP2_SOURCE_LOCATION_ARG);
    }
    return result;
}

template<std::integral A, std::integral B>
constexpr auto checked_sub(A a, B b CPP2_SOURCE_LOCATION_PARAM_WITH_DEFAULT)
    -> decltype(a - b)
{
    auto result = decltype(a - b){};
    if (impl::sub_overflows(a, b, result)) {
        arithmetic_safety.report_violation("integer subtraction overflow" CPP2_SOURCE_LOCATION_ARG);
    }
    return result;
}

template<std::integral A, std::integral B>
constexpr auto checked_mul(A a, B b CPP2_SOURCE_LOCATION_PARAM_WITH_DEFAULT)
    -> decltype(a * b)
{
    auto result = decltype(a * b){};
    if (impl::mul_overflows(a, b, result)) {
        arithmetic_safety.report_violation("integer multiplication overflow" CPP2_SOURCE_LOCATION_ARG);
    }
    return result;
}

//  `-a`, promoted as C++ promotes it; an unsigned operand overflows unless zero.
template<std::integral A>
constexpr auto checked_neg(A a CPP2_SOURCE_LOCATION_PARAM_WITH_DEFAULT) -> decltype(+a)
{
    auto result = decltype(+a){};
    if (impl::sub_overflows(0, a, result)) {
        arithmetic_safety.report_violation("integer negation overflow" CPP2_SOURCE_LOCATION_ARG);
    }
    return result;
}

template<std::integral A, std::integral B>
constexpr auto checked_add_assign(A& a, B b CPP2_SOURCE_LOCATION_PARAM_WITH_DEFAULT) -> A&
{
    if (impl::add_overflows(a, b, a)) {
        arithmetic_safety.report_violation("integer addition overflow" CPP2_SOURCE_LOCATION_ARG);
    }
    return a;
}

template<std::integral A, std::integral B>
constexpr auto checked_sub_assign(A& a, B b CPP2_SOURCE_LOCATION_PARAM_WITH_DEFAULT) -> A&
{
    if (impl::sub_overflows(a, b, a)) {
        arithmetic_safety.report_violation("integer subtraction overflow" CPP2_SOURCE_LOCATION_ARG);
    }
    return a;
}

template<std::integral A, std::integral B>
constexpr auto checked_mul_assign(A& a, B b CPP2_SOURCE_LOCATION_PARAM_WITH_DEFAULT) -> A&
{
    if (impl::mul_overflows(a, b, a)) {
        arithmetic_safety.report_violation("integer multiplication overflow" CPP2_SOURCE_LOCATION_ARG);
    }
    return a;
}

//  The result of `a op b` in R, modulo 2^N: computed in R's unsigned
//  counterpart, where wrapping is defined, and converted back.
template<std::integral A, std::integral B>
constexpr auto wrapping_add(A a, B b) noexcept -> decltype(a + b)
{
    using R = decltype(a + b);
    using U = std::make_unsigned_t<R>;
    return static_cast<R>(static_cast<U>(static_cast<R>(a)) + static_cast<U>(static_cast<R>(b)));
}

template<std::integral A, std::integral B>
constexpr auto wrapping_sub(A a, B b) noexcept -> decltype(a - b)
{
    using R = decltype(a - b);
    using U = std::make_unsigned_t<R>;
    return static_cast<R>(static_cast<U>(static_cast<R>(a)) - static_cast<U>(static_cast<R>(b)));
}

template<std::integral A, std::integral B>
constexpr auto wrapping_mul(A a, B b) noexcept -> decltype(a * b)
{
    using R = decltype(a * b);
    using U = std::make_unsigned_t<R>;
    return static_cast<R>(static_cast<U>(static_cast<R>(a)) * static_cast<U>(static_cast<R>(b)));
}

} // namespace cpp2

#endif
