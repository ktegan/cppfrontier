#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <cppfrontier/options.hpp>

namespace cppfrontier {

enum class source_category : std::uint8_t {
    empty,
    comment,
    import,
    preprocessor,
    cpp1,
    cpp2,
    raw_string,
};

struct source_line final {
    std::string text;
    source_category category = source_category::cpp1;
    // Whether the line is in an `#elif` or `#else` branch, at any depth of
    // `#if`. Only the first branch's braces are counted, as if it were taken.
    bool in_later_branch = false;
    // The line continues the one before it across a line splice.
    bool spliced = false;
};

struct source_position final {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

struct source_span final {
    source_position begin;
    source_position end;
};

struct section_range final {
    std::uint32_t begin_line = 1;
    std::uint32_t end_line_exclusive = 1;
};

class source_text final {
public:
    // `experiments` adds the line forms an enabled experiment declares.
    explicit source_text(std::string text, const experiment_set& experiments = {});
    [[nodiscard]] static source_text from_bytes(std::string text,
                                                const experiment_set& experiments = {});
    [[nodiscard]] auto text_between(source_position begin, source_position end) const -> std::string;

    [[nodiscard]] auto data() const noexcept -> const std::string&;
    [[nodiscard]] auto lines() const noexcept -> const std::vector<source_line>&;
    // The offset of the start of each line, and of the end after a final newline.
    [[nodiscard]] auto line_starts() const noexcept -> const std::vector<std::size_t>&;
    [[nodiscard]] auto has_cpp2() const noexcept -> bool;
    [[nodiscard]] auto cpp2_sections() const -> std::vector<section_range>;

private:
    std::string text_;
    std::vector<source_line> lines_;
    std::vector<std::size_t> line_starts_;
    std::vector<section_range> sections_;
};

[[nodiscard]] auto categorize_lines(std::string_view text,
                                    const experiment_set& experiments = {})
    -> std::vector<source_line>;
[[nodiscard]] auto category_name(source_category category) noexcept -> std::string_view;
// Whether line `index` continues the line before it across a line splice.
[[nodiscard]] auto continues_line(const source_text& source, std::size_t index) -> bool;
[[nodiscard]] auto code_begin_index(const source_text& source) -> std::size_t;

}
