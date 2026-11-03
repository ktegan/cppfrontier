#include <cppfrontier/include_policy.hpp>
#include <cppfrontier/options.hpp>

#include <algorithm>
#include <array>
#include <set>

namespace cppfrontier {

auto include_header(std::string_view line) -> std::string {
    const auto begin = line.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) return {};
    line.remove_prefix(begin);
    if (!line.starts_with("#include")) return {};
    const auto open = line.find_first_of("<\u0022");
    if (open == std::string_view::npos) return {};
    const auto close = line.find(line[open] == '<' ? '>' : '\u0022', open + 1);
    if (close == std::string_view::npos) return {};
    return std::string{line.substr(open, close - open + 1)};
}

namespace {

struct include_rule final {
    std::string header;
    std::string usage_name;
};

auto uses_identifier(const token_list& tokens, std::string_view name) -> bool {
    return std::any_of(tokens.begin(), tokens.end(), [&](const token& value) {
        return value.kind == token_kind::identifier && value.text == name;
    });
}

auto explicit_headers(const source_text& source) -> std::set<std::string> {
    auto result = std::set<std::string>{};
    const auto& lines = source.lines();
    const auto begin = code_begin_index(source);
    for (auto index = std::size_t{0}; index < begin; ++index) {
        const auto& line = lines[index];
        if (line.category != source_category::preprocessor) continue;
        const auto header = include_header(line.text);
        if (!header.empty()) result.emplace(header);
    }
    return result;
}

}

auto standard_include_directives(const source_text& source,
                                 const token_list& tokens,
                                 const translation_options& options)
    -> std::vector<std::string> {
    if (!options.include_standard_headers) return {};

    const std::array rules{
        include_rule{"<string>", "std::string"},
        include_rule{"<vector>", "std::vector"},
        include_rule{"<array>", "std::array"},
        include_rule{"<span>", "std::span"},
        include_rule{"<map>", "std::map"},
        include_rule{"<unordered_map>", "std::unordered_map"},
        include_rule{"<set>", "std::set"},
        include_rule{"<unordered_set>", "std::unordered_set"},
        include_rule{"<bitset>", "std::bitset"},
        include_rule{"<deque>", "std::deque"},
        include_rule{"<list>", "std::list"},
        include_rule{"<forward_list>", "std::forward_list"},
        include_rule{"<queue>", "std::queue"},
        include_rule{"<queue>", "std::priority_queue"},
        include_rule{"<stack>", "std::stack"},
        include_rule{"<iomanip>", "std::setw"},
        include_rule{"<iomanip>", "std::setprecision"},
        include_rule{"<iomanip>", "std::setfill"},
    };
    const auto already_included = explicit_headers(source);
    auto directives = std::vector<std::string>{};
    for (const auto& rule : rules) {
        const auto used = uses_identifier(tokens, rule.usage_name);
        if (used && !already_included.contains(rule.header) &&
            std::find(directives.begin(), directives.end(), rule.header) == directives.end()) {
            directives.push_back(rule.header);
        }
    }
    const auto uses_stream = uses_identifier(tokens, "std::cout") ||
        uses_identifier(tokens, "std::cerr") || uses_identifier(tokens, "std::endl");
    if (uses_stream && !already_included.contains("<iostream>")) {
        directives.insert(directives.begin(), "<iostream>");
    }
    return directives;
}

auto add_runtime_include(std::string& preamble, std::string_view header) -> void {
    constexpr auto runtime = std::string_view{"#include \"cpp2util.h\"\n"};
    const auto after = preamble.find(runtime);
    if (after == std::string::npos) return;
    preamble.insert(after + runtime.size(), "#include \"" + std::string{header} + "\"\n");
}

}
