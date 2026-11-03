#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include <cppfrontier/ir.hpp>
#include <cppfrontier/source.hpp>

namespace cppfrontier::lowering_detail {

using source_map_entries = std::vector<std::pair<source_position, source_position>>;

struct emitter_output final {
    std::string text;
    source_map_entries source_map;
};

// `line` with a `#include "x.h2"` directive changed to include the generated
// `x.h`.
[[nodiscard]] auto cpp1_include_line(std::string_view line) -> std::string;

// Rendered Cpp1 in pieces. Each piece after the first begins with a `#line`
// directive to its source line, so the line of every output line is known.
class line_text final {
public:
    struct piece final {
        std::uint32_t line = 0;
        // The last source line of the declaration or Cpp1 line that a piece
        // whose directive names the source file renders; 0 for other pieces.
        std::uint32_t end = 0;
        std::string text;
    };

    line_text() = default;
    line_text(std::string text);
    line_text(const char* text) : line_text{std::string{text}} {}
    auto operator+=(std::string_view text) -> line_text&;
    // Appends `text`, whose first piece continues the last piece.
    auto append(line_text text) -> void;
    // Begins a piece on source line `line`.
    auto begin_line(std::uint32_t line, std::uint32_t end = 0) -> void;
    // The text of the last piece.
    [[nodiscard]] auto tail() -> std::string& { return pieces_.back().text; }
    [[nodiscard]] auto pieces() const -> const std::vector<piece>& { return pieces_; }
    [[nodiscard]] auto pieces() -> std::vector<piece>& { return pieces_; }
    [[nodiscard]] auto empty() const -> bool {
        return pieces_.size() == 1 && pieces_.front().text.empty();
    }
    [[nodiscard]] auto ends_with_newline() const -> bool;

private:
    std::vector<piece> pieces_{1};
};

// The source line at the end of a growing `line_text` that begins on `line`.
struct line_counter final {
    std::uint32_t line = 0;
    std::size_t piece = 0;
    std::size_t counted = 0;

    auto at_end(const line_text& text) -> std::uint32_t;
};

class output_builder final {
public:
    explicit output_builder(std::string source_path);
    auto try_mark_include(const std::string& header) -> bool;
    auto add_include(const std::string& header) -> bool;
    // Appends `#line line_number` and `text`, which renders source lines
    // `line_number` through `end_line` (`line_number` when 0).
    auto add_line(std::uint32_t line_number, line_text text, std::uint32_t end_line = 0) -> void;
    // Appends a continuation of the previous `add_line` text.
    auto add_verbatim_line(std::uint32_t line_number, std::string_view text) -> void;
    auto add_raw(line_text text) -> void;
    [[nodiscard]] auto take() -> line_text;
    // The output `text` (taken and then rewritten) with each generated line
    // mapped to the source line of its piece, if that is within the source
    // lines its `add_line` rendered. `#line` directives are emitted only if
    // `line_directives`, and one without the file name only where it changes
    // the line.
    [[nodiscard]] auto finish(const line_text& text, bool line_directives) const
        -> emitter_output;

private:
    std::string source_path_;
    line_text output_;
    std::set<std::string> includes_;
    // The piece the last `add_line` began.
    std::size_t last_line_ = 0;
};

}
