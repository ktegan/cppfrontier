#include <catch2/catch_all.hpp>

#include "../support/statement_forms.hpp"
#include <iostream>
#include <cppfrontier/pipeline.hpp>

using namespace cppfrontier::test;

namespace {

auto parse_source = [](const std::string& text) {
    const auto source = cppfrontier::source_text::from_bytes(text);
    auto tokens = std::vector<cppfrontier::token>{};
    for (const auto& section : source.cpp2_sections()) {
        auto section_tokens = cppfrontier::tokenize(
            source, section.begin_line, section.end_line_exclusive);
        tokens.insert(tokens.end(), section_tokens.begin(), section_tokens.end());
    }
    return cppfrontier::parse_cpp2(source, tokens);
};

auto has_diagnostic = [](const cppfrontier::parse_result& parsed, std::string_view code) {
    return std::any_of(parsed.diagnostics.begin(), parsed.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
};

}

TEST_CASE("unterminated parameters terminate with a diagnostic", "[parser][recovery]") {
    const auto parsed = parse_source("f: (x\n");

    CHECK(parsed.tree.declarations.empty());
    CHECK(has_diagnostic(parsed, "unterminated-parameters"));
}


TEST_CASE("mixed is-as variant corpus source terminates parsing", "[parser][recovery][upstream]") {
    auto mixed_source = std::ostringstream{};
    mixed_source << R"cpp2(test: (forward v) = {
    v is std::monostate;
}

main: () -> int = {
    v: std::variant<std::monostate,
                    X< 1>, X< 2>, X<20> > = ();
}
)cpp2";

    const auto parsed = parse_source(mixed_source.str());
    CHECK(parsed.tree.declarations.size() == 2);
}

TEST_CASE("simple function declaration parses directly", "[parser][unit]") {
    const auto parsed = parse_source("hello: () = \"hello\";\n");
    REQUIRE(parsed.tree.declarations.size() == 1);
    CHECK(parsed.tree.declarations[0]->name == "hello");
    CHECK(parsed.tree.declarations[0]->kind == cppfrontier::declaration::node_kind::function);
    REQUIRE(parsed.tree.declarations[0]->body);
    CHECK(parsed.tree.declarations[0]->body->text.source() == "\"hello\"");
}

