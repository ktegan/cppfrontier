#include "lowering_detail.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace cppfrontier::lowering_detail {

line_text::line_text(std::string text) {
    pieces_.front().text = std::move(text);
}

auto line_text::operator+=(std::string_view text) -> line_text& {
    pieces_.back().text += text;
    return *this;
}

auto line_text::append(line_text text) -> void {
    auto& pieces = text.pieces_;
    pieces_.back().text += pieces.front().text;
    pieces_.insert(pieces_.end(), std::make_move_iterator(pieces.begin() + 1),
                   std::make_move_iterator(pieces.end()));
}

auto line_text::begin_line(std::uint32_t line, std::uint32_t end) -> void {
    pieces_.push_back({line, end, {}});
}

// A piece begins after its directive's newline.
auto line_text::ends_with_newline() const -> bool {
    return pieces_.back().text.empty() ? pieces_.size() > 1 : pieces_.back().text.ends_with('\n');
}

auto line_counter::at_end(const line_text& text) -> std::uint32_t {
    const auto& pieces = text.pieces();
    if (pieces.size() - 1 != piece) {
        piece = pieces.size() - 1;
        line = pieces.back().line;
        counted = 0;
    }
    const auto& last = pieces.back().text;
    line += static_cast<std::uint32_t>(
        std::count(last.begin() + static_cast<std::ptrdiff_t>(counted), last.end(), '\n'));
    counted = last.size();
    return line;
}

output_builder::output_builder(std::string source_path)
    : source_path_{std::move(source_path)} {}

auto output_builder::try_mark_include(const std::string& header) -> bool {
    return includes_.insert(header).second;
}

auto output_builder::add_include(const std::string& header) -> bool {
    if (!try_mark_include(header)) return false;
    output_ += "#include " + header + "\n";
    return true;
}

auto cpp1_include_line(std::string_view line) -> std::string {
    if (line.find("#include") == std::string_view::npos || !line.ends_with(".h2\"")) {
        return std::string{line};
    }
    return std::string{line.substr(0, line.size() - 2)} + "\"";
}

auto output_builder::add_line(std::uint32_t line_number, line_text text,
                              std::uint32_t end_line) -> void {
    output_.begin_line(line_number, std::max(line_number, end_line));
    last_line_ = output_.pieces().size() - 1;
    output_.append(std::move(text));
    output_ += "\n";
}

auto output_builder::add_verbatim_line(std::uint32_t line_number, std::string_view text) -> void {
    output_ += text;
    output_ += "\n";
    if (last_line_ != 0) {
        auto& end = output_.pieces()[last_line_].end;
        end = std::max(end, line_number);
    }
}

auto output_builder::add_raw(line_text text) -> void {
    output_.append(std::move(text));
}

auto output_builder::take() -> line_text {
    return std::move(output_);
}

auto output_builder::finish(const line_text& text, bool line_directives) const
    -> emitter_output {
    // The path as a string literal, as `\` and `"` may appear in it.
    auto file = std::string{" \""};
    for (const auto character : source_path_.empty() ? std::string{"input.cpp2"} : source_path_) {
        if (character == '\\' || character == '"') file += '\\';
        file += character;
    }
    file += '"';
    auto result = emitter_output{};
    // The source line of the next generated line (0 if none), and the last
    // source line of the current `add_line` text.
    auto line = std::uint32_t{0};
    auto limit = std::uint32_t{0};
    auto generated = std::uint32_t{0};
    for (const auto& piece : text.pieces()) {
        if (piece.line != 0) {
            // A directive without the file name is omitted where the line it
            // names is already next.
            const auto needed = piece.end != 0 || piece.line != line;
            line = piece.line;
            if (piece.end != 0) limit = piece.end;
            if (line_directives && needed) {
                result.text += "#line " + std::to_string(line) +
                    (piece.end != 0 ? file : std::string{}) + "\n";
                ++generated;
            }
        }
        for (auto rest = std::string_view{piece.text}; !rest.empty();) {
            const auto length = std::min(rest.find('\n'), rest.size() - 1) + 1;
            result.text += rest.substr(0, length);
            rest.remove_prefix(length);
            ++generated;
            if (line != 0 && line <= limit) {
                result.source_map.push_back({{generated, 1}, {line, 1}});
            }
            if (line != 0) ++line;
        }
    }
    return result;
}

}
