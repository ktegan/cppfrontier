#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <cppfrontier/source.hpp>

namespace cppfrontier {

enum class token_kind : std::uint8_t {
    identifier,
    keyword,
    pointer_dereference,
    pointer_suffix,
    integer_literal,
    string_literal,
    character_literal,
    punctuation,
    end_of_file,
};

struct token final {
    token_kind kind = token_kind::end_of_file;
    std::string text;
    source_span span;
    bool escaped_identifier = false;
};

using token_list = std::vector<token>;

[[nodiscard]] auto is_contextual_name(const token& value) noexcept -> bool;
[[nodiscard]] auto is_cpp2_name(const token& value) noexcept -> bool;
[[nodiscard]] auto is_declaration_name(const token& value) noexcept -> bool;
[[nodiscard]] auto is_type_name(const token& value) noexcept -> bool;
[[nodiscard]] auto is_cpp1_reserved_name(std::string_view value) noexcept -> bool;
[[nodiscard]] auto is_escapable_cpp1_name(std::string_view value) noexcept -> bool;
[[nodiscard]] auto cpp1_identifier(std::string_view value) -> std::string;
// The Cpp1 name of an object named `_` declared at `position`, unique by its
// position, as upstream.
[[nodiscard]] auto anonymous_object_name(source_position position) -> std::string;
}
