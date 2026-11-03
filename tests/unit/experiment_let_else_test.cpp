#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include "../../src/scope_model.hpp"

#include <algorithm>
#include <string>

namespace {

auto translate(const std::string& source, std::initializer_list<std::string_view> experiments =
                                              {"let-else"}) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    for (const auto name : experiments) REQUIRE(options.experiments.enable(name));
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

}

TEST_CASE("let-else is an error without the experiment", "[experiments][let-else]") {
    const auto result = translate(
        "f: (o: std::optional<int>) -> int = {\n"
        "    n := o as int else { return 0; }\n"
        "    return n;\n"
        "}\n", {});
    CHECK(has_diagnostic(result, "incomplete-expression", 2, 19));
}

TEST_CASE("let-else lowers to a test of a held value", "[experiments][let-else]") {
    const auto result = translate(
        "f: (o: std::optional<int>) -> int = {\n"
        "    n := o as int else { return 0; }\n"
        "    return n;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result,
        "    auto&& __cpp2_let_2_5 = o;\n"
        "    if (!cpp2::impl::is<int>(__cpp2_let_2_5)) {\n"
        "#line 2\n"
        "        return 0;\n"
        "    }\n"
        "    auto&& n = cpp2::impl::as<int>(__cpp2_let_2_5);\n"));
    CHECK(contains(result, "    return CPP2_FORWARD(n);\n"));
}

TEST_CASE("let-else destructures with the destructuring experiment",
          "[experiments][let-else]") {
    const auto result = translate(
        "f: (p: std::optional<std::pair<int, int>>) -> int = {\n"
        "    (a, _) := p as std::pair<int, int> else { std::terminate(); }\n"
        "    return a;\n"
        "}\n", {"let-else", "destructuring"});
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result, "    [[maybe_unused]] auto&& [a, __cpp2_unused_2_9] = "
                           "cpp2::impl::as<std::pair<int, int>>(__cpp2_let_2_5);\n"));
    CHECK(contains(result, "    return a;\n"));
}

TEST_CASE("a let-else initializer is a cast and its else block leaves",
          "[experiments][let-else]") {
    const auto result = translate(
        "f: (o: std::optional<int>, k: bool) -> int = {\n"
        "    a := o.value_or(0) else { return 0; }\n"
        "    b := o as int else { }\n"
        "    c := o as int else { if k { return 1; } }\n"
        "    d := o as int else { if k { return 1; } else { throw(2); } }\n"
        "    e := o as int else { std::abort(); }\n"
        "    while k {\n"
        "        g := o as int else { break; }\n"
        "        h := o as int else { continue; }\n"
        "    }\n"
        "    i := o as int else { { return 0; } }\n"
        "    return a + b + c + d + e + i;\n"
        "}\n");
    CHECK(has_diagnostic(result, "let-else-initializer", 2, 7));
    CHECK(has_diagnostic(result, "let-else-falls-through", 3, 19));
    CHECK(has_diagnostic(result, "let-else-falls-through", 4, 26));
    CHECK(has_diagnostic(result, "let-else-falls-through", 11, 26));
    CHECK(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto& value) { return value.code == "let-else-falls-through"; })
          == 3);
}

TEST_CASE("a let-else name is in scope after the statement only", "[experiments][let-else]") {
    const auto result = translate(
        "f: (o: std::optional<int>) -> int = {\n"
        "    n := o as int else {\n"
        "        return 0;\n"
        "    }\n"
        "    return n;\n"
        "}\n");
    REQUIRE_FALSE(result.has_errors());
    const auto& program = result.program;
    const auto scopes = cppfrontier::scope_model{program};
    const auto& statements = program.declarations.front().function_statements->statements;
    const auto& form = statements[0]->as<cppfrontier::statement::declaration_form>();
    REQUIRE(form.is_let_else);
    const auto* declared = scopes.declared_by(*statements[0]);
    REQUIRE(declared != nullptr);
    CHECK(scopes.lookup("n", scopes.at(*form.else_body.front())) == nullptr);
    CHECK(scopes.lookup("n", scopes.at(*statements[0])) == nullptr);
    CHECK(scopes.lookup("n", scopes.at(*statements[1])) == declared);
}

TEST_CASE("a let-else else block is a path that leaves", "[experiments][let-else]") {
    const auto uninitialized = translate(
        "f: (o: std::optional<int>) -> int = {\n"
        "    x: int;\n"
        "    n := o as int else { x = 1; return x; }\n"
        "    return x + n;\n"
        "}\n");
    CHECK(has_diagnostic(uninitialized, "use-before-initialization", 4, 12));
    const auto result = translate(
        "g: (copy s: std::string, o: std::optional<int>) -> std::string = {\n"
        "    n := o as int else { return s; }\n"
        "    return s + std::to_string(n);\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result, "        return s;\n"));
    CHECK(contains(result, "return CPP2_FORWARD(s) + std::to_string(CPP2_FORWARD(n));"));
}

TEST_CASE("a let-else value is not moved while its name is used", "[experiments][let-else]") {
    const auto result = translate(
        "use: (s: std::string) = { std::cout << s; }\n"
        "f: () = {\n"
        "    v: std::optional<std::string> = (\"a\");\n"
        "    s := v as std::string else { return; }\n"
        "    use(s);\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result, "auto&& __cpp2_let_4_5 = v;"));
    CHECK(contains(result, "use(CPP2_FORWARD(s));"));
}

TEST_CASE("defer and try-operator rules apply in a let-else block", "[experiments][let-else]") {
    const auto result = translate(
        "f: (o: std::optional<int>) = {\n"
        "    defer { n := o as int else { return; } std::cout << n; }\n"
        "}\n", {"let-else", "defer"});
    CHECK(has_diagnostic(result, "defer-control-flow", 2, 34));
}
