#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include "../../src/scope_model.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

auto with_destructuring() -> cppfrontier::translation_options {
    auto options = cppfrontier::translation_options{};
    REQUIRE(options.experiments.enable("destructuring"));
    return options;
}

auto translate(const std::string& source) -> cppfrontier::translation_result {
    return cppfrontier::translator{}.translate_source(source, with_destructuring());
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line, std::uint32_t column, std::string_view message) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const cppfrontier::diagnostic& value) {
            return value.code == code && value.span.begin.line == line &&
                value.span.begin.column == column && value.message == message;
        });
}

// Compiles the translation with g++ and returns the program's output.
auto run(const cppfrontier::translation_result& translation, std::string_view name)
    -> std::string {
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-destructuring");
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

TEST_CASE("destructuring declarations lower to structured bindings",
          "[experiments][destructuring]") {
    const auto result = translate(
        "make: () -> std::tuple<int, int, int> = std::tuple(1, 2, 3);\n"
        "main: () = {\n"
        "    (a, b) := std::pair(1, 2.5);\n"
        "    (x, _, z) := make();\n"
        "    std::cout << a << b << x << z;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("    auto [a, b] = std::pair(1, 2.5);\n") != std::string::npos);
    CHECK(result.cpp1.find("    [[maybe_unused]] auto [x, __cpp2_unused_4_9, z] = make();\n") !=
          std::string::npos);
    CHECK(result.cpp1.find("__cpp2_unused_4_9") == result.cpp1.rfind("__cpp2_unused_4_9"));
}

TEST_CASE("destructured names are locals in the IR and scope model",
          "[experiments][destructuring]") {
    const auto result = translate(
        "main: () = {\n"
        "    (a, _) := std::pair(1, 2);\n"
        "    std::cout << a;\n"
        "}\n");
    REQUIRE_FALSE(result.has_errors());
    const auto& program = result.program;
    const auto symbol = std::find_if(program.symbols.begin(), program.symbols.end(),
        [](const cppfrontier::symbol& value) { return value.name == "a"; });
    REQUIRE(symbol != program.symbols.end());
    CHECK(symbol->kind == cppfrontier::declaration_kind::object);
    CHECK(symbol->span.begin.line == 2);
    CHECK(symbol->span.begin.column == 6);
    CHECK(std::none_of(program.symbols.begin(), program.symbols.end(),
        [](const cppfrontier::symbol& value) { return value.name == "_"; }));
    // The use on line 3 refers to the binding; the binding itself is not a reference.
    const auto references = std::count_if(program.references.begin(), program.references.end(),
        [&](const cppfrontier::symbol_reference& value) { return value.symbol == symbol->id; });
    CHECK(references == 1);
    CHECK(std::any_of(program.references.begin(), program.references.end(),
        [&](const cppfrontier::symbol_reference& value) {
            return value.symbol == symbol->id && value.span.begin.line == 3;
        }));

    const auto scopes = cppfrontier::scope_model{program};
    const auto& statements = program.declarations.front().function_statements->statements;
    REQUIRE(statements.size() == 2);
    const auto& form = statements.front()->as<cppfrontier::statement::destructuring_form>();
    REQUIRE(form.bindings.size() == 2);
    const auto* entry = scopes.declared_by(form.bindings.front());
    REQUIRE(entry != nullptr);
    CHECK(entry->symbol == symbol->id);
    CHECK(scopes.declared_by(form.bindings.back()) == nullptr);
    CHECK(scopes.lookup("a", scopes.at(*statements.back())) == entry);
}

TEST_CASE("destructured names are forwarded at their definite last use and are initialized",
          "[experiments][destructuring][runtime]") {
    const auto result = translate(
        "#include <iostream>\n"
        "#include <string>\n"
        "f: (copy s: std::string) = { std::cout << s << '\\n'; }\n"
        "main: () = {\n"
        "    (a, b) := std::pair(std::string(\"hello\"), std::string(\"world\"));\n"
        "    f(a);\n"
        "    f(b);\n"
        "    total: int;\n"
        "    (m, n) := std::pair(10, 20);\n"
        "    total = m + n;\n"
        "    std::cout << total << '\\n';\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("f(CPP2_FORWARD(a));") != std::string::npos);
    CHECK(result.cpp1.find("f(CPP2_FORWARD(b));") != std::string::npos);
    CHECK(result.cpp1.find("total.construct(CPP2_FORWARD(m) + CPP2_FORWARD(n));") !=
          std::string::npos);
    CHECK(run(result, "last_use") == "hello\nworld\n30\n");
}

TEST_CASE("destructuring a pair, a tuple and an array runs",
          "[experiments][destructuring][runtime]") {
    const auto result = translate(
        "#include <array>\n"
        "#include <iostream>\n"
        "#include <string>\n"
        "#include <tuple>\n"
        "#include <utility>\n"
        "make: () -> std::tuple<int, std::string, double> = std::tuple(1, \"two\", 3.5);\n"
        "main: () = {\n"
        "    (a, b) := std::pair(1, 2.5);\n"
        "    std::cout << a << ' ' << b << '\\n';\n"
        "    (x, _, z) := make();\n"
        "    std::cout << x << ' ' << z << '\\n';\n"
        "    (p, _, r) := std::array{7, 8, 9};\n"
        "    std::cout << p << ' ' << r << '\\n';\n"
        "    source := std::pair(3, 4);\n"
        "    (e, f) := source;\n"
        "    e += 1;\n"
        "    std::cout << e << ' ' << f << ' ' << source.first << '\\n';\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto [e, f] = cpp2::move(source);") == std::string::npos);
    CHECK(result.cpp1.find("auto [e, f] = source;") != std::string::npos);
    CHECK(run(result, "shapes") == "1 2.5\n1 3.5\n7 9\n4 4 3\n");
}

TEST_CASE("destructuring diagnostics", "[experiments][destructuring][diagnostics]") {
    SECTION("outside a function body") {
        const auto result = translate(
            "(a, b) := std::pair(1, 2);\n"
            "n: namespace = {\n"
            "    (c, d) := std::pair(1, 2);\n"
            "}\n"
            "t: type = {\n"
            "    (e, f) := std::pair(1, 2);\n"
            "}\n"
            "main: () = { }\n");
        CHECK(result.diagnostics.size() == 3);
        for (const auto [line, column] : {std::pair{1u, 1u}, std::pair{3u, 5u}, std::pair{6u, 5u}}) {
            CHECK(has_diagnostic(result, "destructuring-scope", line, column,
                                 "a destructuring declaration must be in a function body"));
        }
    }
    SECTION("names") {
        const auto result = translate(
            "main: () = {\n"
            "    (a) := std::pair(1, 2);\n"
            "    (b, b) := std::pair(1, 2);\n"
            "    (_, _) := std::pair(1, 2);\n"
            "}\n");
        CHECK(result.diagnostics.size() == 2);
        CHECK(has_diagnostic(result, "destructuring-arity", 2, 5,
                             "a destructuring declaration needs at least two names"));
        CHECK(has_diagnostic(result, "destructuring-duplicate-name", 3, 9,
                             "'b' is bound twice in a destructuring declaration"));
    }
    SECTION("initializers") {
        const auto result = translate(
            "main: () = {\n"
            "    (a, b): std::pair<int, int> = std::pair(1, 2);\n"
            "    (c, d) := (1, 2);\n"
            "    (e, f) := ;\n"
            "}\n");
        CHECK(result.diagnostics.size() == 3);
        CHECK(has_diagnostic(result, "destructuring-type", 2, 11,
                             "a destructuring declaration deduces its types - write ':='"));
        CHECK(has_diagnostic(result, "destructuring-initializer", 3, 15,
                             "a destructuring initializer is one tuple-like value - write "
                             "std::tuple(...) for a list"));
        CHECK(has_diagnostic(result, "missing-initializer", 4, 12,
                             "expected an initializer after ':='"));
    }
}

TEST_CASE("destructuring syntax is unchanged without the experiment",
          "[experiments][destructuring][review]") {
    // As before the experiment: a Cpp1 line at file scope, and in a function
    // body an expression whose `:` is not part of it.
    const auto source = std::string{
        "(a, b) := std::pair(1, 2);\n"
        "main: () = {\n"
        "    (c, d) := std::pair(1, 2);\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    CHECK(result.has_errors());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "incomplete-expression");
    CHECK(result.diagnostics.front().span.begin.line == 3);

    const auto file_scope = cppfrontier::translator{}.translate_source(
        "(a, b) := std::pair(1, 2);\nmain: () = { }\n");
    REQUIRE_FALSE(file_scope.has_errors());
    CHECK(file_scope.cpp1.find("\n(a, b) := std::pair(1, 2);\n") != std::string::npos);
    CHECK(file_scope.cpp1.find("auto [") == std::string::npos);

    const auto enabled = translate(source);
    CHECK(enabled.has_errors());
    CHECK(has_diagnostic(enabled, "destructuring-scope", 1, 1,
                         "a destructuring declaration must be in a function body"));
}

TEST_CASE("@print shows a destructuring declaration", "[experiments][destructuring][print]") {
    const auto result = translate(
        "t: @print type = {\n"
        "    f: () = {\n"
        "        (a, b) := std::pair(1, 2);\n"
        "        std::cout << a << b;\n"
        "    }\n"
        "}\n");
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.printed.find("        (a, b) := std::pair(1, 2);\n") != std::string::npos);
}
