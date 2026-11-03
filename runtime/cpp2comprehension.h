//  Runtime support for for-comprehensions (experiment `comprehension`).
//
//  `cpp2::comprehend(source, [guard,] map)` is the one call shape the
//  translator emits for a generator; the overloads decide what the source is.
//  A range becomes a lazy `std::views::filter | std::views::transform`
//  pipeline, and a type with `.and_then` and `.transform` members, such as
//  `std::optional` and `std::expected`, uses that monadic interface.
//  `cpp2::comprehend_bind(source, [guard,] f)` is the same for a generator
//  whose body is another comprehension: `transform | join` for ranges,
//  `and_then` for monads.

#ifndef CPP2COMPREHENSION_H_INCLUDED
#define CPP2COMPREHENSION_H_INCLUDED

#include <concepts>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

namespace cpp2 {

namespace impl {

//  Callables for the concept below, declared only: they neither copy nor
//  construct the element.
struct transform_probe final {
    template<class T> auto operator()(T&&) const -> int;
};

template<class M>
struct and_then_probe final {
    template<class T> auto operator()(T&&) const -> M;
};

//  A type the monadic lowering applies to: it has `.and_then` and
//  `.transform` members and is not a range.
template<class M>
concept comprehension_monad =
    !std::ranges::range<std::remove_cvref_t<M>> &&
    requires(M&& m) {
        std::forward<M>(m).transform(transform_probe{});
        std::forward<M>(m).and_then(and_then_probe<std::remove_cvref_t<M>>{});
    };

//  A range source: one a view can hold, or an lvalue, such as a move-only
//  view passed as an `in` parameter, that the view refers to.
template<class M>
concept comprehension_range =
    (std::ranges::viewable_range<M> || (std::is_lvalue_reference_v<M> && std::ranges::range<M>)) &&
    !comprehension_monad<M>;

template<comprehension_range R>
[[nodiscard]] auto comprehension_view(R&& source) {
    if constexpr (std::ranges::viewable_range<R>) return std::views::all(std::forward<R>(source));
    else return std::ranges::ref_view{source};
}

//  `m.and_then(f)` where `f` runs only when `guard` holds: an empty result is
//  produced otherwise, which only an optional-like type (constructible from
//  `std::nullopt`) has. `f` returns the monad.
template<class M, class Guard, class F>
[[nodiscard]] auto guarded_and_then(M&& m, Guard&& guard, F&& f) -> decltype(auto) {
    return std::forward<M>(m).and_then([&](auto&& value) {
        using result = std::remove_cvref_t<decltype(f(std::forward<decltype(value)>(value)))>;
        static_assert(std::is_constructible_v<result, std::nullopt_t>,
            "a for-comprehension 'if' guard needs an optional source: an expected "
            "has no empty value to produce when the guard fails");
        if (guard(std::as_const(value))) return result{f(std::forward<decltype(value)>(value))};
        return result{std::nullopt};
    });
}

} // namespace impl

//  `for source do (x) yield map(x)`
template<impl::comprehension_range R, class F>
[[nodiscard]] auto comprehend(R&& source, F&& map) -> decltype(auto) {
    return std::views::transform(impl::comprehension_view(std::forward<R>(source)),
                                 std::forward<F>(map));
}

template<impl::comprehension_monad M, class F>
[[nodiscard]] auto comprehend(M&& source, F&& map) -> decltype(auto) {
    return std::forward<M>(source).transform(std::forward<F>(map));
}

//  `for source do (x) if guard(x) yield map(x)`
template<impl::comprehension_range R, class Guard, class F>
[[nodiscard]] auto comprehend(R&& source, Guard&& guard, F&& map) -> decltype(auto) {
    return std::views::transform(
        std::views::filter(impl::comprehension_view(std::forward<R>(source)),
                           std::forward<Guard>(guard)),
        std::forward<F>(map));
}

//  The mapped value is constructed in the result, `m.transform(map)`'s type.
template<impl::comprehension_monad M, class Guard, class F>
[[nodiscard]] auto comprehend(M&& source, Guard&& guard, F&& map) -> decltype(auto) {
    using result = decltype(std::forward<M>(source).transform(std::forward<F>(map)));
    return impl::guarded_and_then(std::forward<M>(source), std::forward<Guard>(guard),
        [&](auto&& value) {
            return result{std::in_place, map(std::forward<decltype(value)>(value))};
        });
}

//  `for source do (x) for ...`: `f(x)` is the inner comprehension.
template<impl::comprehension_range R, class F>
[[nodiscard]] auto comprehend_bind(R&& source, F&& f) -> decltype(auto) {
    return std::views::join(std::views::transform(
        impl::comprehension_view(std::forward<R>(source)), std::forward<F>(f)));
}

template<impl::comprehension_monad M, class F>
[[nodiscard]] auto comprehend_bind(M&& source, F&& f) -> decltype(auto) {
    return std::forward<M>(source).and_then(std::forward<F>(f));
}

//  `for source do (x) if guard(x) for ...`
template<impl::comprehension_range R, class Guard, class F>
[[nodiscard]] auto comprehend_bind(R&& source, Guard&& guard, F&& f) -> decltype(auto) {
    return std::views::join(std::views::transform(
        std::views::filter(impl::comprehension_view(std::forward<R>(source)),
                           std::forward<Guard>(guard)),
        std::forward<F>(f)));
}

template<impl::comprehension_monad M, class Guard, class F>
[[nodiscard]] auto comprehend_bind(M&& source, Guard&& guard, F&& f) -> decltype(auto) {
    return impl::guarded_and_then(std::forward<M>(source), std::forward<Guard>(guard),
                                  std::forward<F>(f));
}

} // namespace cpp2

#endif
