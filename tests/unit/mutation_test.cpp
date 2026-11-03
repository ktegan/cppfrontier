#include <catch2/catch_all.hpp>
#include <cppfrontier/pipeline.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct seed_source final {
    std::string_view name;
    std::string_view text;
};

constexpr seed_source seeds[] = {
    {"type-members", R"cpp2(Widget: type = {
    value: i32 = 0;
    read: (this) -> i32 = value;
    reset: (inout this) = { value = 0; }
}

main: () -> int = {
    w: Widget = ();
    return w.read();
}
)cpp2"},
    {"templates-forward", R"cpp2(sum: <T: type> (a: T, b: T) -> T = a + b;
first: (forward values: std::vector<int>) -> forward int = values[0];
main: () -> int = {
    v: std::vector<int> = (3, 1, 2);
    return first(v);
}
)cpp2"},
    {"control-flow", R"cpp2(classify: (value: i32) -> i32 = {
    i := 0;
    while i < value next i++ {
        if i == 1 { continue; }
        else if i == 2 { break; }
        else { do { i += 1; } next i-- while i > 0; }
    }
    for items do (inout item) { item += 1; }
    return i;
}
)cpp2"},
    {"using-discard", R"cpp2(using std::string;
f: (copy text: string) -> string = {
    words: std::vector<string> = ("a", "b");
    _ = words;
    return text;
}
)cpp2"},
    {"namespace-alias", R"cpp2(namespace demo {
    Item: type = { value: i32 = 0; }
    make: () -> Item = ();
}
label: namespace == demo;
main: () = { value := label::make(); _ = value; }
)cpp2"},
    {"requires", R"cpp2(f: <T> (x: T) requires std::regular<T> = x;
box: <T> type requires std::is_integral_v<T> = { value: T = (); }
)cpp2"},
    {"pointer-indexed", R"cpp2(g: (_: i32) = { }
f: () = {
    p := new<int>(0);
    p*++;
    _ = p*;
    functions: std::array = (g, g);
    i := 0;
    functions[i](1);
}
)cpp2"},
};

auto token_signature(const cppfrontier::token_list& tokens) -> std::string {
    auto result = std::string{};
    for (const auto& value : tokens) {
        result += std::to_string(static_cast<int>(value.kind));
        result += ':';
        result += value.text;
        result += '\n';
    }
    return result;
}

auto tokenize_all(const cppfrontier::source_text& source) -> cppfrontier::token_list {
    auto tokens = cppfrontier::token_list{};
    for (const auto& section : source.cpp2_sections()) {
        auto section_tokens = cppfrontier::tokenize(
            source, section.begin_line, section.end_line_exclusive);
        tokens.insert(tokens.end(), section_tokens.begin(), section_tokens.end());
    }
    return tokens;
}

auto protected_masks(std::string_view text) -> std::vector<bool> {
    auto protected_mask = std::vector<bool>(text.size(), false);
    auto position = std::size_t{0};
    while (position < text.size()) {
        if (position + 1 < text.size() && text[position] == '/' && text[position + 1] == '/') {
            const auto end = text.find('\n', position);
            const auto limit = end == std::string_view::npos ? text.size() : end;
            std::fill(protected_mask.begin() + static_cast<std::ptrdiff_t>(position),
                      protected_mask.begin() + static_cast<std::ptrdiff_t>(limit), true);
            position = limit;
            continue;
        }
        if (position + 1 < text.size() && text[position] == '/' && text[position + 1] == '*') {
            const auto end = text.find("*/", position + 2);
            const auto limit = end == std::string_view::npos ? text.size() : end + 2;
            std::fill(protected_mask.begin() + static_cast<std::ptrdiff_t>(position),
                      protected_mask.begin() + static_cast<std::ptrdiff_t>(limit), true);
            position = limit;
            continue;
        }
        if (text[position] == '"' || text[position] == '\'') {
            const auto quote = text[position];
            auto end = position + 1;
            while (end < text.size()) {
                if (text[end] == '\\' && end + 1 < text.size()) {
                    end += 2;
                    continue;
                }
                if (text[end] == quote) {
                    ++end;
                    break;
                }
                ++end;
            }
            std::fill(protected_mask.begin() + static_cast<std::ptrdiff_t>(position),
                      protected_mask.begin() + static_cast<std::ptrdiff_t>(end), true);
            position = end;
            continue;
        }
        ++position;
    }
    return protected_mask;
}

