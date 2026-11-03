#include <catch2/catch_all.hpp>

#include "../support/statement_forms.hpp"
#include <cppfrontier/pipeline.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace cppfrontier::test;

namespace {

struct corpus_file final {
    std::filesystem::path path;
    std::string source;
};

auto load_corpus() -> std::vector<corpus_file> {
    const auto directory = std::filesystem::path{CPPFRONTIER_UPSTREAM_DIR} / "regression-tests";

    auto files = std::vector<corpus_file>{};
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.path().extension() == ".cpp2") {
            files.push_back({entry.path(), test_support::read_file(entry.path())});
        }
    }
    std::sort(files.begin(), files.end(), [](const corpus_file& left, const corpus_file& right) {
        return left.path < right.path;
    });
    return files;
}

struct upstream_case final {
    std::filesystem::path source_path;
    std::string source;
};

auto load_upstream_cases() -> std::vector<upstream_case> {
    const auto source_directory = std::filesystem::path{CPPFRONTIER_UPSTREAM_DIR} / "regression-tests";
    const auto expected_directory = std::filesystem::path{CPPFRONTIER_UPSTREAM_TESTS_DIR};
    auto cases = std::vector<upstream_case>{};
    for (const auto& entry : std::filesystem::directory_iterator{source_directory}) {
        if (entry.path().extension() != ".cpp2") continue;
        const auto expected = expected_directory / (entry.path().stem().string() + ".cpp");
        if (!std::filesystem::exists(expected)) continue;
        cases.push_back({entry.path(), test_support::read_file(entry.path())});
    }
    std::sort(cases.begin(), cases.end(), [](const upstream_case& left, const upstream_case& right) {
        return left.source_path < right.source_path;
    });
    return cases;
}

TEST_CASE("upstream generated Cpp1 is recognized by the rewrite", "[corpus][upstream]") {
    const auto cases = load_upstream_cases();
    REQUIRE(cases.size() >= 150);
    auto emitted = 0;
    auto rejected = 0;
    auto silent = 0;
    for (const auto& test_case : cases) {
        const auto result = cppfrontier::translator{}.translate_source(test_case.source);
        if (result.has_errors()) {
            CHECK(result.cpp1.empty());
            ++rejected;
        }
        else {
            CHECK_FALSE(result.cpp1.empty());
            ++emitted;
        }
        if (result.cpp1.empty() && !result.has_errors()) ++silent;
    }
    INFO("emitted=" << emitted << " rejected=" << rejected << " silent=" << silent);
    CHECK(silent == 0);
}

