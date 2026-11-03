#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include "../../src/scope_model.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

auto translate(const std::string& source, bool enabled = true)
    -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("condition-bindings"));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line, std::uint32_t column) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const cppfrontier::diagnostic& value) {
            return value.code == code && value.span.begin.line == line &&
                value.span.begin.column == column;
        });
}

auto contains(const cppfrontier::translation_result& result, std::string_view text) -> bool {
    return result.cpp1.find(text) != std::string::npos;
}

// Compiles the translation with g++ and returns the program's output.
auto run(const cppfrontier::translation_result& translation, std::string_view name)
    -> std::string {
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-condition-bindings");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (std::string{name} + ".cpp");
    const auto executable_path = scratch / name;
    test_support::write_file(cpp1_path, translation.cpp1);
    const auto compiled = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
        scratch, std::chrono::seconds{60});
    INFO("compiler output: " << compiled.output);
    REQUIRE(compiled.exit_status == 0);
    const auto ran = test_support::run_process({executable_path.string()}, scratch,
                                               std::chrono::seconds{10});
    REQUIRE(ran.exit_status == 0);
    return ran.output;
}

}

TEST_CASE("condition bindings are errors without the experiment",
          "[experiments][condition-bindings]") {
    const auto result = translate(
        "f: (v: std::variant<int, std::pair<int, int>>) -> int = {\n"
        "    if v is int as n { return n; }\n"
        "    if v is std::pair<int, int>(a, b) { return a; }\n"
        "    return 0;\n"
        "}\n", false);
    CHECK(has_diagnostic(result, "mixed-is-as", 2, 17));
    CHECK(has_diagnostic(result, "juxtaposed-operands", 3, 32));
}

TEST_CASE("an if condition's pattern tests lower to nested ifs",
          "[experiments][condition-bindings]") {
    const auto result = translate(
        "f: (v: std::variant<int, std::string>, k: bool) -> int = {\n"
        "    if k && v is int as n && n > 0 { return n; }\n"
        "    if v is std::string as s { return 1; }\n"
        "    else if v is int as m { return m; }\n"
        "    else { return 2; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result,
        "    if (k) {\n"
        "        if (auto&& __cpp2_subject_2_5_1 = v; cpp2::impl::is<int>(__cpp2_subject_2_5_1)) {\n"
        "            [[maybe_unused]] auto&& n = cpp2::impl::as<int>(__cpp2_subject_2_5_1);\n"
        "            if (cpp2::impl::cmp_greater(n,0)) {\n"
        "#line 2\n"
        "                return CPP2_FORWARD(n);\n"));
    // A body that returns needs no flag: the else, or an else-if chain, follows it.
    CHECK(contains(result,
        "    {\n"
        "        if (auto&& __cpp2_subject_3_5_0 = v; cpp2::impl::is<std::string>("
        "__cpp2_subject_3_5_0)) {\n"
        "            [[maybe_unused]] auto&& s = cpp2::impl::as<std::string>("
        "__cpp2_subject_3_5_0);\n"
        "#line 3\n"
        "            return 1;\n"
        "        }\n"
        "        {\n"
        "            {\n"
        "                if (auto&& __cpp2_subject_4_5_0 = v;"));
    CHECK_FALSE(contains(result, "__cpp2_matched"));
}