auto spacing_mutations(std::string_view text) -> std::vector<std::string> {
    const auto protected_mask = protected_masks(text);
    auto mutations = std::vector<std::string>{};
    for (auto position = std::size_t{0}; position < text.size(); ++position) {
        if (protected_mask[position]) continue;
        if (position == 0 || position + 1 >= text.size()) continue;
        if (text[position - 1] == '\n' || text[position] == '\n') continue;
        if (text[position - 1] == ' ' || text[position - 1] == '\t') continue;
        if (text[position] == ' ' || text[position] == '\t') continue;
        const auto left = text[position - 1];
        const auto right = text[position];
        if (left == '*' || right == '*' || left == '/' || right == '/') continue;
        if (left == right && std::string_view{"<>+-&|:=.!"}.find(left) != std::string_view::npos) {
            continue;
        }
        auto mutation = std::string{text};
        mutation.insert(mutation.begin() + static_cast<std::ptrdiff_t>(position), ' ');
        mutations.push_back(std::move(mutation));
    }
    return mutations;
}

auto whitespace_variants(std::string_view text) -> std::vector<std::string> {
    const auto protected_mask = protected_masks(text);
    auto variants = std::vector<std::string>{};
    for (auto position = std::size_t{0}; position < text.size(); ++position) {
        if (protected_mask[position] || text[position] == '\n') continue;
        const auto left = position == 0 ? '\0' : text[position - 1];
        const auto right = text[position];
        const auto punctuation = [](char value) {
            return std::string_view{"(){}[];,:=<>+-&|.!?@"}.find(value) != std::string_view::npos;
        };
        if (!punctuation(left) && !punctuation(right)) continue;
        if (left == '*' || right == '*' || left == '/' || right == '/') continue;
        if (left == right && std::string_view{"<>+-&|:=."}.find(left) != std::string_view::npos) {
            continue;
        }

        if (text[position] == ' ' || text[position] == '\t') {
            auto variant = std::string{text};
            variant.erase(variant.begin() + static_cast<std::ptrdiff_t>(position));
            variants.push_back(std::move(variant));
        }
        else if (left != '\0' && left != ' ' && left != '\t') {
            auto variant = std::string{text};
            variant.insert(variant.begin() + static_cast<std::ptrdiff_t>(position), ' ');
            variants.push_back(std::move(variant));
        }
    }
    return variants;
}

auto canonical_text(std::string_view text) -> std::string {
    auto result = std::string{};
    result.reserve(text.size());
    auto pending_space = false;
    for (const auto value : text) {
        if (std::isspace(static_cast<unsigned char>(value))) {
            pending_space = !result.empty();
            continue;
        }
        const auto punctuation = std::string_view{"(){}[];,:=<>+-&|.!?@"}.find(value) !=
            std::string_view::npos;
        const auto previous_punctuation = !result.empty() &&
            std::string_view{"(){}[];,:=<>+-&|.!?@"}.find(result.back()) != std::string_view::npos;
        if (pending_space && !punctuation && !previous_punctuation) result += ' ';
        result += value;
        pending_space = false;
    }
    return result;
}

auto diagnostic_signature(const cppfrontier::translation_result& result) -> std::string {
    auto signature = std::string{};
    for (const auto& issue : result.diagnostics) {
        signature += std::to_string(static_cast<int>(issue.severity));
        signature += ':';
        signature += issue.code;
        signature += ':';
        signature += issue.message;
        signature += '\n';
    }
    return signature;
}

}

TEST_CASE("token-boundary whitespace mutations preserve Cpp2 translation", "[mutation][whitespace]") {
    for (const auto& seed : seeds) {
        const auto baseline_source = cppfrontier::source_text::from_bytes(std::string{seed.text});
        const auto baseline_tokens = tokenize_all(baseline_source);
        const auto baseline = cppfrontier::translator{}.translate_source(std::string{seed.text});
        INFO("seed: " << seed.name);
        for (const auto& issue : baseline.diagnostics) INFO(issue.code << ": " << issue.message);
        REQUIRE_FALSE(baseline.has_errors());

        auto checked = 0;
        for (const auto& mutation : spacing_mutations(seed.text)) {
            const auto mutated_source = cppfrontier::source_text::from_bytes(mutation);
            const auto mutated_tokens = tokenize_all(mutated_source);
            if (token_signature(mutated_tokens) != token_signature(baseline_tokens)) continue;
            ++checked;
            const auto translated = cppfrontier::translator{}.translate_source(mutation);
            INFO("mutation: " << mutation);
            CHECK_FALSE(translated.has_errors());
            CHECK(diagnostic_signature(translated) == diagnostic_signature(baseline));
        }
        INFO("checked mutations: " << checked);
        CHECK(checked >= 10);
    }
}