TEST_CASE("upstream Cpp2 range-for lowers to a Cpp1 range-for", "[corpus][upstream][statements]") {
    const auto source = std::string{
        "f: () = { for words do (inout word) { std::cout << word; } }"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK(result.cpp1.find("for (") != std::string::npos);
    CHECK(result.cpp1.find("for (auto& word : words)") != std::string::npos);
}

TEST_CASE("range operators lower to cpp2::range with negative bounds",
          "[corpus][upstream][statements][range]") {
    const auto source = std::string{
        "f: () = {\n"
        "    for -2 ..< 2 do (value) { std::cout << value; }\n"
        "    for 1 ..= 3 do (value) { std::cout << value; }\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("for (auto const& value : cpp2::range(-2,2))") !=
          std::string::npos);
    CHECK(result.cpp1.find("for (auto const& value : cpp2::range(1,3,true))") !=
          std::string::npos);
}

TEST_CASE("while loop lowers to Cpp1 while", "[corpus][upstream][statements]") {
    const auto source = std::string{
        "f: () = {\n"
        "    i := 0;\n"
        "    while i < 4 next i++ {\n"
        "        std::cout << i;\n"
        "    }\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK(result.cpp1.find("for (; cpp2::impl::cmp_less(i,4); ++i)") != std::string::npos);
}

TEST_CASE("for with next and range parameter lowers to a range-for", "[corpus][upstream][statements]") {
    const auto source = std::string{
        "f: () = { for words do (inout word) { std::cout << word; } }"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    CHECK(result.cpp1.find("for (auto& word : words)") != std::string::npos);
}

TEST_CASE("for does not accept a next clause before its range", "[corpus][upstream][statements]") {
    const auto source_text = cppfrontier::source_text::from_bytes(
        "f: () = { for next std::size(words) do (word) { std::cout << word; } }");
    const auto parsed = cppfrontier::tokenize(
        source_text, 1, static_cast<std::uint32_t>(source_text.lines().size() + 1));
    const auto tree = cppfrontier::parse_cpp2(source_text, parsed);
    CHECK(std::any_of(tree.diagnostics.begin(), tree.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "unexpected-for-next-clause";
        }));
}

TEST_CASE("as expressions lower to the safe Cpp2 cast helper", "[corpus][upstream][expressions]") {
    const auto source = std::string{
        "f: (value: i32) = { std::cout << (value == 0) as std::string; }"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK(result.cpp1.find("cpp2::impl::as_<std::string>(") != std::string::npos);
}

TEST_CASE("unnamed named arguments construct a temporary", "[corpus][upstream][functions]") {
    const auto source = std::string{R"cpp2(
print: (x) = { std::cout << x; }
main: () = {
    print( : std::string = "end of program" );
}
)cpp2"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK(result.cpp1.find("print( std::string{\"end of program\"} )") !=
          std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-unnamed-arg");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "unnamed-arg.cpp";
    test_support::write_file(cpp1_path, result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("new expressions lower to the Cpp2 allocation helper", "[corpus][upstream][expressions]") {
    const auto source = std::string{"f: () = { value := new<int>(0); }"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK(result.cpp1.find("cpp2_new<int>(0)") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-new-expression");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "new-expression.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("indexed callable calls lower to bounds-checked Cpp2 calls", "[corpus][upstream][expressions]") {
    const auto source = std::string{
        "f: (_: i32) = { }\n"
        "main: () = {\n"
        "    array_of_functions: std::array = (f, f);\n"
        "    index := 0;\n"
        "    arguments: i32 = 0;\n"
        "    array_of_functions[index](arguments);\n"
        "    _ = array_of_functions;\n"
        "    _ = index;\n"
        "    _ = arguments;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK(result.cpp1.find("CPP2_ASSERT_IN_BOUNDS(array_of_functions, index)") != std::string::npos);
    CHECK(result.cpp1.find("(arguments)") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-indexed-call");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "indexed-call.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("deduced function parameters pass by const reference", "[corpus][upstream][functions]") {
    const auto source = std::string{"print: (x) = { std::cout << x; }"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK(result.cpp1.find("auto print(auto const& x) -> void") != std::string::npos);
}

TEST_CASE("guard-prefixed locals remain ordinary objects without last-use moves", "[corpus][upstream][last-use]") {
    const auto source = std::string{
        "f: () = {\n"
        "    guard_lock := 1;\n"
        "    guard_scope := 2;\n"
        "    use(guard_lock, guard_scope);\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    CHECK(result.cpp1.find("auto guard_lock {1};") != std::string::npos);
    CHECK(result.cpp1.find("auto guard_scope {2};") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::move") == std::string::npos);
}

TEST_CASE("guard-prefixed declarations and parameters are ordinary names", "[corpus][upstream][names][guard]") {
    const auto source = std::string{
        "guard_type: type = {\n"
        "    value: i32 = 0;\n"
        "    guard_method: (this) -> i32 = value;\n"
        "}\n"
        "\n"
        "guard_function: (\n"
        "    copy guard_copy: std::string,\n"
        "    move guard_move: std::string,\n"
        "    forward guard_forward: std::string\n"
        ") -> std::string = guard_copy;\n"
        "\n"
        "main: () = {\n"
        "    guard_local: std::string = \"x\";\n"
        "    _ = guard_local;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find("struct guard_type") != std::string::npos);
    CHECK(result.cpp1.find("guard_method") != std::string::npos);
    CHECK(result.cpp1.find("guard_function") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::copy") == std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::move") == std::string::npos);
    CHECK(result.cpp1.find("guard_copy") != std::string::npos);
    CHECK(result.cpp1.find("guard_move") != std::string::npos);
    CHECK(result.cpp1.find("guard_forward") != std::string::npos);
    CHECK(result.cpp1.find("std::string guard_local {\"x\"};") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::move") == std::string::npos);
}

TEST_CASE("contextual keyword names emit valid Cpp1 spelling", "[corpus][upstream][names][keywords]") {
    const auto source = std::string{
        "class: () = { }\n"
        "wrap: <class: type> (class: int) -> int = class;\n"
        "Box: type = {\n"
        "    class: int = 1;\n"
        "    get: (this) -> int = class;\n"
        "}\n"
        "main: () -> int = {\n"
        "    class := 2;\n"
        "    _ = class;\n"
        "    return wrap<int>(1) - 1;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto cpp2_class() -> void") != std::string::npos);
    CHECK(result.cpp1.find("template<typename cpp2_class>") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::in<int> cpp2_class") != std::string::npos);
    CHECK(result.cpp1.find("private: int cpp2_class {1};") != std::string::npos);
    CHECK(result.cpp1.find("return cpp2_class;") != std::string::npos);
    CHECK(result.cpp1.find("auto cpp2_class {2};") != std::string::npos);
    CHECK(result.cpp1.find("cpp2_class") != std::string::npos);
}

TEST_CASE("contextual keyword names compile as declarations and uses", "[corpus][upstream][names][keywords][compile]") {
    const auto source = std::string{
        "#include <string>\n"
        "class: (class: int) -> int = class;\n"
        "Box: type = {\n"
        "    class: int = 2;\n"
        "    get: (this) -> int = class;\n"
        "}\n"
        "main: () -> int = {\n"
        "    type: int = 3;\n"
        "    box: Box = ();\n"
        "    return class(type) + box.get() - 5;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-keyword-names");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "keyword-names.cpp";
    test_support::write_file(cpp1_path, result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("escaped identifiers render in declarations and uses", "[corpus][upstream][names][keywords]") {
    const auto source = std::string{
        "__identifier__class: (__identifier__class: int) -> int = __identifier__class;\n"
        "main: () -> int = {\n"
        "    __identifier__class := 4;\n"
        "    return __identifier__class - 4;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto cpp2_class(cpp2::impl::in<int> cpp2_class) -> int") !=
          std::string::npos);
    CHECK(result.cpp1.find("auto cpp2_class {4};") != std::string::npos);
    CHECK(result.cpp1.find("return cpp2_class - 4;") != std::string::npos);
    CHECK(result.cpp1.find("{ return cpp2_class; }") != std::string::npos);
    CHECK(result.cpp1.find("__identifier__") == std::string::npos);
}

TEST_CASE("the identifier escape preserves reserved names", "[corpus][upstream][names][keywords]") {
    const auto source = std::string{
        "__identifier__class: () -> int = 1;\n"
        "__identifier__default: () -> int = 2;\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto cpp2_class() -> int") != std::string::npos);
    CHECK(result.cpp1.find("auto cpp2_default() -> int") != std::string::npos);
}

TEST_CASE("this and that cannot be ordinary typed parameters", "[corpus][upstream][names][keywords]") {
    for (const auto& source : {
             std::string{"f: (this: int) = { }\n"},
             std::string{"f: (that: int) = { }\n"},
         }) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        INFO("source: " << source);
        CHECK(result.has_errors());
        CHECK(result.cpp1.empty());
    }
}

TEST_CASE("passing-style words cannot be bare parameter names", "[corpus][upstream][names][keywords]") {
    for (const auto& word : {"copy", "in", "inout", "out", "move", "forward", "in_ref"}) {
        const auto source = "f: (" + std::string{word} + ": int) = { }\n";
        const auto result = cppfrontier::translator{}.translate_source(source);
        INFO("source: " << source);
        for (const auto& diagnostic : result.diagnostics) {
            INFO(diagnostic.code << ": " << diagnostic.message);
        }
        CHECK(result.has_errors());
        CHECK(result.cpp1.empty());
    }
}

TEST_CASE("Cpp1 reserved words cannot be ordinary parameter names",
          "[corpus][upstream][names][keywords]") {
    for (const auto& word : {"int", "const", "auto", "void"}) {
        const auto source = "f: (" + std::string{word} + ": int) = { }\n";
        const auto result = cppfrontier::translator{}.translate_source(source);
        INFO("source: " << source);
        for (const auto& diagnostic : result.diagnostics) {
            INFO(diagnostic.code << ": " << diagnostic.message);
        }
        CHECK(result.has_errors());
        CHECK(result.cpp1.empty());
    }
}

TEST_CASE("pointer suffix variables are dereferenced in statements", "[corpus][upstream][expressions]") {
    const auto source = std::string{
        "f: () = {\n"
        "    i := new<int>(0);\n"
        "    i*++;\n"
        "    _ = i*;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK(result.cpp1.find("++*cpp2::impl::assert_not_null(i);") != std::string::npos);
    CHECK(result.cpp1.find("*cpp2::impl::assert_not_null(i)") != std::string::npos);
}

TEST_CASE("while loop clauses dereference pointer suffix variables", "[corpus][upstream][statements]") {
    const auto source = std::string{
        "f: () = {\n"
        "    i := new<int>(0);\n"
        "    while i* < 4 next i*++ {\n"
        "        _ = i*;\n"
        "    }\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    CHECK(result.cpp1.find("for (; cpp2::impl::cmp_less(*cpp2::impl::assert_not_null(i),4); ++*cpp2::impl::assert_not_null(i))") !=
          std::string::npos);
}

TEST_CASE("do-loop next clauses dereference pointer suffix variables", "[corpus][upstream][statements]") {
    const auto source = std::string{
        "f: () = {\n"
        "    i := new<int>(0);\n"
        "    do {\n"
        "        _ = i*;\n"
        "    } next i*-- while i* > 0;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    CHECK(result.cpp1.find("--*cpp2::impl::assert_not_null(i)") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::cmp_greater(*cpp2::impl::assert_not_null(i),0)") != std::string::npos);
}

TEST_CASE("local declarations with parentheses retain their initializer", "[parser][statements]") {
    const auto source = cppfrontier::source_text::from_bytes("f: () = { words: std::vector<std::string> = (\"hello\"); }");
    const auto tokens = cppfrontier::tokenize(source, 1, source.lines().size() + 1);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    CHECK(parsed.diagnostics.empty());
    CHECK(parsed.tree.declarations[0]->block_body->statements[0]->kind ==
        cppfrontier::statement_kind::declaration_statement);
}

TEST_CASE("wildcard return types are deferred upstream scope", "[corpus][upstream][wildcards]") {
    const auto source = cppfrontier::source_text::from_bytes(
        test_support::read_file(std::filesystem::path{CPPFRONTIER_UPSTREAM_DIR} /
                  "regression-tests/pure2-repeated-call.cpp2"));
    const auto tokens = cppfrontier::tokenize(
        source, 1, static_cast<std::uint32_t>(source.lines().size() + 1));
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);

    CHECK(parsed.tree.declarations.size() == 6);
    CHECK(std::any_of(parsed.tree.declarations.begin(), parsed.tree.declarations.end(),
        [](const auto& declaration) {
            return declaration->deduction == cppfrontier::return_deduction::by_value;
        }));
}

TEST_CASE("discard statement renders a Cpp1 expression", "[parser][statements]") {
    const auto source = cppfrontier::source_text::from_bytes("f: () = { _ = words; }");
    const auto tokens = cppfrontier::tokenize(
        source, 1, static_cast<std::uint32_t>(source.lines().size() + 1));
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations[0]->block_body->statements.size() == 1);
    const auto& statement = *parsed.tree.declarations[0]->block_body->statements[0];
    CHECK(statement.kind == cppfrontier::statement_kind::discard_statement);
    CHECK(text_of(statement).source() == "words");
}

TEST_CASE("discard assignments lower to static_cast<void>", "[corpus][upstream][statements]") {
    const auto source = std::string{"f: () = { _ = words; }"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    CHECK(result.cpp1.find("_ = words") == std::string::npos);
    CHECK(result.cpp1.find("static_cast<void>(words);") != std::string::npos);
}

TEST_CASE("ssize member call lowers to UFCS", "[corpus][upstream][ufcs]") {
    const auto source = std::string{"f: (s: std::span<int>) = { _ = s.ssize(); }"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    CHECK(result.cpp1.find("CPP2_UFCS(ssize)(s)") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-ssize");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "ssize.cpp";
    test_support::write_file(cpp1_path, result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("CTAD declarations infer a standard container include", "[corpus][upstream][includes]") {
    const auto source = std::string{"f: () = { vi: std::vector = (0, 1, 2); }"};
    const auto result = cppfrontier::translator{}.translate_source(source);

    INFO(result.cpp1);
    CHECK(result.cpp1.find("std::vector vi {0, 1, 2};") != std::string::npos);
    CHECK(result.cpp1.find("#include <vector>") != std::string::npos);
}

TEST_CASE("CTAD declarations infer standard associative container includes", "[corpus][upstream][includes]") {
    const auto source = std::string{
        "main: () = {\n"
        "    m : std::map = (1, 2);\n"
        "    um : std::unordered_map = (1, 2);\n"
        "    s : std::set = (1, 2);\n"
        "    us : std::unordered_set = (1, 2);\n"
        "    _ = m; _ = um; _ = s; _ = us;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    CHECK(result.cpp1.find("#include <map>") != std::string::npos);
    CHECK(result.cpp1.find("#include <unordered_map>") != std::string::npos);
    CHECK(result.cpp1.find("#include <set>") != std::string::npos);
    CHECK(result.cpp1.find("#include <unordered_set>") != std::string::npos);
}

TEST_CASE("loop-control statements retain their target label", "[parser][statements]") {
    const auto source = cppfrontier::source_text::from_bytes(
        R"(f: () = { outer: while i<3 next i++ { if j == 1 { continue outer; break outer; } } })");
    const auto tokens = cppfrontier::tokenize(
        source, 1, static_cast<std::uint32_t>(source.lines().size() + 1));
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);

    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& loop = *parsed.tree.declarations[0]->block_body->statements[0];
    REQUIRE(loop.kind == cppfrontier::statement_kind::while_statement);
    CHECK(label_of(loop) == "outer");
    const auto& branch = *body_of(loop)[0];
    const auto& continue_statement = *body_of(branch)[0];
    const auto& break_statement = *body_of(branch)[1];
    CHECK(continue_statement.kind == cppfrontier::statement_kind::continue_statement);
    CHECK(label_of(continue_statement) == "outer");
    CHECK(break_statement.kind == cppfrontier::statement_kind::break_statement);
    CHECK(label_of(break_statement) == "outer");
}

TEST_CASE("fixed-width aliases expand to standard C++ types", "[types][regression]") {
    const auto source = std::string{
        "main: () -> int = {\n"
        "    foo : u8 = 'a';\n"
        "    bar : i16 = 3;\n"
        "    return 0;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK(result.cpp1.find("unsigned char foo") != std::string::npos);
    CHECK(result.cpp1.find("short bar") != std::string::npos);
}

TEST_CASE("for with next honors break as the enclosing-loop break", "[regression][loops]") {
    const auto source = std::string{
        "main: () -> int = {\n"
        "    values : std::array<i32, 4> = (0, 1, 2, 3);\n"
        "    i := 0;\n"
        "    for values next i++ do (member) {\n"
        "        if member == 0 { break; }\n"
        "    }\n"
        "    return i;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK(result.cpp1.find("for (auto const& member : CPP2_FORWARD(values))") != std::string::npos);
    CHECK(result.cpp1.find("cpp2_for_break_") != std::string::npos);
    CHECK(result.cpp1.find("} while (false);") != std::string::npos);
    CHECK(result.cpp1.find("if (__cpp2_for_break_") != std::string::npos);
}

TEST_CASE("for with next omits the break flag when no break is present",
          "[regression][loops]") {
    const auto source = std::string{
        "main: () -> int = {\n"
        "    values : std::array<i32, 3> = (0, 1, 2);\n"
        "    i := 0;\n"
        "    for values next i++ do (member) { _ = member; }\n"
        "    return i;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("cpp2_for_break_") == std::string::npos);
    CHECK(result.cpp1.find("} while (false);") != std::string::npos);
}

TEST_CASE("for with a parameter and an unbraced body parses the body", "[corpus][upstream][statements]") {
    const auto source = std::string{
        "f: () = {\n"
        "    for words do (word)\n"
        "        std::cout << word;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
    }
    INFO(result.cpp1);
    CHECK(result.cpp1.find("for (auto const& word : words)") != std::string::npos);
    CHECK(result.cpp1.find("std::cout << word") != std::string::npos);
}
}

TEST_CASE("upstream corpus completes without compiler hangs", "[corpus][upstream]") {
    const auto files = load_corpus();
    REQUIRE(files.size() >= 190);

    auto completed = std::size_t{0};
    auto produced_cpp1 = 0;
    auto no_errors = 0;
    auto elapsed_ms = std::chrono::milliseconds::zero();

    for (const auto& file : files) {
        const auto started = std::chrono::steady_clock::now();
        std::cout << file.path.filename() << "\n";
        const auto result = cppfrontier::translator{}.translate_source(file.source);
        std::cout << std::flush;
        elapsed_ms += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        ++completed;
        if (!result.cpp1.empty()) ++produced_cpp1;
        if (std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
                return diagnostic.severity == cppfrontier::diagnostic_severity::error;
            })) {
            ++no_errors;
        }
    }

    INFO("upstream_files=" << files.size()
          << " completed=" << completed
          << " produced_cpp1=" << produced_cpp1
          << " no_errors=" << no_errors
          << " elapsed_ms=" << elapsed_ms.count());
    CHECK(completed == files.size());
}
