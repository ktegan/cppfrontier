#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

auto translate(const std::string& source, bool enabled = true) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("comprehension"));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) { return issue.code == code && issue.span.begin.line == line; });
}

// Compiles the translation of `source`; the output is the compiler's when it
// fails, else the program's.
auto compile_and_run(const std::string& source, std::string_view name, bool expect_compiled = true)
    -> std::string {
    const auto translation = translate(source);
    INFO(translation.cpp1);
    for (const auto& issue : translation.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(translation.has_errors());
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-comprehension");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (std::string{name} + ".cpp");
    const auto executable_path = scratch / name;
    test_support::write_file(cpp1_path, translation.cpp1);
    const auto compiled = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
        scratch, std::chrono::seconds{180});
    INFO("compiler output: " << compiled.output);
    REQUIRE((compiled.exit_status == 0) == expect_compiled);
    if (!expect_compiled) return compiled.output;
    const auto ran = test_support::run_process({executable_path.string()}, scratch,
                                               std::chrono::seconds{10});
    REQUIRE_FALSE(ran.timed_out);
    REQUIRE(ran.exit_status == 0);
    return ran.output;
}

}

TEST_CASE("a for-comprehension lowers to cpp2::comprehend over lambdas",
          "[experiments][comprehension]") {
    const auto result = translate(
        "f: (xs: std::vector<int>, a: std::optional<int>) = {\n"
        "    evens := for xs do (x) if x % 2 == 0 yield x * 2;\n"
        "    all := for xs do (x) yield x;\n"
        "    pairs := for xs do (x) for a do (y) yield x + y;\n"
        "    guarded := for xs do (x) if x > 0 for a do (y) if y > 0 yield x * y;\n"
        "}\n");
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("#include \"cpp2comprehension.h\"") != std::string::npos);
    CHECK(result.cpp1.find(
        "auto evens {cpp2::comprehend(xs, [&](auto const& x) { return x % "
        "CPP2_ASSERT_NOT_ZERO_LITERAL(CPP2_TYPEOF(x),2) == 0; }, "
        "[&](auto const& x) { return x * 2; })};") != std::string::npos);
    CHECK(result.cpp1.find("auto all {cpp2::comprehend(xs, [&](auto const& x) { return x; })};")
          != std::string::npos);
    CHECK(result.cpp1.find(
        "auto pairs {cpp2::comprehend_bind(xs, [&](auto const& x) { return cpp2::comprehend(a, "
        "[&, x](auto const& y) { return x + y; }); })};") != std::string::npos);
    // A lambda copies only the enclosing bound names its body uses.
    CHECK(result.cpp1.find(
        "auto guarded {cpp2::comprehend_bind(xs, [&](auto const& x) { return "
        "cpp2::impl::cmp_greater(x,0); }, [&](auto const& x) { return cpp2::comprehend(a, "
        "[&](auto const& y) { return cpp2::impl::cmp_greater(y,0); }, "
        "[&, x](auto const& y) { return x * y; }); })};") != std::string::npos);
}

TEST_CASE("a for-comprehension is an expression in any expression position",
          "[experiments][comprehension]") {
    const auto result = translate(
        "f: (xs: std::vector<int>) -> int = {\n"
        "    n := std::ranges::distance(for xs do (x) yield x);\n"
        "    v: std::vector<int> = std::ranges::to<std::vector<int>>(\n"
        "        for xs do (x) // keep the comment\n"
        "            yield x);\n"
        "    return for v do (x) yield x + n;\n"
        "}\n"
        "g: (xs: std::vector<int>) = for xs do (x) yield x;\n");
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("std::ranges::distance(cpp2::comprehend(xs, [&](auto const& x) "
                           "{ return x; }))") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::comprehend(xs, // keep the comment\n") != std::string::npos);
    CHECK(result.cpp1.find("return cpp2::comprehend(v, [&](auto const& x) { return x + n; });")
          != std::string::npos);
    CHECK(result.cpp1.find("{ return cpp2::comprehend(xs, [&](auto const& x) { return x; }); }")
          != std::string::npos);
}

TEST_CASE("names in a for-comprehension are not moved from", "[experiments][comprehension]") {
    const auto result = translate(
        "f: (copy xs: std::vector<int>) -> int = {\n"
        "    k := 2;\n"
        "    v := for xs do (x) yield x * k;\n"
        "    return std::ranges::distance(v);\n"
        "}\n");
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("cpp2::move(k)") == std::string::npos);
    CHECK(result.cpp1.find("cpp2::move(xs)") == std::string::npos);
    CHECK(result.cpp1.find("std::ranges::distance(cpp2::move(v))") != std::string::npos);
}

