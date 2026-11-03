#include <catch2/catch_all.hpp>

#include <cppfrontier/source.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

struct directive final {
    std::uint32_t line = 0;
    bool names_file = false;
};

// The `#line` directive on `text`, if it is one.
auto parse_directive(std::string_view text) -> std::optional<directive> {
    if (!text.starts_with("#line ")) return std::nullopt;
    auto number = std::uint32_t{0};
    const auto parsed = std::from_chars(text.data() + 6, text.data() + text.size(), number);
    if (parsed.ec != std::errc{}) return std::nullopt;
    return directive{number, parsed.ptr != text.data() + text.size()};
}

auto split_lines(std::string_view text) -> std::vector<std::string_view> {
    auto lines = std::vector<std::string_view>{};
    while (!text.empty()) {
        const auto newline = text.find('\n');
        lines.push_back(text.substr(0, newline));
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
    }
    return lines;
}

// Checks `result`'s source map against its `#line` directives, and against
// `clean`, the same translation without them.
auto check_source_map(const std::string& name, std::uint32_t source_lines,
                      const cppfrontier::translation_result& result,
                      const cppfrontier::translation_result& clean) -> void {
    CAPTURE(name);
    const auto lines = split_lines(result.cpp1);
    // A file without Cpp2 is copied, line for line.
    if (std::none_of(lines.begin(), lines.end(), [](auto line) { return parse_directive(line); })) {
        CHECK(result.source_map.size() == lines.size());
        for (const auto& [generated, original] : result.source_map) {
            CHECK(generated.line == original.line);
        }
        CHECK(clean.cpp1 == result.cpp1);
        return;
    }
    auto mapped = std::map<std::uint32_t, std::uint32_t>{};
    auto previous = std::uint32_t{0};
    for (const auto& [generated, original] : result.source_map) {
        CAPTURE(generated.line, original.line);
        // Each generated line maps once, in order, to a valid source line.
        CHECK(generated.line > previous);
        CHECK(generated.line <= lines.size());
        CHECK(original.line >= 1);
        CHECK(original.line <= source_lines);
        previous = generated.line;
        mapped[generated.line] = original.line;
    }

    // A mapped line is where the `#line` directives place it, at or after the
    // line its chunk begins on, and the first line of each chunk is mapped.
    // A directive without a file name stays within its chunk and changes the
    // line that follows it; it may go back, as for a `next` clause.
    auto implied = std::uint32_t{0};
    auto chunk_begin = std::uint32_t{0};
    auto starts_chunk = false;
    auto stripped = std::vector<std::uint32_t>{};
    for (std::uint32_t index = 0; index < lines.size(); ++index) {
        const auto generated = index + 1;
        CAPTURE(generated, lines[index]);
        if (const auto found = parse_directive(lines[index])) {
            CHECK_FALSE(mapped.contains(generated));
            if (found->names_file) {
                chunk_begin = found->line;
                starts_chunk = true;
            }
            else {
                CHECK(found->line >= chunk_begin);
                CHECK(found->line != implied);
            }
            implied = found->line;
            continue;
        }
        const auto entry = mapped.find(generated);
        if (starts_chunk) CHECK(entry != mapped.end());
        starts_chunk = false;
        if (entry != mapped.end()) {
            CHECK(entry->second == implied);
            CHECK(entry->second >= chunk_begin);
            stripped.push_back(entry->second);
        }
        else {
            stripped.push_back(0);
        }
        if (implied != 0) ++implied;
    }

    // Without `#line` directives, the output and map are the same lines.
    auto expected = std::string{};
    for (const auto line : lines) {
        if (!parse_directive(line)) expected += std::string{line} + "\n";
    }
    CHECK(clean.cpp1 == expected);
    auto clean_mapped = std::vector<std::uint32_t>(stripped.size());
    for (const auto& [generated, original] : clean.source_map) {
        REQUIRE(generated.line >= 1);
        REQUIRE(generated.line <= clean_mapped.size());
        clean_mapped[generated.line - 1] = original.line;
    }
    CHECK(clean_mapped == stripped);
}

