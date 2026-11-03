#ifndef CPP2TRY_H_CPP2
#define CPP2TRY_H_CPP2

// The runtime of the `try-operator` experiment: `r?` returns
// `cpp2::try_failure(r)` from the enclosing function when `r` has no value.

#include "cpp2util.h"

#include <optional>
#include <utility>
#include <version>
#if defined(__cpp_lib_expected)
#include <expected>
#endif

namespace cpp2 {

// The operand of `e?`, held by `auto&&`: an lvalue stays a reference, and an
// rvalue, which may be an xvalue into a temporary, is moved into a value.
template<class T>
[[nodiscard]] constexpr auto try_hold(T&& x) -> T {
    return std::forward<T>(x);
}

#if defined(__cpp_lib_expected)
template<typename T, typename E>
[[nodiscard]] auto try_failure(std::expected<T, E>&& r) -> std::unexpected<E> {
    return std::unexpected<E>(std::move(r).error());
}

template<typename T, typename E>
[[nodiscard]] auto try_failure(std::expected<T, E> const& r) -> std::unexpected<E> {
    return std::unexpected<E>(r.error());
}
#endif

template<typename T>
[[nodiscard]] auto try_failure(std::optional<T> const&) -> std::nullopt_t {
    return std::nullopt;
}

}

#endif