TEST_CASE("an else runs when the body's flag is unset, and needs none after a jump",
          "[experiments][condition-bindings][hunt]") {
    const auto result = translate(
        "#include <iostream>\n"
        "#include <variant>\n"
        "f: (v: std::variant<int, std::string>) -> int = {\n"
        "    if v is int as n { return n; }\n"
        "    else { return 0; }\n"
        "}\n"
        "g: (v: std::variant<int, std::string>) -> int = {\n"
        "    r := 0;\n"
        "    if v is int as n { r = n; }\n"
        "    else { r = 1; }\n"
        "    return r;\n"
        "}\n"
        "main: () = { std::cout << f(3) << g(4) << g(\"x\"); }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK_FALSE(contains(result, "__cpp2_matched_4_5"));
    CHECK(contains(result,
        "        bool __cpp2_matched_9_5 = false;\n"));
    CHECK(contains(result,
        "            __cpp2_matched_9_5 = true;\n"
        "#line 9\n"
        "            r = CPP2_FORWARD(n);\n"
        "        }\n"
        "        if (!__cpp2_matched_9_5) {\n"));
    // Both branches of `f` return, which -Wreturn-type sees.
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-condition-bindings");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "else-after-jump.cpp";
    test_support::write_file(cpp1_path, result.cpp1);
    const auto compiled = test_support::run_process({compiler, "-std=c++23", "-Wall", "-Wextra",
        "-Werror", "-fsyntax-only", "-I", CPPFRONTIER_RUNTIME_INCLUDE, cpp1_path.string()},
        scratch, std::chrono::seconds{60});
    INFO("compiler output: " << compiled.output);
    CHECK(compiled.exit_status == 0);
    CHECK(run(result, "else-after-jump") == "341");
}

TEST_CASE("a while condition's pattern tests leave the loop when one fails",
          "[experiments][condition-bindings]") {
    const auto result = translate(
        "f: (inout q: std::queue<std::optional<int>>) -> int = {\n"
        "    sum := 0;\n"
        "    while !q.empty() && q.front() is int as x next q.pop() { sum += x; }\n"
        "    return sum;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result,
        "    for (;; CPP2_UFCS(pop)(q)) {\n"
        "        if (!(!CPP2_UFCS(empty)(q))) break;\n"
        "        auto&& __cpp2_subject_3_5_1 = CPP2_UFCS(front)(q);\n"
        "        if (!(cpp2::impl::is<int>(__cpp2_subject_3_5_1))) break;\n"
        "        [[maybe_unused]] auto&& x = cpp2::impl::as<int>(__cpp2_subject_3_5_1);\n"
        "#line 3\n"
        "        sum += CPP2_FORWARD(x);\n"
        "    }\n"));
}

TEST_CASE("destructuring and tuple patterns bind in a condition",
          "[experiments][condition-bindings]") {
    const auto result = translate(
        "f: (v: std::optional<std::pair<int, int>>, p: std::pair<int, int>) -> int = {\n"
        "    if v is std::pair<int, int>(a, _) && p is (c, d) { return a + c + d; }\n"
        "    return 0;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result, "[[maybe_unused]] auto&& [a, __cpp2_unused_0] = "
                           "cpp2::impl::as<std::pair<int, int>>(__cpp2_subject_2_5_0);"));
    CHECK(contains(result, "if (auto&& __cpp2_subject_2_5_1 = p; true) {"));
    CHECK(contains(result, "[[maybe_unused]] auto&& [c, d] = __cpp2_subject_2_5_1;"));
}

TEST_CASE("a pattern test binds only as an operand of an if or while condition's && chain",
          "[experiments][condition-bindings]") {
    const auto result = translate(
        "f: (v: std::variant<int, std::string>, k: bool) -> int = {\n"
        "    if k || v is int as a { return 0; }\n"
        "    if !(v is int as b) { return 0; }\n"
        "    if (v is int as c) { return 0; }\n"
        "    x := v is int as d;\n"
        "    y := k && v is int as e;\n"
        "    do { } while v is int as f;\n"
        "    if constexpr v is int as g { }\n"
        "    while k next v is int as h { }\n"
        "    for (v is int as i) do (j) { }\n"
        "    return x + y;\n"
        "}\n");
    for (const auto& [line, column] : std::initializer_list<std::pair<std::uint32_t,
                                                                      std::uint32_t>>{
             {2, 15}, {3, 12}, {4, 11}, {5, 12}, {6, 17}, {7, 20}, {8, 20}, {9, 20},
             {10, 12}}) {
        CAPTURE(line, column);
        CHECK(has_diagnostic(result, "condition-binding-position", line, column));
    }
    CHECK_FALSE(has_diagnostic(result, "mixed-is-as", 2, 22));
}

TEST_CASE("condition bindings are names, each bound once",
          "[experiments][condition-bindings]") {
    const auto result = translate(
        "f: (v: std::variant<int, std::pair<int, int>>) -> int = {\n"
        "    if v is int as _ { return 0; }\n"
        "    if v is std::pair<int, int>(a, a) { return 0; }\n"
        "    if v is int as n && v is int as n { return 0; }\n"
        "    if v is std::pair<int, int>(1, b) { return 0; }\n"
        "    if v is std::pair<int, int>() { return 0; }\n"
        "    if v is int as this { return 0; }\n"
        "    return 0;\n"
        "}\n");
    CHECK(has_diagnostic(result, "invalid-condition-binding", 2, 20));
    CHECK(has_diagnostic(result, "invalid-condition-binding", 3, 36));
    CHECK(has_diagnostic(result, "invalid-condition-binding", 4, 37));
    CHECK(has_diagnostic(result, "invalid-condition-binding", 5, 33));
    CHECK(has_diagnostic(result, "invalid-condition-binding", 6, 32));
    CHECK(has_diagnostic(result, "invalid-condition-binding", 7, 20));
}