auto translate(const std::string& source, bool line_directives)
    -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    options.source_path = "input.cpp2";
    options.line_directives = line_directives;
    return cppfrontier::translator{}.translate_source(source, options);
}

}

TEST_CASE("source maps place statements and members on their lines", "[source-map]") {
    const auto source = std::string{
        "base: type = { f: (virtual this) = { } }\n"   // 1
        "derived: type = {\n"                          // 2
        "    name: std::string;\n"                     // 3
        "    this: base = ();\n"                       // 4
        "    operator=: (out this, n: std::string) = {\n"  // 5
        "        name = n;\n"                          // 6
        "    }\n"                                      // 7
        "    g: (this) -> int = {\n"                   // 8
        "        x := 1; y := 2;\n"                    // 9
        "        return x + y;\n"                      // 10
        "    }\n"                                      // 11
        "}\n"};                                        // 12
    const auto result = translate(source, true);
    REQUIRE_FALSE(result.has_errors());
    const auto lines = split_lines(result.cpp1);
    const auto line_of = [&](std::string_view text) -> std::uint32_t {
        for (std::uint32_t index = 0; index < lines.size(); ++index) {
            if (lines[index].find(text) == std::string_view::npos) continue;
            for (const auto& [generated, original] : result.source_map) {
                if (generated.line == index + 1) return original.line;
            }
            return 0;
        }
        return 0;
    };
    // The members of `derived` follow its base helper.
    CHECK(line_of("derived(cpp2::impl::in<std::string> n);") == 5);
    CHECK(line_of("auto g() const& -> int;") == 8);
    CHECK(line_of("auto derived::g() const& -> int") == 8);
    // Statements that share a line keep it.
    CHECK(line_of("auto x {1};") == 9);
    CHECK(line_of("auto y {2};") == 9);
    CHECK(line_of("return cpp2::move(x) + cpp2::move(y);") == 10);

    check_source_map("example", 12, result, translate(source, false));
}

TEST_CASE("source maps follow #line directives that go back", "[source-map]") {
    const auto source = std::string{
        "f: (v: std::vector<int>) -> int = { n := v.ssize(); return v[0] + n; }\n"  // 1
        "main: () = {\n"                               // 2
        "    i := 0;\n"                                // 3
        "    for (1, 2) next i++ do (e) {\n"           // 4
        "        i += e;\n"                            // 5
        "    }\n"                                      // 6
        "    std::cout << i;\n"                        // 7
        "}\n"};                                        // 8
    const auto result = translate(source, true);
    REQUIRE_FALSE(result.has_errors());
    const auto lines = split_lines(result.cpp1);
    const auto line_of = [&](std::string_view text) -> std::uint32_t {
        for (std::uint32_t index = 0; index < lines.size(); ++index) {
            if (lines[index].find(text) == std::string_view::npos) continue;
            for (const auto& [generated, original] : result.source_map) {
                if (generated.line == index + 1) return original.line;
            }
            return 0;
        }
        return 0;
    };
    CHECK(line_of("auto n {CPP2_UFCS(ssize)(v)};") == 1);
    CHECK(line_of("return CPP2_ASSERT_IN_BOUNDS_LITERAL(v, 0) + cpp2::move(n);") == 1);
    CHECK(line_of("i += e;") == 5);
    CHECK(line_of("++i;") == 4);
    CHECK(line_of("std::cout << cpp2::move(i);") == 7);

    check_source_map("next clause", 8, result, translate(source, false));
}