TEST_CASE("range comprehensions are lazy views that materialize and nest",
          "[experiments][comprehension]") {
    const auto output = compile_and_run(
        "#include <iostream>\n"
        "#include <ranges>\n"
        "main: () = {\n"
        "    xs: std::vector<int> = (1, 2, 3, 4, 5, 6);\n"
        "    evens := for xs do (x) if x % 2 == 0 yield x * 2;\n"
        "    doubled: std::vector<int> = std::ranges::to<std::vector<int>>(evens);\n"
        "    for doubled do (d) std::cout << d << \" \";\n"
        "    std::cout << \"| \" << doubled.size() << \"\\n\";\n"
        "    ys: std::vector<int> = (10, 20);\n"
        "    pairs := for xs do (x) if x < 3 for ys do (y) yield x + y;\n"
        "    for pairs do (p) std::cout << p << \" \";\n"
        "    std::cout << \"\\n\";\n"
        "    squares := for 1 ..< 4 do (i) yield i * i;\n"
        "    for squares do (s) std::cout << s << \" \";\n"
        "    std::cout << \"\\n\";\n"
        "}\n",
        "comprehension-ranges");
    CHECK(output == "4 8 12 | 3\n11 21 12 22 \n1 4 9 \n");
}

TEST_CASE("optional and expected comprehensions chain through and_then and transform",
          "[experiments][comprehension]") {
    const auto output = compile_and_run(
        "#include <iostream>\n"
        "#include <optional>\n"
        "#include <expected>\n"
        "#include <string>\n"
        "half: (x: int) -> std::optional<int> = {\n"
        "    if x % 2 == 0 { return x / 2; }\n"
        "    return std::nullopt;\n"
        "}\n"
        "parse: (s: std::string) -> std::expected<int, std::string> = {\n"
        "    if s.empty() { return std::unexpected(\"empty\"); }\n"
        "    return std::stoi(s);\n"
        "}\n"
        "main: () = {\n"
        "    a := half(4);\n"
        "    b := half(6);\n"
        "    none := half(3);\n"
        "    sum := for a do (p) for b do (q) yield p + q;\n"
        "    missing := for a do (p) for none do (q) yield p + q;\n"
        "    kept := for a do (p) if p > 1 yield p * 10;\n"
        "    dropped := for a do (p) if p > 5 yield p * 10;\n"
        "    std::cout << sum.value_or(-1) << \" \" << missing.value_or(-1) << \" \"\n"
        "              << kept.value_or(-1) << \" \" << dropped.value_or(-1) << \"\\n\";\n"
        "    total := for parse(\"20\") do (m) for parse(\"22\") do (n) yield m + n;\n"
        "    failed := for parse(\"1\") do (m) for parse(\"\") do (n) yield m + n;\n"
        "    plus := for parse(\"41\") do (m) yield m + 1;\n"
        "    std::cout << total.value_or(0) << \" \" << failed.has_value() << \" \"\n"
        "              << failed.error() << \" \" << plus.value_or(0) << \"\\n\";\n"
        "}\n",
        "comprehension-monads");
    CHECK(output == "5 -1 20 -1\n42 0 empty 42\n");
}

TEST_CASE("a guard on an expected fails to compile with a message",
          "[experiments][comprehension]") {
    const auto output = compile_and_run(
        "#include <expected>\n"
        "#include <string>\n"
        "main: () = {\n"
        "    e: std::expected<int, std::string> = 5;\n"
        "    g := for e do (x) if x > 1 yield x;\n"
        "    _ = g.has_value();\n"
        "}\n",
        "comprehension-expected-guard", false);
    CHECK(output.find("a for-comprehension 'if' guard needs an optional source")
          != std::string::npos);
}

TEST_CASE("a for-comprehension without yield is rejected", "[experiments][comprehension]") {
    const auto result = translate(
        "f: (xs: std::vector<int>) = {\n"
        "    a := for xs do (x) x * 2;\n"
        "    b := for xs do (x) if x > 1;\n"
        "    c := for xs do (x: int) yield x;\n"
        "    d := for xs do (x) yield x;\n"
        "}\n");
    CHECK(has_diagnostic(result, "comprehension-yield", 2));
    CHECK(has_diagnostic(result, "comprehension-yield", 3));
    CHECK(has_diagnostic(result, "comprehension-yield", 4));
    CHECK_FALSE(has_diagnostic(result, "comprehension-yield", 5));
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) { return issue.code == "comprehension-yield"; });
    REQUIRE(found != result.diagnostics.end());
    CHECK(found->span.begin.column == 10);
    CHECK(found->message ==
          "a for-comprehension is 'for range do (name) [if guard] yield value' - 'yield' is "
          "missing");
}

