
//  cpp2units.h: the runtime for the `@unit` metafunction.
//
//  `Length: @unit<f32, 1> type = { }` lowers to the alias
//  `cpp2::quantity<float, cpp2::dims<1, 0, 0, 0, 0, 0, 0>>`. A quantity holds
//  one value of its representation; the exponents of its dimensions are added
//  by `*` and subtracted by `/`, so the product of two lengths is an area and
//  a type mismatch is a Cpp1 compile error. A quantity with no dimension
//  converts implicitly to its representation; every other one only explicitly,
//  or with `as`.

#ifndef CPP2UNITS_H_INCLUDED
#define CPP2UNITS_H_INCLUDED

#include <compare>
#include <concepts>

namespace cpp2 {

//  The exponent of each base dimension, in the order of the SI base units:
//  length, mass, time, current, temperature, amount of substance, luminous intensity.
template<int... Exponents>
struct dims { };

template<typename T, typename Dims>
class quantity;

template<typename T, int... Es>
class quantity<T, dims<Es...>>
{
    T value_ {};

    static constexpr bool dimensionless = ((Es == 0) && ...);

public:
    using rep = T;
    using cpp2_newtype_rep = T;
    using dimensions = dims<Es...>;

    constexpr quantity() = default;
    constexpr explicit quantity(T value) : value_{value} { }

    [[nodiscard]] constexpr auto value() const -> T { return value_; }

    constexpr explicit operator T() const requires (!dimensionless) { return value_; }
    constexpr operator T() const requires dimensionless { return value_; }

    //  `q as T`, found by argument-dependent lookup from `cpp2::impl::as_`.
    template<typename C>
        requires std::same_as<C, T>
    friend constexpr auto as(quantity const& q) -> T { return q.value_; }

    friend constexpr auto operator<=>(quantity const&, quantity const&) = default;

    friend constexpr auto operator+(quantity const& a, quantity const& b) -> quantity {
        return quantity{a.value_ + b.value_};
    }
    friend constexpr auto operator-(quantity const& a, quantity const& b) -> quantity {
        return quantity{a.value_ - b.value_};
    }
    friend constexpr auto operator-(quantity const& a) -> quantity { return quantity{-a.value_}; }
    friend constexpr auto operator*(quantity const& a, T k) -> quantity {
        return quantity{a.value_ * k};
    }
    friend constexpr auto operator*(T k, quantity const& a) -> quantity {
        return quantity{k * a.value_};
    }
    friend constexpr auto operator/(quantity const& a, T k) -> quantity {
        return quantity{a.value_ / k};
    }

    constexpr auto operator+=(quantity const& that) -> quantity& {
        value_ += that.value_;
        return *this;
    }
    constexpr auto operator-=(quantity const& that) -> quantity& {
        value_ -= that.value_;
        return *this;
    }
    constexpr auto operator*=(T k) -> quantity& {
        value_ *= k;
        return *this;
    }
    constexpr auto operator/=(T k) -> quantity& {
        value_ /= k;
        return *this;
    }

    template<int... Fs>
    friend constexpr auto operator*(quantity const& a, quantity<T, dims<Fs...>> const& b)
        -> quantity<T, dims<(Es + Fs)...>>
    {
        return quantity<T, dims<(Es + Fs)...>>{a.value_ * static_cast<T>(b)};
    }
    template<int... Fs>
    friend constexpr auto operator/(quantity const& a, quantity<T, dims<Fs...>> const& b)
        -> quantity<T, dims<(Es - Fs)...>>
    {
        return quantity<T, dims<(Es - Fs)...>>{a.value_ / static_cast<T>(b)};
    }
};

}

#endif