TEST_CASE("condition bindings are locals of the guarded body only",
          "[experiments][condition-bindings]") {
    const auto result = translate(
        "f: (v: std::variant<int, std::string>) -> int = {\n"
        "    if v is int as n && n > 0 {\n"
        "        return n;\n"
        "    } else {\n"
        "        return 0;\n"
        "    }\n"
        "}\n");
    REQUIRE_FALSE(result.has_errors());
    const auto& program = result.program;
    const auto scopes = cppfrontier::scope_model{program};
    const auto& if_statement = *program.declarations.front().function_statements->statements[0];
    const auto& form = if_statement.as<cppfrontier::statement::if_form>();
    REQUIRE(form.bindings.size() == 1);
    CHECK(form.bindings.front().symbol != cppfrontier::invalid_symbol);
    const auto* binding = scopes.declared_by(form.bindings.front());
    REQUIRE(binding != nullptr);
    CHECK(scopes.lookup("n", scopes.at(*form.body.front())) == binding);
    CHECK(scopes.lookup("n", scopes.at(*form.else_body.front())) == nullptr);
    CHECK(scopes.lookup("n", scopes.at(if_statement)) == nullptr);
    CHECK(scopes.lookup("n", scopes.at(cppfrontier::source_position{2, 25})) == binding);
}

TEST_CASE("a pattern test's subject is not moved while a binding refers to it",
          "[experiments][condition-bindings]") {
    const auto result = translate(
        "use: (s: std::string) = { std::cout << s; }\n"
        "f: () = {\n"
        "    v: std::optional<std::string> = (\"a\");\n"
        "    if v is std::string as s {\n"
        "        use(v.value());\n"
        "        use(s);\n"
        "    }\n"
        "    w: std::optional<std::string> = (\"b\");\n"
        "    if w is std::string as t { use(t); }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result, "use(CPP2_UFCS(value)(v));"));
    CHECK(contains(result, "use(CPP2_FORWARD(s));"));
    CHECK(contains(result, "auto&& __cpp2_subject_9_5_0 = w;"));
}

TEST_CASE("a condition binding follows the flow analyses",
          "[experiments][condition-bindings]") {
    auto options = cppfrontier::translation_options{};
    REQUIRE(options.experiments.enable("condition-bindings"));
    REQUIRE(options.experiments.enable("use-after-move"));
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (copy v: std::optional<std::string>, copy w: std::string) -> int = {\n"
        "    if v is std::string as s && s.empty() {\n"
        "        _ = (move w);\n"
        "        return 0;\n"
        "    }\n"
        "    return w.ssize() as int;\n"
        "}\n"
        "g: (copy w: std::string) -> int = {\n"
        "    if w.empty() is bool as e && e {\n"
        "        _ = (move w);\n"
        "    }\n"
        "    return w.ssize() as int;\n"
        "}\n", options);
    CHECK_FALSE(has_diagnostic(result, "use-after-move", 6, 12));
    CHECK(has_diagnostic(result, "use-after-maybe-move", 12, 5));
}

TEST_CASE("condition bindings compile and run", "[experiments][condition-bindings]") {
    const auto result = translate(
        "describe: (v: std::variant<int, std::string>) -> std::string = {\n"
        "    if v is int as n && n > 9 { return \"big \" + std::to_string(n); }\n"
        "    else if v is int as n { return \"small \" + std::to_string(n); }\n"
        "    else if v is std::string as s { return s; }\n"
        "    return \"?\";\n"
        "}\n"
        "main: () = {\n"
        "    std::cout << describe(42) << \", \" << describe(3) << \", \" << describe(\"x\");\n"
        "    q: std::queue<std::optional<int>> = ();\n"
        "    q.push(1); q.push(2); q.push(std::nullopt); q.push(4);\n"
        "    sum := 0;\n"
        "    while !q.empty() && q.front() is int as x next q.pop() { sum += x; }\n"
        "    std::cout << \", \" << sum << \"\\n\";\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(run(result, "condition_bindings") == "big 42, small 3, x, 3\n");
}