TEST_CASE("a for-comprehension is a syntax error unless the experiment is enabled",
          "[experiments][comprehension]") {
    const auto source = std::string{
        "f: (xs: std::vector<int>) -> int = {\n"
        "    return std::ranges::distance(for xs do (x) yield x);\n"
        "}\n"
        "g: (xs: std::vector<int>, a: std::optional<int>) = {\n"
        "    pairs: std::vector<int> = for xs do (x) for a do (y) yield x + y;\n"
        "}\n"};
    const auto off = translate(source, false);
    CHECK(has_diagnostic(off, "statement-as-initializer", 2));
    CHECK(has_diagnostic(off, "statement-as-initializer", 5));
    CHECK(off.diagnostics.front().message == "'for' begins a statement, not an expression");
    CHECK_FALSE(translate(source).has_errors());

    // With the experiment, `v := for ...` is an initializer, not a loop label.
    const auto inferred = std::string{
        "main: () = {\n    xs: std::vector<int> = (1, 2);\n    v := for xs do (x) yield x;\n}\n"};
    const auto initialized = translate(inferred);
    CHECK_FALSE(initialized.has_errors());
    CHECK(initialized.cpp1.find("auto v {cpp2::comprehend(xs") != std::string::npos);

    const auto plain = std::string{
        "main: () = {\n"
        "    xs: std::vector<int> = (1, 2);\n"
        "    for xs do (x) std::cout << x;\n"
        "    yield := 1;\n"
        "    yield++;\n"
        "}\n"};
    REQUIRE_FALSE(translate(plain, false).has_errors());
    CHECK(translate(plain, false).cpp1 == translate(plain).cpp1);
    CHECK(translate(plain).cpp1.find("cpp2comprehension.h") == std::string::npos);
}

TEST_CASE("a for-comprehension's bound name hides an outer local",
          "[experiments][comprehension]") {
    // The deferred local `x` is neither read nor rendered as `x.value()`.
    const auto result = translate(
        "f: (xs: std::vector<int>) -> int = {\n"
        "    x: int;\n"
        "    n := std::ranges::distance(for xs do (x) if x > 1 yield x + 1);\n"
        "    x = 1;\n"
        "    return n + x;\n"
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("(auto const& x) { return cpp2::impl::cmp_greater(x,1); }") !=
          std::string::npos);
    CHECK(result.cpp1.find("(auto const& x) { return x + 1; }") != std::string::npos);
}

TEST_CASE("a for-comprehension outside a function captures nothing by default",
          "[experiments][comprehension]") {
    const auto result = translate(
        "xs: std::vector<int> = (1, 2);\n"
        "doubled: std::vector<int> =\n"
        "    std::ranges::to<std::vector<int>>(for xs do (x) for xs do (y) yield x * y);\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("cpp2::comprehend_bind(xs, [](auto const& x) { return "
                           "cpp2::comprehend(xs, [x](auto const& y) { return x * y; }); })") !=
          std::string::npos);
}

TEST_CASE("a for loop iterates a parenthesized for-comprehension",
          "[experiments][comprehension]") {
    const auto output = compile_and_run(
        "#include <iostream>\n"
        "#include <vector>\n"
        "main: () = {\n"
        "    xs: std::vector<int> = (1, 2, 3);\n"
        "    for (for xs do (x) if x != 2 yield x * 10) do (v) std::cout << v << \" \";\n"
        "}\n", "loop-over-comprehension");
    CHECK(output == "10 30 ");
}

TEST_CASE("an inner comprehension copies only the bound names it uses",
          "[experiments][comprehension][hunt]") {
    const auto result = translate(
        "half: (n: int) -> std::optional<int> = n / 2;\n"
        "main: () = {\n"
        "    quarter := for half(8) do (h) for half(h) do (q) yield q;\n"
        "    pairs := for half(8) do (h) for half(h) do (q) yield h + q;\n"
        "    std::cout << quarter.value_or(-1) << pairs.value_or(-1);\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    // clang's -Wunused-lambda-capture reports a copied name that the body does not use.
    CHECK(result.cpp1.find("cpp2::comprehend(half(h), [&](auto const& q) { return q; })") !=
          std::string::npos);
    CHECK(result.cpp1.find("cpp2::comprehend(half(h), [&, h](auto const& q) { return h + q; })") !=
          std::string::npos);
}