TEST_CASE("expression bodies retain their complete expression", "[parser][expression-body]") {
    const auto parsed = parse_source("answer: () = 40 + 2;\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    REQUIRE(parsed.tree.declarations[0]->body);
    CHECK(parsed.tree.declarations[0]->body->text.source() == "40 + 2");
    CHECK(parsed.tree.declarations[0]->body->span.begin.column == 14);
    CHECK(parsed.tree.declarations[0]->body->span.end.column == 20);
}

TEST_CASE("copy parameters retain their passing style", "[parser][parameters]") {
    const auto parsed = parse_source(
        "copy_value: (copy text: std::string) -> std::string = { return text; }\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    REQUIRE(parsed.tree.declarations[0]->parameters.size() == 1);
    CHECK(parsed.tree.declarations[0]->parameters[0].passing == cppfrontier::passing_style::copy);
    CHECK(parsed.tree.declarations[0]->parameters[0].type.text == "std::string");
    CHECK(parsed.tree.declarations[0]->parameters[0].name == "text");
}

TEST_CASE("parsed declaration lowers to IR", "[parser][unit][ir]") {
    const auto source = cppfrontier::source_text::from_bytes("hello: () = \"hello\";\n");
    const auto tokens = cppfrontier::tokenize(source, 1, source.lines().size() + 1);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    REQUIRE(parsed.tree.declarations.size() == 1);

    const auto built = cppfrontier::build_ir(parsed.tree);
    REQUIRE(built.diagnostics.empty());
    REQUIRE(built.program.declarations.size() == 1);
    CHECK(built.program.declarations[0].name == "hello");
    CHECK(built.program.declarations[0].kind == cppfrontier::declaration_kind::function);
    CHECK(built.program.declarations[0].body.source() == "\"hello\"");
}

TEST_CASE("main declarations are marked as entry points in IR", "[parser][ir]") {
    const auto source = cppfrontier::source_text::from_bytes("main: () = { }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, source.lines().size() + 1);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    const auto built = cppfrontier::build_ir(parsed.tree);

    REQUIRE(built.diagnostics.empty());
    REQUIRE(built.program.declarations.size() == 1);
    CHECK(built.program.declarations[0].is_entry_point);
}

TEST_CASE("large declarations and bodies have no size limit", "[parser][limits]") {
    auto members = std::ostringstream{};
    members << "t: type = { ";
    for (auto index = 0; index < 600; ++index) members << "x" << index << ": int = 1; ";
    members << "}";
    const auto parsed_members = parse_source(members.str());
    CHECK(parsed_members.diagnostics.empty());
    REQUIRE(parsed_members.tree.declarations.size() == 1);
    CHECK(parsed_members.tree.declarations[0]->members.size() == 600);

    auto nested = std::string{"f: () = { x := 1; "};
    for (auto index = 0; index < 100; ++index) nested += "if true { ";
    nested += "x = 2; ";
    for (auto index = 0; index < 100; ++index) nested += "} ";
    CHECK(parse_source(nested + "}").diagnostics.empty());
}

TEST_CASE("nesting deeper than the limit is diagnosed before parsing", "[parser][limits]") {
    const auto deep = [](std::string_view open, std::string_view close, int levels) {
        auto text = std::string{"f: () = { x := "};
        for (auto index = 0; index < levels; ++index) text += open;
        text += "1";
        for (auto index = 0; index < levels; ++index) text += close;
        return text + "; }";
    };
    CHECK_FALSE(has_diagnostic(parse_source(deep("(", ")", 200)), "nesting-too-deep"));
    CHECK(has_diagnostic(parse_source(deep("(", ")", 2000)), "nesting-too-deep"));
    CHECK(has_diagnostic(parse_source(deep("std::vector<", ">", 3000)), "nesting-too-deep"));

    auto loops = std::string{"f: (r: std::vector<int>) = { "};
    for (auto index = 0; index < 300; ++index) loops += "for r do (x) ";
    CHECK(has_diagnostic(parse_source(loops + "g(x); }"), "statement-depth-limit-exceeded"));
}

TEST_CASE("function bodies preserve loop control statements", "[parser][statements]") {
    const auto parsed = parse_source(R"cpp2(
        count: () -> int = {
            i := 0;
            while i < 4 next i++ {
                if i == 1 {
                    break;
                }
            }
            return i;
        }
    )cpp2");
    CHECK(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& function = *parsed.tree.declarations[0];
    REQUIRE(function.block_body);
    REQUIRE(function.block_body->statements.size() == 3);

    const auto& loop = *function.block_body->statements[1];
    CHECK(loop.kind == cppfrontier::statement_kind::while_statement);
    CHECK(label_of(loop).empty());
    CHECK(condition_of(loop).source() == "i < 4");
    CHECK(loop_of(loop).next.source() == "i++");
    REQUIRE(body_of(loop).size() == 1);
    CHECK(body_of(loop)[0]->kind == cppfrontier::statement_kind::if_statement);

    const auto& conditional = *body_of(loop)[0];
    CHECK(condition_of(conditional).source() == "i == 1");
    REQUIRE(body_of(conditional).size() == 1);
    CHECK(body_of(conditional)[0]->kind == cppfrontier::statement_kind::break_statement);
}

TEST_CASE("postfix expression statements are accepted", "[parser][statements][expressions]") {
    const auto parsed = parse_source(R"cpp2(
        exercise: (object: std::vector<int>, ptr: *int) = {
            object.size();
            object.push_back(1);
            ptr*.method();
            (*ptr).method();
            factory()(1);
            callbacks[0](2);
            x = y;
        }
    )cpp2");
    for (const auto& diagnostic : parsed.diagnostics) INFO(diagnostic.code << ": " << diagnostic.message);
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& body = parsed.tree.declarations[0]->block_body;
    REQUIRE(body);
    REQUIRE(body->statements.size() == 7);
    CHECK(text_of(*body->statements[0]).source() == "object.size();");
    CHECK(text_of(*body->statements[1]).source() == "object.push_back(1);");
    CHECK(text_of(*body->statements[2]).source() == "ptr*.method();");
    CHECK(text_of(*body->statements[3]).source() == "(*ptr).method();");
    CHECK(text_of(*body->statements[4]).source() == "factory()(1);");
    CHECK(text_of(*body->statements[5]).source() == "callbacks[0](2);");
    CHECK(text_of(*body->statements[6]).source() == "x = y;");
}

TEST_CASE("adjacent operands are rejected as malformed statements",
          "[parser][statements][diagnostics]") {
    const auto parsed = parse_source(
        "f: () = { value other; first.second third; }\n");
    CHECK(has_diagnostic(parsed, "invalid-statement-expression"));
}

TEST_CASE("else-if and else blocks retain the branch structure", "[parser][statements]") {
    const auto parsed = parse_source(R"cpp2(
        classify: (value: int) = {
            if value < 0 {
                std::cout << "negative\n";
            }
            else if value == 0 {
                std::cout << "zero\n";
            }
            else {
                std::cout << "positive\n";
            }
        }
    )cpp2");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& statement = *parsed.tree.declarations[0]->block_body->statements[0];
    CHECK(condition_of(statement).source() == "value < 0");
    REQUIRE(if_of(statement).else_body.size() == 1);
    CHECK(condition_of(*if_of(statement).else_body[0]).source() == "value == 0");
    CHECK(if_of(*if_of(statement).else_body[0]).is_else_if);
    REQUIRE(if_of(*if_of(statement).else_body[0]).else_body.size() == 1);
    CHECK(if_of(*if_of(statement).else_body[0]).has_else);
    CHECK_FALSE(if_of(*if_of(statement).else_body[0]).else_body[0]->is<cppfrontier::statement::if_form>());
    CHECK(text_of(*if_of(*if_of(statement).else_body[0]).else_body[0]).source() == "std::cout << \"positive\\n\";");
}

TEST_CASE("plain else bodies retain their statements", "[parser][statements]") {
    const auto parsed = parse_source(R"cpp2(
        f: () = {
            value: int = 0;
            if true {
                value = 1;
            }
            else {
                value = 2;
            }
        }
    )cpp2");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& statement = *parsed.tree.declarations[0]->block_body->statements[1];
    CHECK(if_of(statement).has_else);
    REQUIRE(if_of(statement).else_body.size() == 1);
    CHECK(text_of(*if_of(statement).else_body[0]).source() == "value = 2;");
}

TEST_CASE("a second else after a one-statement else is rejected", "[parser][statements]") {
    const auto parsed = parse_source("f: () = { if true { } else { g(); } else { } }\n");
    CHECK(has_diagnostic(parsed, "else-without-if"));
}

TEST_CASE("an empty else body is represented as an empty branch", "[parser][statements]") {
    const auto parsed = parse_source(R"cpp2(
        f: () = {
            if true { }
            else { }
        }
    )cpp2");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& statement = *parsed.tree.declarations[0]->block_body->statements[0];
    CHECK(if_of(statement).has_else);
    CHECK(if_of(statement).else_body.empty());
}

TEST_CASE("parameter and local declarations retain qualifiers and types", "[parser][unit]") {
    const auto parsed = parse_source(R"cpp2(
name: () -> std::string = {
    s: std::string = "world";
    return s;
}

decorate: (inout s: std::string) = {
    s = "[" + s + "]";
}
    )cpp2");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 2);

    const auto& name = *parsed.tree.declarations[0];
    CHECK(name.kind == cppfrontier::declaration::node_kind::function);
    CHECK(name.type.text == "std::string");
    REQUIRE(name.block_body);
    REQUIRE(name.block_body->statements.size() == 2);
    CHECK(name.block_body->statements[0]->kind == cppfrontier::statement_kind::declaration_statement);
    REQUIRE(declaration_of(*name.block_body->statements[0]));
    CHECK(declaration_of(*name.block_body->statements[0])->name == "s");
    CHECK(declaration_of(*name.block_body->statements[0])->type.text == "std::string");
    CHECK(declaration_of(*name.block_body->statements[0])->initializer.source() == "\"world\"");
    CHECK(text_of(*name.block_body->statements[0]).source() == "s = \"world\";");
    CHECK(name.block_body->statements[1]->kind == cppfrontier::statement_kind::return_statement);

    const auto& decorate = *parsed.tree.declarations[1];
    CHECK(decorate.kind == cppfrontier::declaration::node_kind::function);
    REQUIRE(decorate.parameters.size() == 1);
    CHECK(decorate.parameters[0].passing == cppfrontier::passing_style::inout);
    CHECK(decorate.parameters[0].type.text == "std::string");
    CHECK(decorate.parameters[0].name == "s");
}

TEST_CASE("function-local static declarations retain their storage class", "[parser][locals][static]") {
    const auto parsed = parse_source(R"cpp2(
next_value: () -> int = {
    static typed: int = 0;
    static deduced := 0;
    ordinary := 0;
    return typed + deduced + ordinary;
}
    )cpp2");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& body = parsed.tree.declarations[0]->block_body;
    REQUIRE(body);
    REQUIRE(body->statements.size() == 4);
    REQUIRE(declaration_of(*body->statements[0]));
    REQUIRE(declaration_of(*body->statements[1]));
    REQUIRE(declaration_of(*body->statements[2]));
    CHECK(declaration_of(*body->statements[0])->name == "typed");
    CHECK(declaration_of(*body->statements[0])->type.text == "int");
    CHECK(declaration_of(*body->statements[0])->is_static);
    CHECK(declaration_of(*body->statements[1])->name == "deduced");
    CHECK(declaration_of(*body->statements[1])->is_static);
    CHECK_FALSE(declaration_of(*body->statements[2])->is_static);
}

TEST_CASE("operator= is recognized as a member declaration name",
          "[parser][members][operator]") {
    const auto parsed = parse_source(
        "Value: type = { operator=: (out this, that) = { } }\n");
    for (const auto& diagnostic : parsed.diagnostics) INFO(diagnostic.message);
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    REQUIRE(parsed.tree.declarations[0]->members.size() == 1);
    CHECK(parsed.tree.declarations[0]->members[0]->name == "operator=");
}

TEST_CASE("static is rejected on local aliases", "[parser][locals][static][diagnostics]") {
    const auto parsed = parse_source(
        "f: () = { static Value: int == 42; return Value; }\n");
    CHECK(has_diagnostic(parsed, "static-alias"));
}

TEST_CASE("declaration spans cover their own closing brace", "[parser][spans]") {
    const auto parsed = parse_source(R"cpp2(
main: () -> int = {
    return name();
}

name: () -> int = {
    return 1;
}
    )cpp2");
    REQUIRE(parsed.tree.declarations.size() == 2);
    CHECK(parsed.tree.declarations[0]->span.begin.line == 2);
    CHECK(parsed.tree.declarations[0]->span.end.line == 4);
    CHECK(parsed.tree.declarations[1]->span.begin.line == 6);
    CHECK(parsed.tree.declarations[1]->span.end.line == 8);
}

TEST_CASE("declaration spans cover every closing brace in a multi-declaration file", "[parser][spans]") {
    const auto parsed = parse_source(
        "first: () = { }\nsecond: () = { }\nthird: () = { }");
    REQUIRE(parsed.diagnostics.empty());
    for (const auto& value : parsed.tree.declarations) {
        INFO("declaration: " << value->name << " using=" << value->using_name <<
             " kind=" << static_cast<int>(value->kind));
    }
    REQUIRE(parsed.tree.declarations.size() == 3);
    CHECK(parsed.tree.declarations[0]->span.end.line == 1);
    CHECK(parsed.tree.declarations[1]->span.end.line == 2);
    CHECK(parsed.tree.declarations[2]->span.end.line == 3);
}

TEST_CASE("parameters retain passing style, type, and name in one record", "[parser][parameters]") {
    const auto parsed = parse_source(
        "update: (inout text: std::string, count: int, out value: int) -> bool = { return true; }\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& parameters = parsed.tree.declarations[0]->parameters;
    REQUIRE(parameters.size() == 3);
    CHECK(parameters[0].passing == cppfrontier::passing_style::inout);
    CHECK(parameters[0].type.text == "std::string");
    CHECK(parameters[0].name == "text");
    CHECK(parameters[1].passing == cppfrontier::passing_style::in);
    CHECK(parameters[1].type.text == "int");
    CHECK(parameters[1].name == "count");
    CHECK(parameters[2].passing == cppfrontier::passing_style::out);
    CHECK(parameters[2].type.text == "int");
    CHECK(parameters[2].name == "value");
}

TEST_CASE("omitted parameter types remain wildcards in the AST", "[parser][parameters]") {
    const auto parsed = parse_source("main: (args) = args.ssize();\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    REQUIRE(parsed.tree.declarations[0]->parameters.size() == 1);
    CHECK(parsed.tree.declarations[0]->parameters[0].type.text.empty());
}

TEST_CASE("explicit auto is diagnosed where Cpp2 has a deduction spelling",
          "[parser][types][diagnostics]") {
    const auto variable = parse_source("value: auto = 0;\n");
    CHECK(has_diagnostic(variable, "cpp1-auto-type"));
    REQUIRE(variable.tree.declarations.size() == 1);
    CHECK(variable.tree.declarations[0]->type.text == "auto");

    const auto return_value = parse_source("value: () -> auto = 0;\n");
    CHECK(has_diagnostic(return_value, "cpp1-auto-type"));

    const auto wildcard = parse_source("value: _ = 0;\n");
    CHECK_FALSE(has_diagnostic(wildcard, "cpp1-auto-type"));

    const auto member_access = parse_source(
        "factory: () -> decltype(auto) = { return value; }\n");
    CHECK_FALSE(has_diagnostic(member_access, "cpp1-auto-type"));
}

TEST_CASE("reference-forced parameter kinds retain their identity", "[parser][parameters]") {
    const auto parsed = parse_source(
        "minimum: (in_ref left: int, in_ref right: int) -> forward_ref _ = left;\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& declaration = *parsed.tree.declarations[0];
    REQUIRE(declaration.parameters.size() == 2);
    CHECK(declaration.parameters[0].passing == cppfrontier::passing_style::in_ref);
    CHECK(declaration.parameters[0].type.text == "int");
    CHECK(declaration.parameters[1].passing == cppfrontier::passing_style::in_ref);
    CHECK(declaration.deduction == cppfrontier::return_deduction::by_forwarding_ref);
}

TEST_CASE("forward_ref is rejected in parameter position", "[parser][parameters][diagnostics]") {
    const auto parsed = parse_source("f: (forward_ref value: int) = value;\n");
    CHECK(has_diagnostic(parsed, "invalid-forward-ref-parameter"));
}

TEST_CASE("parameter lists accept one trailing comma", "[parser][parameters][lists]") {
    const auto one = parse_source("f: (x: int,) = x;\n");
    REQUIRE(one.diagnostics.empty());
    REQUIRE(one.tree.declarations.size() == 1);
    REQUIRE(one.tree.declarations[0]->parameters.size() == 1);
    CHECK(one.tree.declarations[0]->parameters[0].name == "x");

    const auto two = parse_source("f: (x: int, y: int,) = x;\n");
    REQUIRE(two.diagnostics.empty());
    REQUIRE(two.tree.declarations.size() == 1);
    REQUIRE(two.tree.declarations[0]->parameters.size() == 2);
    CHECK(two.tree.declarations[0]->parameters[1].name == "y");

    const auto empty = parse_source("f: () = { }\n");
    REQUIRE(empty.diagnostics.empty());
    REQUIRE(empty.tree.declarations.size() == 1);
    CHECK(empty.tree.declarations[0]->parameters.empty());
}

TEST_CASE("parameter lists reject leading and repeated commas", "[parser][parameters][lists][diagnostics]") {
    const auto leading = parse_source("f: (, x: int) = x;\n");
    CHECK(has_diagnostic(leading, "invalid-parameter-declaration"));

    const auto repeated = parse_source("f: (x: int,, y: int) = x;\n");
    CHECK(has_diagnostic(repeated, "invalid-parameter-declaration"));
}

TEST_CASE("typed forward returns retain the named type", "[parser][returns]") {
    const auto parsed = parse_source(
        "get: (inout value: int) -> forward int = { return value; }\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    CHECK(parsed.tree.declarations[0]->type.text == "int");
    CHECK(parsed.tree.declarations[0]->deduction ==
          cppfrontier::return_deduction::by_forwarding_typed);
}

TEST_CASE("template commas do not split parameters", "[parser][parameters][templates]") {
    const auto parsed = parse_source(
        "f: (value: std::variant<X<0>, X<1>>, count: int) = value;\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    REQUIRE(parsed.tree.declarations[0]->parameters.size() == 2);
    CHECK(parsed.tree.declarations[0]->parameters[0].name == "value");
    CHECK(parsed.tree.declarations[0]->parameters[0].type.text == "std::variant<X<0>, X<1>>");
    CHECK(parsed.tree.declarations[0]->parameters[1].name == "count");
    CHECK(parsed.tree.declarations[0]->parameters[1].type.text == "int");
}

TEST_CASE("brace-initialized expressions remain in one statement", "[parser][statements][braces]") {
    const auto parsed = parse_source(
        "main: () -> int = { std::cout << std::string{\"x\"}; return 0; }\n");

    CHECK(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& body = parsed.tree.declarations[0]->block_body;
    REQUIRE(body);
    REQUIRE(body->statements.size() == 2);
    CHECK(body->statements[0]->kind == cppfrontier::statement_kind::expression);
    CHECK(text_of(*body->statements[0]).source().find("std::string{\"x\"}") != std::string::npos);
    CHECK(body->statements[1]->kind == cppfrontier::statement_kind::return_statement);
}

TEST_CASE("local declarations retain merged template closers", "[parser][statements][templates]") {
    const auto parsed = parse_source(
        "main: () = { values: std::vector<std::vector<int>> = (); }\n");

    CHECK(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& body = parsed.tree.declarations[0]->block_body;
    REQUIRE(body);
    REQUIRE(body->statements.size() == 1);
    const auto& declaration = declaration_of(*body->statements[0]);
    REQUIRE(declaration);
    CHECK(declaration->type.text == "std::vector<std::vector<int>>");
    CHECK(declaration->name == "values");
}

TEST_CASE("function type parameters keep commas inside the type", "[parser][parameters][types]") {
    const auto parsed = parse_source(
        "signal: (handler: *(_: int, value: int) -> void) -> void = { }\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    REQUIRE(parsed.tree.declarations[0]->parameters.size() == 1);
    CHECK(parsed.tree.declarations[0]->parameters[0].name == "handler");
    CHECK(parsed.tree.declarations[0]->parameters[0].type.text ==
          "*(_: int, value: int) -> void");
}

TEST_CASE("pointer and function-type locals retain their declared types",
          "[parser][locals][types]") {
    const auto parsed = parse_source(
        "main: () -> int = {\n"
        "    p: *int;\n"
        "    pf: * (i: i32) -> std::string = decorate_int&;\n"
        "    return 0;\n"
        "}\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& body = parsed.tree.declarations[0]->block_body;
    REQUIRE(body);
    REQUIRE(body->statements.size() == 3);
    REQUIRE(declaration_of(*body->statements[0]));
    CHECK(declaration_of(*body->statements[0])->name == "p");
    CHECK(declaration_of(*body->statements[0])->type.text == "*int");
    REQUIRE(declaration_of(*body->statements[1]));
    CHECK(declaration_of(*body->statements[1])->name == "pf");
    CHECK(declaration_of(*body->statements[1])->type.text == "* (i: i32) -> std::string");
}

TEST_CASE("local type boundaries exclude initializer parentheses", "[parser][locals][types]") {
    const auto valid = parse_source(
        "main: () -> int = {\n"
        "    t: std::string = (\"x\");\n"
        "    return 0;\n"
        "}\n");
    REQUIRE(valid.tree.declarations.size() == 1);
    const auto& body = valid.tree.declarations[0]->block_body;
    REQUIRE(body);
    REQUIRE(body->statements.size() == 2);
    REQUIRE(declaration_of(*body->statements[0]));
    CHECK(declaration_of(*body->statements[0])->type.text == "std::string");
    CHECK(declaration_of(*body->statements[0])->initializer.source() == "(\"x\")");

    const auto invalid = parse_source(
        "main: () -> int = { s: std::string(\"x\"); return 0; }\n");
    CHECK(has_diagnostic(invalid, "invalid-declaration-initializer"));
}

TEST_CASE("local declarations preserve parenthesized initializers and reject braced ones",
          "[parser][statements][initializers][review]") {
    const auto parsed = parse_source(
        "main: () = {\n"
        "    values: std::vector<int> = (1, 2);\n"
        "}\n");

    CHECK(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& body = parsed.tree.declarations[0]->block_body;
    REQUIRE(body);
    REQUIRE(body->statements.size() == 1);
    REQUIRE(declaration_of(*body->statements[0]));
    CHECK(declaration_of(*body->statements[0])->initializer.source() == "(1, 2)");

    const auto braced = parse_source("main: () = { braces: std::vector<int> = {3, 4}; }\n");
    CHECK(has_diagnostic(braced, "braced-list"));
}

TEST_CASE("malformed statement starts are diagnosed", "[parser][statements][diagnostics]") {
    const auto garbage = parse_source("f: () = { @@@ }\n");
    CHECK(has_diagnostic(garbage, "invalid-statement-start"));

    const auto question_marks = parse_source("f: () = { ???; }\n");
    CHECK(has_diagnostic(question_marks, "invalid-statement-start"));

    const auto stray_parenthesis = parse_source("f: () = { ) }\n");
    CHECK(has_diagnostic(stray_parenthesis, "invalid-statement-start"));
}

TEST_CASE("unmatched statement delimiters are diagnosed", "[parser][statements][diagnostics]") {
    const auto close_parenthesis = parse_source(
        "main: () -> int = { std::cout << \"hello\"); return 0; }\n");
    CHECK(has_diagnostic(close_parenthesis, "unmatched-close-parenthesis"));

    const auto open_parenthesis = parse_source("main: () = { call(1; }\n");
    CHECK(has_diagnostic(open_parenthesis, "unmatched-open-parenthesis"));
}

TEST_CASE("declaration missing an initializer value is diagnosed", "[parser][statements][diagnostics]") {
    const auto parsed = parse_source("main: () -> int = { value: int = ; return 0; }\n");
    CHECK(has_diagnostic(parsed, "missing-initializer"));
}

TEST_CASE("using declarations preserve their qualified target", "[parser][using][namespaces]") {
    const auto parsed = parse_source(
        "using ns::value;\n"
        "using ns::_;\n"
        "main: () = { using ::std::string; using local::_ ; }\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 3);
    CHECK(parsed.tree.declarations[0]->kind == cppfrontier::declaration_kind::using_declaration);
    CHECK(parsed.tree.declarations[0]->using_name == "ns::value");
    CHECK(parsed.tree.declarations[1]->kind == cppfrontier::declaration_kind::using_declaration);
    CHECK(parsed.tree.declarations[1]->using_name == "ns::_");

    const auto& body = parsed.tree.declarations[2]->block_body;
    REQUIRE(body);
    REQUIRE(body->statements.size() == 2);
    CHECK(body->statements[0]->kind == cppfrontier::statement_kind::using_declaration);
    CHECK(using_name_of(*body->statements[0]) == "::std::string");
    CHECK(body->statements[1]->kind == cppfrontier::statement_kind::using_declaration);
    CHECK(using_name_of(*body->statements[1]) == "local::_");
}

TEST_CASE("Cpp1 using namespace syntax is diagnosed as invalid Cpp2", "[parser][using][diagnostics]") {
    const auto parsed = parse_source(
        "main: () = {\n"
        "    using namespace std;\n"
        "}\n");
    CHECK(has_diagnostic(parsed, "invalid-using-declaration"));
}

TEST_CASE("template parameters preserve kinds defaults and constraints", "[parser][templates]") {
    const auto parsed = parse_source(R"cpp2(
f: <T, U: type, V: _, N: i32 = 42, Ts...: type> (t: T) requires std::regular<T> = t;
g: <T: type> type requires std::is_integral_v<T> = { value: T = (); }
)cpp2");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 2);

    const auto& function = *parsed.tree.declarations[0];
    REQUIRE(function.template_parameters.size() == 5);
    CHECK(function.template_parameters[0].name == "T");
    CHECK(function.template_parameters[0].is_type_parameter());
    CHECK(function.template_parameters[1].is_type_parameter());
    CHECK(function.template_parameters[2].type.text == "_");
    CHECK(function.template_parameters[3].type.text == "i32");
    CHECK(function.template_parameters[3].default_value.source() == "42");
    CHECK(function.template_parameters[4].is_variadic);
    CHECK(function.requires_clause.source() == "std::regular<T>");

    const auto& type = *parsed.tree.declarations[1];
    CHECK(type.kind == cppfrontier::declaration_kind::type);
    CHECK(type.requires_clause.source() == "std::is_integral_v<T>");
}

TEST_CASE("template angle brackets handle merged closers", "[parser][templates][diagnostics]") {
    const auto parsed = parse_source(
        "f: <T, U> (copy xs: std::vector<std::vector<T>>) = xs;\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    REQUIRE(parsed.tree.declarations[0]->parameters.size() == 1);
    CHECK(parsed.tree.declarations[0]->parameters[0].type.text == "std::vector<std::vector<T>>");

    const auto malformed = parse_source("f: <T, U () = 0;\n");
    CHECK(has_diagnostic(malformed, "unterminated-template-parameters"));
}

TEST_CASE("concept declarations retain their template and body", "[parser][concepts]") {
    const auto parsed = parse_source(
        "arithmetic: <T> concept = std::integral<T> || std::floating_point<T>;\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& concept_decl = *parsed.tree.declarations[0];
    CHECK(concept_decl.kind == cppfrontier::declaration_kind::concept_declaration);
    REQUIRE(concept_decl.template_parameters.size() == 1);
    CHECK(concept_decl.template_parameters[0].name == "T");
    REQUIRE(concept_decl.body);
    CHECK(concept_decl.body->text.source() == "std::integral<T> || std::floating_point<T>");
}

TEST_CASE("concept is reserved outside the definition slot", "[parser][concepts][names]") {
    const auto escaped_concept = parse_source(
        "__identifier__concept: () -> int = 1;\n");
    CHECK_FALSE(escaped_concept.diagnostics.empty());
    CHECK(escaped_concept.tree.declarations.empty());

    const auto contextual_concept = parse_source(
        "concept: () -> int = 1;\n");
    CHECK_FALSE(contextual_concept.diagnostics.empty());
    CHECK(contextual_concept.tree.declarations.empty());

    const auto definition = parse_source(
        "arithmetic: <T> concept = std::integral<T>;\n");
    REQUIRE(definition.diagnostics.empty());
    REQUIRE(definition.tree.declarations.size() == 1);
    CHECK(definition.tree.declarations[0]->kind ==
          cppfrontier::declaration_kind::concept_declaration);
}

TEST_CASE("merged template closers preserve nested parameter kinds",
          "[parser][templates][closers]") {
    const auto parsed = parse_source(
        "f: <T: std::vector<int>> () = 0;\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    REQUIRE(parsed.tree.declarations[0]->template_parameters.size() == 1);
    CHECK(parsed.tree.declarations[0]->template_parameters[0].type.text ==
          "std::vector<int>");
}

TEST_CASE("local type aliases are distinct from local objects", "[parser][aliases][locals]") {
    const auto parsed = parse_source(
        "main: () = {\n"
        "    t1: type == bool;\n"
        "}\n");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& statements = parsed.tree.declarations[0]->block_body->statements;
    REQUIRE(statements.size() == 1);
    REQUIRE(declaration_of(*statements[0]));
    CHECK(declaration_of(*statements[0])->kind == cppfrontier::declaration_kind::type);
    CHECK(declaration_of(*statements[0])->is_alias);
    CHECK(declaration_of(*statements[0])->alias == "bool");
}

TEST_CASE("local template aliases are left to the Cpp1 compiler",
          "[parser][aliases][templates]") {
    const auto parsed = parse_source(
        "main: () = {\n"
        "    box: <T, N: i32 = 2> type == std::array<T, N>;\n"
        "}\n");
    CHECK(parsed.diagnostics.empty());
}

TEST_CASE("range-for parameters retain passing style and separate next clause",
          "[parser][statements][parameters]") {
    const auto parsed = parse_source(R"cpp2(
f: () = {
    for values do (item) { _ = item; }
    for values do (inout item) { item += 1; }
    for values do (copy item: int) { _ = item; }
    for values next count++ do (const_item) { _ = const_item; }
}
)cpp2");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& statements = parsed.tree.declarations[0]->block_body->statements;
    REQUIRE(statements.size() == 4);

    REQUIRE(loop_of(*statements[0]).range_parameter);
    CHECK(loop_of(*statements[0]).range_parameter->passing == cppfrontier::passing_style::in);
    CHECK(loop_of(*statements[0]).range_parameter->name == "item");
    CHECK(loop_of(*statements[0]).next.empty());

    REQUIRE(loop_of(*statements[1]).range_parameter);
    CHECK(loop_of(*statements[1]).range_parameter->passing == cppfrontier::passing_style::inout);
    CHECK(loop_of(*statements[1]).range_parameter->name == "item");

    REQUIRE(loop_of(*statements[2]).range_parameter);
    CHECK(loop_of(*statements[2]).range_parameter->passing == cppfrontier::passing_style::copy);
    CHECK(loop_of(*statements[2]).range_parameter->type.text == "int");

    REQUIRE(loop_of(*statements[3]).range_parameter);
    CHECK(loop_of(*statements[3]).range_parameter->name == "const_item");
    CHECK(loop_of(*statements[3]).next.source() == "count++");
}

TEST_CASE("unbraced range-for bodies parse one complete statement",
          "[parser][statements][loops][upstream-1426]") {
    const auto parsed = parse_source(R"cpp2(
f: () = {
    for values do (item)
        if item != 0 {
            _ = item;
        }
        else if item < 0 {
            _ = 0;
        }
    for values next count++ do (item) std::cout << item;
}
)cpp2");
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& statements = parsed.tree.declarations[0]->block_body->statements;
    REQUIRE(statements.size() == 2);
    REQUIRE(body_of(*statements[0]).size() == 1);
    CHECK(body_of(*statements[0])[0]->kind == cppfrontier::statement_kind::if_statement);
    REQUIRE(if_of(*body_of(*statements[0])[0]).else_body.size() == 1);
    CHECK(condition_of(*if_of(*body_of(*statements[0])[0]).else_body[0]).source() == "item < 0");
    REQUIRE(body_of(*statements[1]).size() == 1);
    CHECK(body_of(*statements[1])[0]->kind == cppfrontier::statement_kind::expression);
    CHECK(text_of(*body_of(*statements[1])[0]).source() == "std::cout << item;");
}

TEST_CASE("try statements retain typed and catch-all handlers", "[parser][statements][exceptions]") {
    const auto parsed = parse_source(R"cpp2(
f: () = {
    try {
        throw(42);
    } catch (i: int) {
        _ = i;
    } catch (_) {
        return;
    }
}
)cpp2");
    for (const auto& diagnostic : parsed.diagnostics) INFO(diagnostic.code << ": " << diagnostic.message);
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& statements = parsed.tree.declarations[0]->block_body->statements;
    REQUIRE(statements.size() == 1);
    CHECK(statements[0]->kind == cppfrontier::statement_kind::try_statement);
    REQUIRE(handlers_of(*statements[0]).size() == 2);
    CHECK(handlers_of(*statements[0])[0].caught.type.text == "int");
    CHECK(handlers_of(*statements[0])[0].caught.name == "i");
    CHECK(handlers_of(*statements[0])[1].caught.name == "_");
    CHECK(handlers_of(*statements[0])[1].caught.type.text.empty());
}
