#include <cppfrontier/json.hpp>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace cppfrontier::json {

auto value::find(std::string_view key) const -> const value* {
    if (!is_object()) return nullptr;
    for (const auto& [name, member] : as_object()) {
        if (name == key) return &member;
    }
    return nullptr;
}

auto value::operator[](std::string_view key) -> value& {
    auto& members = as_object();
    for (auto& [name, member] : members) {
        if (name == key) return member;
    }
    return members.emplace_back(std::string{key}, value{}).second;
}

namespace {

constexpr auto max_depth = 256;

auto append_utf8(std::string& output, std::uint32_t code_point) -> void {
    if (code_point < 0x80) {
        output += static_cast<char>(code_point);
    }
    else if (code_point < 0x800) {
        output += static_cast<char>(0xC0 | (code_point >> 6));
        output += static_cast<char>(0x80 | (code_point & 0x3F));
    }
    else if (code_point < 0x10000) {
        output += static_cast<char>(0xE0 | (code_point >> 12));
        output += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (code_point & 0x3F));
    }
    else {
        output += static_cast<char>(0xF0 | (code_point >> 18));
        output += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        output += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

class parser final {
public:
    explicit parser(std::string_view text) : text_{text} {}

    auto parse_document() -> std::optional<value> {
        auto result = parse_value(0);
        skip_whitespace();
        if (!result || position_ != text_.size()) return std::nullopt;
        return result;
    }

private:
    auto skip_whitespace() -> void {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\n' || text_[position_] == '\r')) {
            ++position_;
        }
    }

    auto consume(std::string_view word) -> bool {
        if (text_.substr(position_, word.size()) != word) return false;
        position_ += word.size();
        return true;
    }

    auto parse_value(int depth) -> std::optional<value> {
        if (depth > max_depth) return std::nullopt;
        skip_whitespace();
        if (position_ >= text_.size()) return std::nullopt;
        switch (text_[position_]) {
        case 'n': return consume("null") ? std::optional<value>{value{}} : std::nullopt;
        case 't': return consume("true") ? std::optional<value>{value{true}} : std::nullopt;
        case 'f': return consume("false") ? std::optional<value>{value{false}} : std::nullopt;
        case '"': {
            auto text = parse_string();
            if (!text) return std::nullopt;
            return value{std::move(*text)};
        }
        case '[': return parse_array(depth);
        case '{': return parse_object(depth);
        default: return parse_number();
        }
    }

    auto parse_number() -> std::optional<value> {
        const auto begin = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        const auto digits = [&] {
            const auto start = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            return position_ - start;
        };
        const auto integer_begin = position_;
        if (digits() == 0) return std::nullopt;
        if (text_[integer_begin] == '0' && position_ - integer_begin > 1) return std::nullopt;
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            if (digits() == 0) return std::nullopt;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            if (digits() == 0) return std::nullopt;
        }
        auto result = json::number{0, std::string{text_.substr(begin, position_ - begin)}};
        const auto parsed = std::from_chars(text_.data() + begin, text_.data() + position_, result.value);
        if (parsed.ec == std::errc::result_out_of_range) {
            const auto exponent = result.text.find_first_of("eE");
            const auto underflow = exponent != std::string::npos &&
                result.text.find('-', exponent) != std::string::npos;
            result.value = underflow ? 0.0 : HUGE_VAL;
            if (result.text[0] == '-') result.value = -result.value;
        }
        else if (parsed.ec != std::errc{} || parsed.ptr != text_.data() + position_) {
            return std::nullopt;
        }
        return value{std::move(result)};
    }

    auto parse_hex4() -> std::optional<std::uint32_t> {
        if (position_ + 4 > text_.size()) return std::nullopt;
        auto result = std::uint32_t{0};
        const auto parsed = std::from_chars(text_.data() + position_, text_.data() + position_ + 4,
                                            result, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text_.data() + position_ + 4) {
            return std::nullopt;
        }
        position_ += 4;
        return result;
    }