TEST_CASE("source maps place data members and code after function expressions",
          "[source-map]") {
    const auto source = std::string{
        "S: type = {\n"                                // 1
        "    public a: int = 1;\n"                     // 2
        "\n"                                           // 3
        "    public b: int = 2;\n"                     // 4
        "    f: (this) -> int = a;\n"                  // 5
        "    public c: int = 3;\n"                     // 6
        "}\n"                                          // 7
        "main: () = {\n"                               // 8
        "    f := :(x: int) -> int = {\n"              // 9
        "        y := x + 1;\n"                        // 10
        "        return y;\n"                          // 11
        "    };\n"                                     // 12
        "    a := f(1);\n"                             // 13
        "    std::cout << a;\n"                        // 14
        "}\n"};                                        // 15
    const auto result = translate(source, true);
    REQUIRE_FALSE(result.has_errors());
    const auto lines = split_lines(result.cpp1);
    const auto line_of = [&](std::string_view text) -> std::uint32_t {
        for (std::uint32_t index = 0; index < lines.size(); ++index) {
            if (lines[index].find(text) == std::string_view::npos) continue;
            for (const auto& [generated, original] : result.source_map) {
                if (generated.line == index + 1) return original.line;
            }
            return 0;
        }
        return 0;
    };
    CHECK(line_of("int a {1};") == 2);
    CHECK(line_of("int b {2};") == 4);
    CHECK(line_of("int c {3};") == 6);
    CHECK(line_of("auto y {x + 1};") == 10);
    CHECK(line_of("auto a {cpp2::move(f)(1)};") == 13);
    CHECK(line_of("std::cout << cpp2::move(a);") == 14);

    check_source_map("function expression", 15, result, translate(source, false));
}

TEST_CASE("source maps agree with #line directives for the upstream corpus",
          "[source-map][upstream]") {
    const auto directory = std::filesystem::path{CPPFRONTIER_UPSTREAM_DIR} / "regression-tests";
    auto requests = std::vector<cppfrontier::file_request>{};
    auto names = std::vector<std::string>{};
    auto line_counts = std::vector<std::uint32_t>{};
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        const auto name = entry.path().filename().string();
        // It uses assignments as values, which cppfrontier rejects; the
        // corpus check rewrites it (tools/upstream-rewrites.pl).
        if (name == "pure2-last-use.cpp2") continue;
        if (entry.path().extension() != ".cpp2" || name.ends_with("-error.cpp2")) continue;
        const auto source = test_support::read_file(entry.path());
        line_counts.push_back(static_cast<std::uint32_t>(
            cppfrontier::source_text::from_bytes(source).lines().size()));
        names.push_back(name);
        for (const auto line_directives : {true, false}) {
            auto options = cppfrontier::translation_options{};
            options.source_path = name;
            options.line_directives = line_directives;
            requests.push_back({requests.size(), source, std::move(options)});
        }
    }
    REQUIRE(names.size() > 100);
    const auto results = cppfrontier::parallel_translator{}.translate_files(requests);
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto& result = results[2 * index].translation;
        CAPTURE(names[index]);
        REQUIRE_FALSE(result.has_errors());
        check_source_map(names[index], line_counts[index], result,
                         results[2 * index + 1].translation);
    }
}

TEST_CASE("source maps place the types of a namespace on their lines",
          "[source-map][review]") {
    const auto source = std::string{
        "N: namespace = {\n"                           // 1
        "\n"                                           // 2
        "    P: type = {\n"                            // 3
        "\n"                                           // 4
        "        public a: int = 0;\n"                 // 5
        "    }\n"                                      // 6
        "    Q: type = {\n"                            // 7
        "\n"                                           // 8
        "        public b: int = 0;\n"                 // 9
        "    }\n"                                      // 10
        "}\n"};                                        // 11
    const auto result = translate(source, true);
    REQUIRE_FALSE(result.has_errors());
    const auto lines = split_lines(result.cpp1);
    const auto line_of = [&](std::string_view text) -> std::uint32_t {
        for (std::uint32_t index = 0; index < lines.size(); ++index) {
            if (lines[index].find(text) == std::string_view::npos) continue;
            for (const auto& [generated, original] : result.source_map) {
                if (generated.line == index + 1) return original.line;
            }
            return 0;
        }
        return 0;
    };
    CHECK(line_of("struct P {") == 3);
    CHECK(line_of("int a {0};") == 5);
    CHECK(line_of("struct Q {") == 7);
    CHECK(line_of("int b {0};") == 9);

    check_source_map("namespace types", 11, result, translate(source, false));
}

TEST_CASE("#line directives escape the source path", "[source-map][hunt]") {
    auto options = cppfrontier::translation_options{};
    options.source_path = "C:\\dir\\a\"b.cpp2";
    const auto result = cppfrontier::translator{}.translate_source("main: () = { }\n", options);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("#line 1 \"C:\\\\dir\\\\a\\\"b.cpp2\"\n") != std::string::npos);
}
