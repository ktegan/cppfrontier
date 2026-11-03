#include <catch2/catch_all.hpp>

#include "../../src/ir_walk.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

auto make_statement(std::string name) -> std::unique_ptr<cppfrontier::statement> {
    auto statement = std::make_unique<cppfrontier::statement>();
    statement->as<cppfrontier::statement::expression_form>().text =
        cppfrontier::parsed_expression{std::move(name)};
    return statement;
}

}

TEST_CASE("IR declaration traversal is post-order and visits nested statements",
          "[ir][traversal]") {
    auto program = cppfrontier::ir_program{};
    auto outer = cppfrontier::ir_declaration{};
    outer.name = "outer";
    auto inner = cppfrontier::ir_declaration{};
    inner.name = "inner";
    outer.members.push_back(std::move(inner));

    auto body = std::make_shared<cppfrontier::function_body>();
    auto parent = std::make_unique<cppfrontier::statement>(cppfrontier::statement_kind::if_statement);
    auto& branch = parent->as<cppfrontier::statement::if_form>();
    branch.condition = cppfrontier::parsed_expression{"parent"};
    branch.body.push_back(make_statement("child"));
    branch.else_body.push_back(make_statement("else_child"));
    body->statements.push_back(std::move(parent));
    outer.function_statements = std::move(body);
    program.declarations.push_back(std::move(outer));

    auto declarations = std::vector<std::string>{};
    auto statements = std::vector<std::string>{};
    cppfrontier::ir_detail::visit_declarations(program.declarations,
        [&](const cppfrontier::ir_declaration& declaration) {
            declarations.push_back(declaration.name);
        },
        [&](const cppfrontier::statement& statement) {
            const auto* text = cppfrontier::statement_text(statement);
            statements.push_back(text ? text->source() :
                statement.as<cppfrontier::statement::if_form>().condition.source());
        });

    CHECK(declarations == std::vector<std::string>{"inner", "outer"});
    CHECK(statements == std::vector<std::string>{"parent", "child", "else_child"});
}