TEST_CASE("whitespace additions and removals preserve diagnostics and Cpp1",
          "[mutation][whitespace]") {
    for (const auto& seed : seeds) {
        const auto baseline = cppfrontier::translator{}.translate_source(std::string{seed.text});
        INFO("seed: " << seed.name);
        for (const auto& issue : baseline.diagnostics) INFO(issue.code << ": " << issue.message);
        REQUIRE_FALSE(baseline.has_errors());

        const auto baseline_tokens = tokenize_all(
            cppfrontier::source_text::from_bytes(std::string{seed.text}));
        auto checked = 0;
        for (const auto& variant : whitespace_variants(seed.text)) {
            const auto variant_tokens = tokenize_all(cppfrontier::source_text::from_bytes(variant));
            if (token_signature(variant_tokens) != token_signature(baseline_tokens)) continue;
            const auto translated = cppfrontier::translator{}.translate_source(variant);
            INFO("variant: " << variant);
            for (const auto& issue : translated.diagnostics) {
                INFO(issue.code << ": " << issue.message);
            }
            CHECK_FALSE(translated.has_errors());
            CHECK(diagnostic_signature(translated) == diagnostic_signature(baseline));
            CHECK(canonical_text(translated.cpp1) == canonical_text(baseline.cpp1));
            ++checked;
        }
        INFO("checked variants: " << checked);
        CHECK(checked >= 20);
    }
}

TEST_CASE("whitespace mutations do not change literal contents", "[mutation][whitespace][literals]") {
    const auto source = std::string{
        "f: () -> std::string = \"  spaced  \\\" text  \";\n"};
    const auto baseline = cppfrontier::translator{}.translate_source(source);
    INFO(baseline.cpp1);
    REQUIRE_FALSE(baseline.has_errors());

    const auto baseline_tokens = tokenize_all(cppfrontier::source_text::from_bytes(source));
    for (const auto& variant : whitespace_variants(source)) {
        const auto variant_tokens = tokenize_all(cppfrontier::source_text::from_bytes(variant));
        if (token_signature(variant_tokens) != token_signature(baseline_tokens)) continue;
        INFO("variant: " << variant);
        const auto translated = cppfrontier::translator{}.translate_source(variant);
        INFO(translated.cpp1);
        CHECK_FALSE(translated.has_errors());
        CHECK(canonical_text(translated.cpp1) == canonical_text(baseline.cpp1));
    }
}

TEST_CASE("raw string whitespace and delimiters remain opaque",
          "[mutation][whitespace][literals][raw]") {
    const auto source = std::string{
        "auto text = R\"tag(first  \nsecond \"quoted\" / // not a comment)tag\";\n"
        "main: () -> int = { return 0; }\n"};
    const auto baseline = cppfrontier::translator{}.translate_source(source);
    INFO(baseline.cpp1);
    REQUIRE_FALSE(baseline.has_errors());
    CHECK(baseline.cpp1.find("R\"tag(first  \nsecond \"quoted\" / // not a comment)tag\"") !=
          std::string::npos);

    const auto space_inside = cppfrontier::translator{}.translate_source(
        "auto text = R\"tag(first   \nsecond \"quoted\" / // not a comment)tag\";\n"
        "main: () -> int = { return 0; }\n");
    REQUIRE_FALSE(space_inside.has_errors());
    CHECK(space_inside.cpp1.find("R\"tag(first   \nsecond \"quoted\" / // not a comment)tag\"") !=
          std::string::npos);

    const auto fake_terminator = cppfrontier::translator{}.translate_source(
        "auto text = R\"tag(first )\" not the end\ntag)\";\n"
        "main: () -> int = { return 0; }\n");
    INFO(fake_terminator.cpp1);
    REQUIRE_FALSE(fake_terminator.has_errors());
    CHECK(fake_terminator.cpp1.find("first )\" not the end\ntag") != std::string::npos);
}

TEST_CASE("raw string contents are not rewritten as Cpp2 expressions",
          "[mutation][literals][raw][emitter]") {
    const auto source = std::string{
        "main: () -> int = {\n"
        "    std::cout << R\"tag(new<int>(x) p* s.ssize() a as T v[i](x) (:int=1))tag\";\n"
        "    std::cout << \"new<int>(x) p* s.ssize() a as T v[i](x) (:int=1)\";\n"
        "    return 0;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("R\"tag(new<int>(x) p* s.ssize() a as T v[i](x) (:int=1))tag\"") !=
          std::string::npos);
    CHECK(result.cpp1.find("\"new<int>(x) p* s.ssize() a as T v[i](x) (:int=1)\"") !=
          std::string::npos);
}

TEST_CASE("pointer dereference rewriting leaves literal and comment contents opaque",
          "[mutation][literals][comments][pointers]") {
    const auto source = std::string{
        "main: () -> int = {\n"
        "    pointer := new<int>(0);\n"
        "    std::cout << \"word*++\" /* item*++ */ << ' ' << pointer*;\n"
        "    return 0;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("\"word*++\"") != std::string::npos);
    CHECK(result.cpp1.find("/* item*++ */") != std::string::npos);
    CHECK(result.cpp1.find("*cpp2::impl::assert_not_null(cpp2::move(pointer))") != std::string::npos);
}