    auto parse_string() -> std::optional<std::string> {
        ++position_;
        auto result = std::string{};
        while (position_ < text_.size()) {
            const auto next = text_[position_++];
            if (next == '"') return result;
            if (static_cast<unsigned char>(next) < 0x20) return std::nullopt;
            if (next != '\\') {
                result += next;
                continue;
            }
            if (position_ >= text_.size()) return std::nullopt;
            switch (text_[position_++]) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                auto code_point = parse_hex4();
                if (!code_point) return std::nullopt;
                // A high surrogate combines with a following low surrogate;
                // an unpaired surrogate becomes U+FFFD.
                if (*code_point >= 0xD800 && *code_point < 0xDC00 &&
                    text_.substr(position_, 2) == "\\u") {
                    const auto saved = position_;
                    position_ += 2;
                    const auto low = parse_hex4();
                    if (low && *low >= 0xDC00 && *low < 0xE000) {
                        *code_point = 0x10000 + ((*code_point - 0xD800) << 10) + (*low - 0xDC00);
                    }
                    else {
                        position_ = saved;
                    }
                }
                if (*code_point >= 0xD800 && *code_point < 0xE000) *code_point = 0xFFFD;
                append_utf8(result, *code_point);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    auto parse_array(int depth) -> std::optional<value> {
        ++position_;
        auto result = array{};
        skip_whitespace();
        if (consume("]")) return value{std::move(result)};
        for (;;) {
            auto element = parse_value(depth + 1);
            if (!element) return std::nullopt;
            result.push_back(std::move(*element));
            skip_whitespace();
            if (consume("]")) return value{std::move(result)};
            if (!consume(",")) return std::nullopt;
        }
    }

    auto parse_object(int depth) -> std::optional<value> {
        ++position_;
        auto result = value{object{}};
        auto& members = result.as_object();
        // The index of each key in `members`; a repeated key replaces the earlier value.
        auto indices = std::unordered_map<std::string, std::size_t>{};
        skip_whitespace();
        if (consume("}")) return result;
        for (;;) {
            skip_whitespace();
            if (position_ >= text_.size() || text_[position_] != '"') return std::nullopt;
            auto key = parse_string();
            if (!key) return std::nullopt;
            skip_whitespace();
            if (!consume(":")) return std::nullopt;
            auto member = parse_value(depth + 1);
            if (!member) return std::nullopt;
            const auto [found, added] = indices.try_emplace(*key, members.size());
            if (added) members.emplace_back(std::move(*key), std::move(*member));
            else members[found->second].second = std::move(*member);
            skip_whitespace();
            if (consume("}")) return result;
            if (!consume(",")) return std::nullopt;
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

// The length of the valid UTF-8 sequence at the start of `text`, or 0.
auto utf8_length(std::string_view text) -> std::size_t {
    const auto lead = static_cast<unsigned char>(text[0]);
    const auto length = lead < 0x80 ? 1u : (lead >> 5) == 0x6 ? 2u : (lead >> 4) == 0xE ? 3u
        : (lead >> 3) == 0x1E ? 4u : 0u;
    if (length == 0 || length > text.size()) return 0;
    auto code_point = static_cast<std::uint32_t>(length == 1 ? lead : lead & (0x7F >> length));
    for (auto index = std::size_t{1}; index < length; ++index) {
        const auto next = static_cast<unsigned char>(text[index]);
        if ((next & 0xC0) != 0x80) return 0;
        code_point = (code_point << 6) | (next & 0x3F);
    }
    constexpr std::uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    if (code_point < minimum[length] || code_point > 0x10FFFF ||
        (code_point >= 0xD800 && code_point < 0xE000)) {
        return 0;
    }
    return length;
}

auto serialize_string(std::string& output, std::string_view text) -> void {
    output += '"';
    while (!text.empty()) {
        const auto next = static_cast<unsigned char>(text[0]);
        if (next == '"' || next == '\\') {
            output += '\\';
            output += static_cast<char>(next);
        }
        else if (next == '\n') output += "\\n";
        else if (next == '\r') output += "\\r";
        else if (next == '\t') output += "\\t";
        else if (next < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", next);
            output += buffer;
        }
        else if (next >= 0x80) {
            const auto length = utf8_length(text);
            if (length == 0) {
                output += "�";
                text.remove_prefix(1);
                continue;
            }
            output += text.substr(0, length);
            text.remove_prefix(length);
            continue;
        }
        else {
            output += static_cast<char>(next);
        }
        text.remove_prefix(1);
    }
    output += '"';
}

auto serialize_to(std::string& output, const value& data) -> void {
    if (data.is_null()) output += "null";
    else if (data.is_bool()) output += data.as_bool() ? "true" : "false";
    else if (data.is_number()) {
        if (!data.as_number_text().empty()) {
            output += data.as_number_text();
            return;
        }
        const auto number = data.as_number();
        if (!std::isfinite(number)) {
            output += "null";
            return;
        }
        char buffer[32];
        const auto written = std::to_chars(buffer, buffer + sizeof buffer, number);
        output.append(buffer, written.ptr);
    }
    else if (data.is_string()) serialize_string(output, data.as_string());
    else if (data.is_array()) {
        output += '[';
        auto first = true;
        for (const auto& element : data.as_array()) {
            if (!std::exchange(first, false)) output += ',';
            serialize_to(output, element);
        }
        output += ']';
    }
    else {
        output += '{';
        auto first = true;
        for (const auto& [key, member] : data.as_object()) {
            if (!std::exchange(first, false)) output += ',';
            serialize_string(output, key);
            output += ':';
            serialize_to(output, member);
        }
        output += '}';
    }
}

}

auto parse(std::string_view text) -> std::optional<value> {
    return parser{text}.parse_document();
}

auto serialize(const value& data) -> std::string {
    auto result = std::string{};
    serialize_to(result, data);
    return result;
}

}
