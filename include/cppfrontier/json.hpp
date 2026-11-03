#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cppfrontier::json {

class value;
using array = std::vector<value>;
// Members in insertion order; a key appears at most once.
using object = std::vector<std::pair<std::string, value>>;

// A JSON number. A parsed number keeps its text, which `serialize` writes
// unchanged; its value is the nearest double (infinity or 0 out of range).
struct number {
    double value = 0;
    std::string text;
    friend auto operator==(const number& left, const number& right) -> bool {
        return left.value == right.value;
    }
};

// A JSON value.
class value final {
public:
    value() = default;
    value(std::nullptr_t) {}
    value(bool data) : data_{data} {}
    value(double data) : data_{number{data, {}}} {}
    value(int data) : value{static_cast<double>(data)} {}
    value(std::int64_t data) : value{static_cast<double>(data)} {}
    value(std::uint32_t data) : value{static_cast<double>(data)} {}
    value(std::uint64_t data) : value{static_cast<double>(data)} {}
    value(number data) : data_{std::move(data)} {}
    value(std::string data) : data_{std::move(data)} {}
    value(std::string_view data) : data_{std::string{data}} {}
    value(const char* data) : data_{std::string{data}} {}
    value(array data) : data_{std::move(data)} {}
    value(object data) : data_{std::move(data)} {}

    [[nodiscard]] auto is_null() const noexcept -> bool { return data_.index() == 0; }
    [[nodiscard]] auto is_bool() const noexcept -> bool { return data_.index() == 1; }
    [[nodiscard]] auto is_number() const noexcept -> bool { return data_.index() == 2; }
    [[nodiscard]] auto is_string() const noexcept -> bool { return data_.index() == 3; }
    [[nodiscard]] auto is_array() const noexcept -> bool { return data_.index() == 4; }
    [[nodiscard]] auto is_object() const noexcept -> bool { return data_.index() == 5; }

    // Each accessor requires the matching kind.
    [[nodiscard]] auto as_bool() const -> bool { return std::get<bool>(data_); }
    [[nodiscard]] auto as_number() const -> double { return std::get<number>(data_).value; }
    [[nodiscard]] auto as_number_text() const -> const std::string& { return std::get<number>(data_).text; }
    [[nodiscard]] auto as_string() const -> const std::string& { return std::get<std::string>(data_); }
    [[nodiscard]] auto as_array() const -> const array& { return std::get<array>(data_); }
    [[nodiscard]] auto as_object() const -> const object& { return std::get<object>(data_); }
    [[nodiscard]] auto as_array() -> array& { return std::get<array>(data_); }
    [[nodiscard]] auto as_object() -> object& { return std::get<object>(data_); }

    // The member named `key` of an object, or null if absent or not an object.
    [[nodiscard]] auto find(std::string_view key) const -> const value*;
    // The member named `key`, added as null if absent; requires an object.
    auto operator[](std::string_view key) -> value&;

    friend auto operator==(const value&, const value&) -> bool = default;

private:
    std::variant<std::nullptr_t, bool, number, std::string, array, object> data_;
};

// The value of `text`, or nothing if it is not a single valid JSON value.
[[nodiscard]] auto parse(std::string_view text) -> std::optional<value>;
// Compact JSON text. Invalid UTF-8 in strings becomes U+FFFD.
[[nodiscard]] auto serialize(const value& data) -> std::string;

}