TEST_CASE("keyword mutations are rejected without emitting Cpp1", "[mutation][keywords][diagnostics]") {
    const auto cases = std::vector<std::string>{
        "f: () -> int = { retrun 0; }\n",
        "f: () = { fi true { } }\n",
        "f: () = { wihle true { } }\n",
        "f: () = { fro items do (x) { } }\n",
        "f: () = { brk; }\n",
        "f: () = { coninue; }\n",
        "f: () = { else { } }\n",
        "f: () = { return := 1; }\n",
        "f: (foward x: i32) = { }\n",
        "f: (inot x: i32) = { }\n",
        "f: (copy x y: i32) = { }\n",
        "T: typ = { }\n",
        "N: namespac = { }\n",
        "f: <T> (x: T) requiers true = x;\n",
    };
    for (const auto& source : cases) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        INFO("source: " << source);
        for (const auto& issue : result.diagnostics) INFO(issue.code << ": " << issue.message);
        CHECK(result.has_errors());
        CHECK(result.cpp1.empty());
    }
}

TEST_CASE("contextual keyword names survive independent position mutations", "[mutation][keywords][names]") {
    struct mutation_case final {
        std::string baseline;
        std::string mutated;
        std::string expected;
    };
    const auto cases = std::vector<mutation_case>{
        {"f: () = { }\n", "class: () = { }\n", "auto cpp2_class()"},
        {"f: (x: int) = { }\n", "f: (class: int) = { }\n",
            "cpp2::impl::in<int> cpp2_class"},
        {"f: <T> () = { }\n", "f: <class> () = { }\n", "typename cpp2_class"},
        {"T: type = { value: int = 0; }\n", "T: type = { class: int = 0; }\n", "int cpp2_class"},
        {"f: () = { value := 0; _ = value; }\n", "f: () = { class := 0; _ = class; }\n", "auto cpp2_class {0};"},
        {"f: () = { value(); }\n", "f: () = { class(); }\n", "cpp2_class()"},
        {"f: () -> _ = { return value; }\n", "f: () -> _ = { return class; }\n", "return cpp2_class;"},
    };
    for (const auto& mutation : cases) {
        const auto baseline_result = cppfrontier::translator{}.translate_source(mutation.baseline);
        INFO("baseline: " << mutation.baseline);
        for (const auto& issue : baseline_result.diagnostics) {
            INFO(issue.code << ": " << issue.message);
        }
        REQUIRE_FALSE(baseline_result.has_errors());

        const auto mutated_result = cppfrontier::translator{}.translate_source(mutation.mutated);
        INFO("mutation: " << mutation.mutated);
        for (const auto& issue : mutated_result.diagnostics) {
            INFO(issue.code << ": " << issue.message);
        }
        INFO(mutated_result.cpp1);
        CHECK_FALSE(mutated_result.has_errors());
        CHECK(mutated_result.cpp1.find(mutation.expected) != std::string::npos);
    }
}

TEST_CASE("statement keywords remain structural rather than becoming names", "[mutation][keywords][statements]") {
    const auto cases = std::vector<std::string>{
        "f: () = { if := 1; }\n",
        "f: () = { return := 1; }\n",
        "f: () = { while := 1; }\n",
        "f: () = { break := 1; }\n",
    };
    for (const auto& source : cases) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        INFO("source: " << source);
        for (const auto& issue : result.diagnostics) {
            INFO(issue.code << ": " << issue.message);
        }
        CHECK(result.has_errors());
        CHECK(result.cpp1.empty());
    }
}

// Truncations and deletions produce malformed input; translation must still
// terminate with diagnostics rather than crash. Run under CPPFRONTIER_SANITIZE
// to catch out-of-bounds reads.
TEST_CASE("truncated and damaged sources translate without crashing", "[mutation][robustness]") {
    const auto translator = cppfrontier::translator{};
    for (const auto& seed : seeds) {
        INFO(seed.name);
        const auto text = std::string{seed.text};
        for (auto length = std::size_t{0}; length <= text.size(); ++length) {
            INFO("prefix " << length);
            CHECK_NOTHROW(static_cast<void>(translator.translate_source(text.substr(0, length))));
        }
        for (auto index = std::size_t{0}; index < text.size(); ++index) {
            INFO("deleted " << index);
            auto damaged = text;
            damaged.erase(index, 1);
            CHECK_NOTHROW(static_cast<void>(translator.translate_source(damaged)));
        }
    }
}
