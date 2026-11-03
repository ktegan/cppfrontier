#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include "../../src/scope_model.hpp"

namespace {

using cppfrontier::scope_model;

auto translate(const std::string& source) -> cppfrontier::translation_result {
    auto result = cppfrontier::translator{}.translate_source(source);
    REQUIRE_FALSE(result.has_errors());
    return result;
}

auto statements_of(const cppfrontier::ir_program& program, std::string_view name)
    -> const cppfrontier::statement_list& {
    for (const auto& declaration : program.declarations) {
        if (declaration.name == name) return declaration.function_statements->statements;
    }
    FAIL("no function " << name);
    static const auto empty = cppfrontier::statement_list{};
    return empty;
}

// The line of the declaration `name` names at `point`, or 0.
auto line_of(const scope_model& scopes, std::string_view name, cppfrontier::scope_point point)
    -> std::uint32_t {
    const auto* found = scopes.lookup(name, point);
    return found == nullptr ? 0 : found->span.begin.line;
}

}

TEST_CASE("a local is visible from its own declaration onward", "[scope-model]") {
    const auto result = translate(
        "x: int = 1;\n"
        "f: () = {\n"
        "    a := x;\n"
        "    x := 2;\n"
        "    b := x;\n"
        "}\n");
    const auto scopes = scope_model{result.program};
    const auto& body = statements_of(result.program, "f");
    CHECK(line_of(scopes, "x", scopes.at(*body[0])) == 1);
    CHECK(line_of(scopes, "x", scopes.at(*body[1])) == 4);
    CHECK(line_of(scopes, "x", scopes.at(*body[2])) == 4);
    const auto* local = scopes.lookup("x", scopes.at(*body[1]));
    REQUIRE(local != nullptr);
    CHECK(scopes.under_initialization(*local, scopes.at(*body[1])));
    CHECK_FALSE(scopes.under_initialization(*local, scopes.at(*body[2])));
    CHECK(scopes.declared_by(*body[1]) == local);
}

TEST_CASE("lookup finds parameters, members through this, and enclosing namespaces",
          "[scope-model]") {
    const auto result = translate(
        "N: namespace = {\n"
        "    limit: int = 3;\n"
        "    widget: type = {\n"
        "        size: int = 0;\n"
        "        grow: (inout this, by: int) = { size += by + limit; }\n"
        "    }\n"
        "}\n"
        "N: namespace = { more: int = 4; }\n"
        "g: () -> int = N::more + N::limit;\n");
    const auto scopes = scope_model{result.program};
    const auto* widget = scopes.lookup("N::widget", scopes.at(cppfrontier::source_position{9, 1}));
    REQUIRE(widget != nullptr);
    const auto* grow = scopes.member(*widget, "grow");
    REQUIRE(grow != nullptr);
    const auto inside = scopes.inside(grow->symbol);
    CHECK(line_of(scopes, "by", inside) == 5);
    CHECK(line_of(scopes, "size", inside) == 4);
    CHECK(line_of(scopes, "limit", inside) == 2);
    CHECK(scopes.enclosing_type(inside) == widget);
    // A reopened namespace shares its scope.
    CHECK(line_of(scopes, "N::more", scopes.at(cppfrontier::source_position{9, 1})) == 8);
    CHECK(line_of(scopes, "N::limit", scopes.at(cppfrontier::source_position{9, 1})) == 2);
    CHECK(scopes.parameter_of(grow->symbol, "by") != nullptr);
}

TEST_CASE("using declarations and directives name declarations in other scopes",
          "[scope-model]") {
    const auto result = translate(
        "A: namespace = { f: () = { } }\n"
        "B: namespace = { g: () = { } }\n"
        "h: () = {\n"
        "    using A::f;\n"
        "    using B::_;\n"
        "    f();\n"
        "    g();\n"
        "}\n");
    const auto scopes = scope_model{result.program};
    const auto& body = statements_of(result.program, "h");
    const auto* f = scopes.lookup("f", scopes.at(*body[2]));
    REQUIRE(f != nullptr);
    CHECK(f->kind == scope_model::entry_kind::using_name);
    CHECK(scopes.resolve(*f)->span.begin.line == 1);
    CHECK(line_of(scopes, "g", scopes.at(*body[3])) == 2);
    CHECK(line_of(scopes, "g", scopes.at(*body[0])) == 0);
}

TEST_CASE("statement parameters and function expressions nest where they appear",
          "[scope-model]") {
    const auto result = translate(
        "f: () = {\n"
        "    n := 1;\n"
        "    (copy i := n) while i < 3 next i++ { }\n"
        "    g := :(n: int) = n + 1;\n"
        "    _ = g(n);\n"
        "}\n");
    const auto scopes = scope_model{result.program};
    const auto& body = statements_of(result.program, "f");
    const auto& parameter = body[1]->as<cppfrontier::statement::block_form>().parameters.front();
    const auto* i = scopes.lookup("i", scopes.at(parameter));
    REQUIRE(i != nullptr);
    CHECK(i->kind == scope_model::entry_kind::statement_parameter);
    CHECK(scopes.under_initialization(*i, scopes.at(parameter)));
    CHECK(line_of(scopes, "i", scopes.at(*body[2])) == 0);

    const auto& expressions = result.program.function_expressions;
    REQUIRE(expressions.size() == 1);
    auto lambda = cppfrontier::invalid_symbol;
    for (const auto& declaration : result.program.declarations) {
        if (declaration.name == expressions.front().name) lambda = declaration.symbol;
    }
    // The parameter hides the local; the expression appears in `g`'s initializer.
    CHECK(line_of(scopes, "n", scopes.inside(lambda)) == 4);
    const auto* g = scopes.lookup("g", scopes.inside(lambda));
    REQUIRE(g != nullptr);
    CHECK(scopes.under_initialization(*g, scopes.inside(lambda)));
    CHECK(scopes.outside(lambda).scope == scopes.at(*body[2]).scope);
    // Positions inside the expression resolve to its parameter.
    CHECK(line_of(scopes, "n", scopes.at(cppfrontier::source_position{4, 22})) == 4);
    CHECK(line_of(scopes, "n", scopes.at(cppfrontier::source_position{5, 11})) == 2);
}
