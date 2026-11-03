#include <catch2/catch_all.hpp>
#include <cppfrontier/pipeline.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>

TEST_CASE("simple function translates to Cpp1", "[pipeline]") {
    const auto result = cppfrontier::translator{}.translate_source("hello: () = \"hello\";\n");
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto hello() -> decltype(auto) { return \"hello\"; }") != std::string::npos);
    CHECK(result.cpp1.find("#line 1 \"input.cpp2\"") != std::string::npos);
}

TEST_CASE("function parser retains declaration and body tokens", "[parser]") {
    const auto result = cppfrontier::translator{}.translate_source("hello: () = \"hello\";\n");
    REQUIRE(result.program.declarations.size() == 1);
    CHECK(result.program.declarations[0].name == "hello");
    CHECK(result.program.declarations[0].body.source() == "\"hello\"");
}

TEST_CASE("type qualifier parses as a type declaration", "[parser]") {
    const auto source = cppfrontier::source_text::from_bytes("Point: @value type = { x: int = 0; }\n");
    const auto sections = source.cpp2_sections();
    REQUIRE(sections.size() == 1);
    const auto tokens = cppfrontier::tokenize(
        source, sections.front().begin_line, sections.front().end_line_exclusive);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    REQUIRE(parsed.tree.declarations.size() == 1);
    const auto& type = *parsed.tree.declarations[0];
    REQUIRE(type.members.size() == 1);
    CHECK(type.kind == cppfrontier::declaration::node_kind::type);
    CHECK(type.type.text.empty());
    CHECK(type.members[0]->kind == cppfrontier::declaration::node_kind::object);
    CHECK(type.members[0]->type.text == "int");
    CHECK(type.members[0]->name == "x");
}

TEST_CASE("emitter renders declarations without a trailing newline", "[pipeline][emitter]") {
    const auto result = cppfrontier::translator{}.translate_source("hello: () = \"hello\";");
    CHECK(result.cpp1.find("auto hello() -> decltype(auto) { return \"hello\"; }") != std::string::npos);
}

TEST_CASE("single-expression bodies use the documented return rules", "[pipeline][expression-body]") {
    const auto deduced = cppfrontier::translator{}.translate_source("sum: () = 40 + 2;\n");
    CHECK_FALSE(deduced.has_errors());
    CHECK(deduced.cpp1.find("auto sum() -> decltype(auto) { return 40 + 2; }") != std::string::npos);

    const auto explicit_void = cppfrontier::translator{}.translate_source(
        "announce: () -> void = std::cout << \"hello\";\n");
    CHECK_FALSE(explicit_void.has_errors());
    CHECK(explicit_void.cpp1.find(
        "auto announce() -> void { std::cout << \"hello\"; }") != std::string::npos);

    const auto explicit_int = cppfrontier::translator{}.translate_source("answer: () -> int = 42;\n");
    CHECK_FALSE(explicit_int.has_errors());
    CHECK(explicit_int.cpp1.find("auto answer() -> int { return 42; }") != std::string::npos);

    const auto entry_point = cppfrontier::translator{}.translate_source(
        "main: () = std::cout << \"hello\";\n");
    CHECK_FALSE(entry_point.has_errors());
    CHECK(entry_point.cpp1.find(
        "auto main() -> int { std::cout << \"hello\"; }") != std::string::npos);

    const auto explicit_void_wildcard = cppfrontier::translator{}.translate_source(
        "announce: () -> _ = std::cout << \"hello\";\n");
    INFO(explicit_void_wildcard.cpp1);
    CHECK_FALSE(explicit_void_wildcard.has_errors());
    CHECK(explicit_void_wildcard.cpp1.find(
        "auto announce() -> auto { return std::cout << \"hello\"; }") != std::string::npos);
}

TEST_CASE("single-expression entry points return their value", "[pipeline][expression-body][main]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = 42;\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto main() -> int { return 42; }") != std::string::npos);
}

TEST_CASE("main expression bodies follow the declared return semantics",
          "[pipeline][expression-body][main]") {
    const auto default_return = cppfrontier::translator{}.translate_source(
        "main: () = std::cout << \"hello\";\n");
    INFO(default_return.cpp1);
    REQUIRE_FALSE(default_return.has_errors());
    CHECK(default_return.cpp1.find(
        "auto main() -> int { std::cout << \"hello\"; }") != std::string::npos);

    const auto explicit_value = cppfrontier::translator{}.translate_source(
        "main: () -> int = 42;\n");
    INFO(explicit_value.cpp1);
    REQUIRE_FALSE(explicit_value.has_errors());
    CHECK(explicit_value.cpp1.find("auto main() -> int { return 42; }") != std::string::npos);

    const auto args_default_return = cppfrontier::translator{}.translate_source(
        "main: (args) = args.ssize();\n");
    INFO(args_default_return.cpp1);
    REQUIRE_FALSE(args_default_return.has_errors());
    CHECK(args_default_return.cpp1.find(
        "auto const args = cpp2::make_args(argc_, argv_); CPP2_UFCS(ssize)(args);") !=
        std::string::npos);

    const auto args_explicit_value = cppfrontier::translator{}.translate_source(
        "main: (args) -> int = args.ssize();\n");
    INFO(args_explicit_value.cpp1);
    REQUIRE_FALSE(args_explicit_value.has_errors());
    CHECK(args_explicit_value.cpp1.find(
        "auto const args = cpp2::make_args(argc_, argv_); return CPP2_UFCS(ssize)(args);") !=
        std::string::npos);
}

TEST_CASE("main returns int by default and keeps a declared return type",
          "[pipeline][main][review]") {
    const auto translate = [](const char* source) {
        return cppfrontier::translator{}.translate_source(source);
    };
    const auto has_diagnostic = [](const cppfrontier::translation_result& result,
                                   std::string_view code) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    const auto declared = translate("main: () -> double = 2.9;\n");
    INFO(declared.cpp1);
    CHECK(declared.cpp1.find("auto main() -> double { return 2.9; }") != std::string::npos);
    const auto expression = translate("main: () = 3;\n");
    CHECK(expression.cpp1.find("auto main() -> int { 3; }") != std::string::npos);
    CHECK(has_diagnostic(translate("main: () = { return; }\n"), "void-main-return"));
    CHECK(has_diagnostic(translate("main: () -> void = { if true { return; } }\n"),
                         "void-main-return"));
    CHECK_FALSE(translate("main: () -> int = { return 1; }\n").has_errors());
    CHECK_FALSE(translate("main: () = { f := :() -> int = { return 1; }; _ = f(); }\n")
                    .has_errors());
    // A template `main` is left to the Cpp1 compiler.
    CHECK_FALSE(translate("main: <T> () -> int = 0;\n").has_errors());
}

TEST_CASE("forward declarations and members sharing a type's line keep their lines",
          "[pipeline][source-map][review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "A: type = { f: (this) -> int = 1; g: (this) -> int = 2; }\n"
        "h: (x: int) -> int = x;\n"
        "main: () = { }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("#line 1 \"input.cpp2\"\nstruct A;\n#line 2 \"input.cpp2\"\n"
                           "[[nodiscard]] auto h(cpp2::impl::in<int> x) -> int;\n") !=
          std::string::npos);
    CHECK(result.cpp1.find("struct A { [[nodiscard]] auto f() const& -> int; "
                           "[[nodiscard]] auto g() const& -> int;\n") != std::string::npos);
}

TEST_CASE("main rejects parameter declarations not supported by the language",
          "[pipeline][main][parameters][diagnostics]") {
    const auto has_diagnostic = [](const cppfrontier::translation_result& result) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "invalid-main-parameters"; });
    };

    const auto typed = cppfrontier::translator{}.translate_source(
        "main: (args: int) -> int = { return 0; }\n");
    INFO(typed.cpp1);
    CHECK(typed.has_errors());
    CHECK(has_diagnostic(typed));

    const auto wildcard_type = cppfrontier::translator{}.translate_source(
        "main: (args: _) -> int = args.ssize();\n");
    INFO(wildcard_type.cpp1);
    CHECK_FALSE(wildcard_type.has_errors());
    CHECK_FALSE(has_diagnostic(wildcard_type));
    CHECK(wildcard_type.cpp1.find(
        "auto main(int const argc_, char** argv_) -> int") != std::string::npos);

    const auto cpp1_auto_type = cppfrontier::translator{}.translate_source(
        "main: (args: auto) -> int = args.ssize();\n");
    INFO(cpp1_auto_type.cpp1);
    CHECK(cpp1_auto_type.has_errors());
    // The `auto` is reported; the signature it leaves is the same mistake.
    REQUIRE(cpp1_auto_type.diagnostics.size() == 1);
    CHECK(cpp1_auto_type.diagnostics.front().code == "cpp1-auto-type");

    const auto wrong_name = cppfrontier::translator{}.translate_source(
        "main: (arguments) -> int = { return arguments.ssize(); }\n");
    INFO(wrong_name.cpp1);
    CHECK(wrong_name.has_errors());
    CHECK(has_diagnostic(wrong_name));

    const auto wrong_passing = cppfrontier::translator{}.translate_source(
        "main: (copy args) -> int = { return 0; }\n");
    INFO(wrong_passing.cpp1);
    CHECK(wrong_passing.has_errors());
    CHECK(has_diagnostic(wrong_passing));

    const auto too_many = cppfrontier::translator{}.translate_source(
        "main: (args, extra) -> int = { return 0; }\n");
    INFO(too_many.cpp1);
    CHECK(too_many.has_errors());
    CHECK(has_diagnostic(too_many));

    const auto no_parameters = cppfrontier::translator{}.translate_source(
        "main: () -> int = 0;\n");
    INFO(no_parameters.cpp1);
    CHECK_FALSE(no_parameters.has_errors());
    CHECK_FALSE(has_diagnostic(no_parameters));
}

TEST_CASE("explicit deduced returns distinguish by-value and forwarding", "[pipeline][returns][deduction]") {
    const auto by_value = cppfrontier::translator{}.translate_source(
        "first: (values: std::vector<int>) -> _ = { return values[0]; }\n");
    INFO(by_value.cpp1);
    REQUIRE_FALSE(by_value.has_errors());
    CHECK(by_value.cpp1.find("auto first(cpp2::impl::in<std::vector<int>> values) -> auto") !=
          std::string::npos);

    const auto by_forwarding = cppfrontier::translator{}.translate_source(
        "first: (forward values: std::vector<int>) -> forward _ = { return values[0]; }\n");
    INFO(by_forwarding.cpp1);
    REQUIRE_FALSE(by_forwarding.has_errors());
    CHECK(by_forwarding.cpp1.find("auto first(auto&& values) -> decltype(auto)") !=
          std::string::npos);
}

TEST_CASE("typed forwarding returns and parameters follow upstream lowering",
          "[pipeline][returns][parameters]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "first: (forward value: std::string) -> forward std::string = value;\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto first(auto&& value) -> std::string&") != std::string::npos);
    CHECK(result.cpp1.find(
        "requires (std::is_convertible_v<CPP2_TYPEOF(value), "
        "std::add_const_t<std::string>&>)") != std::string::npos);
}

TEST_CASE("function expressions lower in statements and expression bodies", "[pipeline][expressions][function-expressions]") {
    const auto statement = cppfrontier::translator{}.translate_source(
        "main: () -> int = { f := :(x) = x + 1; return f(1); }\n");
    INFO(statement.cpp1);
    REQUIRE_FALSE(statement.has_errors());
    CHECK(statement.cpp1.find("auto f {[](auto const& x) -> decltype(auto) { return x + 1; }};") !=
          std::string::npos);

    const auto expression_body = cppfrontier::translator{}.translate_source(
        "make_adder: () = :(x) = x + 1;\n");
    INFO(expression_body.cpp1);
    REQUIRE_FALSE(expression_body.has_errors());
    CHECK(expression_body.cpp1.find("{ return [](auto const& x) -> decltype(auto) { return x + 1; }; }") !=
          std::string::npos);
}

TEST_CASE("the conditional operator is rejected", "[pipeline][expressions]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "pick: (flag: bool) -> int = flag ? (1) : (2);\n");
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "cpp1-conditional-operator"; }));
}

TEST_CASE("multiple declarations survive translation", "[pipeline][declarations]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = { return name(); }\n\n"
        "name: () -> int = { return 1; }\n");
    REQUIRE(result.program.declarations.size() == 2);
    CHECK(result.program.declarations[0].name == "main");
    CHECK(result.program.declarations[1].name == "name");
    CHECK(result.cpp1.find("auto main() -> int") != std::string::npos);
    CHECK(result.cpp1.find("auto name() -> int") != std::string::npos);
}

TEST_CASE("namespace bodies and aliases are emitted", "[pipeline][namespaces][aliases]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "tools: namespace = {\n"
        "    answer: int = 42;\n"
        "}\n"
        "shorthand: namespace == tools;\n");
    auto diagnostics = std::string{};
    for (const auto& diagnostic : result.diagnostics) diagnostics += diagnostic.message + "\n";
    INFO(diagnostics);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("namespace tools {") != std::string::npos);
    CHECK(result.cpp1.find("int answer {42};") != std::string::npos);
    CHECK(result.cpp1.find("namespace shorthand = tools;") != std::string::npos);
}

TEST_CASE("namespace members support types and functions", "[pipeline][namespaces]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "tools: namespace = {\n"
        "    Name: type == std::string;\n"
        "    get: () -> int = 42;\n"
        "}\n");
    auto diagnostics = std::string{};
    for (const auto& diagnostic : result.diagnostics) diagnostics += diagnostic.message + "\n";
    INFO(diagnostics);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("using Name = std::string;") != std::string::npos);
    CHECK(result.cpp1.find("auto get() -> int") != std::string::npos);
}

TEST_CASE("using declarations lower at every supported scope", "[pipeline][using][namespaces]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "using ns::value;\n"
        "using ns::_;\n"
        "main: () = {\n"
        "    using ::std::string;\n"
        "    using other::_;\n"
        "}\n");
    auto diagnostics = std::string{};
    for (const auto& diagnostic : result.diagnostics) diagnostics += diagnostic.message + "\n";
    INFO(diagnostics);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("using ns::value;") != std::string::npos);
    CHECK(result.cpp1.find("using namespace ns;") != std::string::npos);
    CHECK(result.cpp1.find("using ::std::string;") != std::string::npos);
    CHECK(result.cpp1.find("using namespace other;") != std::string::npos);
}

TEST_CASE("using a later function from an earlier namespace forwards it first",
          "[pipeline][using][namespaces][upstream-1213]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "qq: namespace = {\n"
        "    using ::xf;\n"
        "}\n"
        "xf: () -> int = 42;\n"
        "main: () -> int = { return xf(); }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto declaration = result.cpp1.find("auto xf() -> int;");
    const auto namespace_using = result.cpp1.find("using ::xf;");
    REQUIRE(declaration != std::string::npos);
    REQUIRE(namespace_using != std::string::npos);
    CHECK(declaration < namespace_using);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-using-order");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "using-order.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("namespace wildcard using is emitted at every supported scope",
          "[pipeline][using][namespaces][upstream-1429]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "using std::_;\n"
        "ns: namespace = {\n"
        "    value: int = 1;\n"
        "}\n"
        "outer: namespace = {\n"
        "    using ns::_;\n"
        "}\n"
        "main: () -> int = {\n"
        "    using other::_;\n"
        "    return 0;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("using namespace std;") != std::string::npos);
    CHECK(result.cpp1.find("using namespace ns;") != std::string::npos);
    CHECK(result.cpp1.find("using namespace other;") != std::string::npos);
}

TEST_CASE("local type aliases lower as using declarations", "[pipeline][aliases][locals]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = {\n"
        "    t1: type == bool;\n"
        "    value: t1 = true;\n"
        "    if value { return 0; } return 1;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("using t1 = bool;") != std::string::npos);
    CHECK(result.cpp1.find("constexpr type t1") == std::string::npos);
}

TEST_CASE("namespace type aliases preserve nested template arguments",
          "[pipeline][aliases][templates]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "namespace A {\n"
        "template<bool> struct t { };\n"
        "constexpr bool f(const t<true>&) { return true; }\n"
        "constexpr t<true> o{};\n"
        "}\n"
        "A: namespace = {\n"
        "    t1: <_: t<o.f()>> type == bool;\n"
        "}\n"
        "B: namespace = {\n"
        "    using A::t1;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find(
        "template<t<CPP2_UFCS_NONLOCAL(f)(o)> UnnamedTemplateParam1> using t1 = bool;") !=
          std::string::npos);
    CHECK(result.cpp1.find("using A::t1;") != std::string::npos);
}

TEST_CASE("nested type bodies retain their members", "[pipeline][types]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Box: type = {\n"
        "    Inner: type = { value: int = 0; get: () -> int = value; }\n"
        "    item: Inner = ();\n"
        "}\n");
    auto diagnostics = std::string{};
    for (const auto& diagnostic : result.diagnostics) diagnostics += diagnostic.message + "\n";
    INFO(diagnostics);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("struct Inner {") != std::string::npos);
    CHECK(result.cpp1.find("int value {0};") != std::string::npos);
    CHECK(result.cpp1.find("auto get() -> int") != std::string::npos);
    CHECK(result.cpp1.find("Inner item {};") != std::string::npos);
}

TEST_CASE("type members use Cpp2 access defaults", "[pipeline][types][access]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 0;\n"
        "    read: (this) -> int = value;\n"
        "    public visible: int = 1;\n"
        "    private hidden: int = 2;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("struct Widget {\nprivate: int value {0};") != std::string::npos);
    CHECK(result.cpp1.find("public:") < result.cpp1.find("auto read()"));
    CHECK(result.cpp1.find("int visible {1};") != std::string::npos);
    CHECK(result.cpp1.find("private:") < result.cpp1.find("int hidden {2};"));
    CHECK(result.cpp1.find("int hidden {2};") != std::string::npos);
}

TEST_CASE("protected member declarations are recognized", "[pipeline][types][access]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = { protected value: int = 0; }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("protected: int value {0};") != std::string::npos);
}

TEST_CASE("parenthesis default initialization lowers safely", "[pipeline][types][initializers]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Box: type = { value: int = ( ); }\n"
        "item: Box = ( );\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("int value {};") != std::string::npos);
    CHECK(result.cpp1.find("Box item {};") != std::string::npos);

    // A braced list is Cpp1.
    const auto braced = cppfrontier::translator{}.translate_source(
        "Box: type = { other: int = { }; }\n");
    CHECK(braced.has_errors());
    CHECK(braced.diagnostics.front().code == "braced-list");
}

TEST_CASE("an object initialized with a block is one error at its name", "[pipeline][initializers]") {
    // A namespace, type or function scope object, with or without a `;` after
    // the block; upstream reports each with this message.
    for (const auto* source : {
             "x: int = { return 1; }\n",
             "x := { return 1; };\n",
             "t: type = { public x: int = { return 1; } y: int = 2; }\n",
             "f: () = { x: int = { return 1; } }\n",
             "f: () = { static x := { return 1; }; }\n"}) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        INFO(source);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().code == "object-initializer-not-expression");
        CHECK(result.diagnostics.front().message == "an object initializer must be an expression");
    }
    const auto local = cppfrontier::translator{}.translate_source(
        "f: () = { static x: int = { return 1; } }\n");
    REQUIRE(local.diagnostics.size() == 1);
    CHECK(local.diagnostics.front().span.begin.column == 18);
    // A braced list holds no statement.
    const auto braced = cppfrontier::translator{}.translate_source("x: std::vector<int> = {1, 2};\n");
    REQUIRE(braced.diagnostics.size() == 1);
    CHECK(braced.diagnostics.front().code == "braced-list");
}

TEST_CASE("member functions with in this are const-reference-qualified",
          "[pipeline][types][members]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = { read: (this) -> int = 0; }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto read() const& -> int") != std::string::npos);
}

TEST_CASE("function and object aliases lower as constexpr declarations", "[pipeline][aliases]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "square: (i: i32) -> i32 == i * i;\n"
        "BufferSize: i32 == 8;\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find(
        "constexpr auto square(cpp2::impl::in<int> i) -> int { return i * i; }") !=
          std::string::npos);
    CHECK(result.cpp1.find("constexpr int BufferSize = 8;") != std::string::npos);
}

TEST_CASE("function alias forward declarations preserve constexpr", "[pipeline][aliases][emitter]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "square: (i: i32) -> i32 == i * i;\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("constexpr auto square(cpp2::impl::in<int> i) -> int;") !=
          std::string::npos);
}

TEST_CASE("object alias storage follows declaration scope", "[pipeline][aliases][scope]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "BufferSize: i32 == 8;\n"
        "tools: namespace = {\n"
        "    NamespaceSize: i32 == 16;\n"
        "}\n"
        "local_size: () -> i32 = {\n"
        "    LocalSize: i32 == 4;\n"
        "    return LocalSize;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("inline constexpr int BufferSize = 8;") != std::string::npos);
    CHECK(result.cpp1.find("inline constexpr int NamespaceSize = 16;") != std::string::npos);
    CHECK(result.cpp1.find("constexpr int LocalSize {4};") != std::string::npos);
}

TEST_CASE("type-scope object aliases are static constexpr members",
          "[pipeline][aliases][scope][upstream-1221]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Board: type = {\n"
        "    kWidth: size_t == 4;\n"
        "    kSize: size_t == kWidth * 4;\n"
        "    public cells: std::array<int, kSize> = ();\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("struct Board {\nstatic constexpr size_t kWidth {4};\n"
                           "static constexpr size_t kSize {kWidth * 4};\n"
                           "std::array<int, kSize> cells {};") != std::string::npos);
}

TEST_CASE("nested type bodies parse independently from their parent", "[pipeline][types][parser]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "Box: type = {\n"
        "    Inner: type = { value: int = 0; }\n"
        "    item: Inner = ();\n"
        "}\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 5);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    REQUIRE(parsed.tree.declarations.size() == 1);
    CHECK(parsed.tree.declarations[0]->name == "Box");
    REQUIRE(parsed.tree.declarations[0]->members.size() == 2);
    CHECK(parsed.tree.declarations[0]->members[0]->name == "Inner");
    CHECK(parsed.tree.declarations[0]->members[0]->kind ==
          cppfrontier::declaration_kind::type);
    CHECK(parsed.tree.declarations[0]->members[1]->name == "item");
}

TEST_CASE("member type declarations consume their nested body", "[parser][types][members]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "Box: type = { Inner: type = { value: int = 0; } }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    for (const auto& diagnostic : parsed.diagnostics) INFO(diagnostic.message);
    REQUIRE(parsed.tree.declarations.size() == 1);
    REQUIRE(parsed.tree.declarations[0]->members.size() == 1);
    const auto& inner = *parsed.tree.declarations[0]->members[0];
    CHECK(inner.name == "Inner");
    CHECK(inner.kind == cppfrontier::declaration_kind::type);
    REQUIRE(inner.members.size() == 1);
    CHECK(inner.members[0]->name == "value");
}

TEST_CASE("multi-line nested type bodies retain their members", "[parser][types][members]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "Box: type = {\n"
        "    Inner: type = {\n"
        "        value: int = 0;\n"
        "    }\n"
        "}\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 6);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    for (const auto& diagnostic : parsed.diagnostics) INFO(diagnostic.message);
    REQUIRE(parsed.tree.declarations.size() == 1);
    REQUIRE(parsed.tree.declarations[0]->members.size() == 1);
    const auto& inner = *parsed.tree.declarations[0]->members[0];
    CHECK(inner.name == "Inner");
    REQUIRE(inner.members.size() == 1);
    CHECK(inner.members[0]->name == "value");
}

TEST_CASE("break in a loop with next skips the next expression", "[pipeline][upstream-1436]") {
    const auto source = R"cpp2(
main: () -> int = {
    i := 0;
    while i < 4 next i++ {
        if i == 0 {
            break;
        }
        i++;
    }
    return i;
}
)cpp2";
    const auto result = cppfrontier::translator{}.translate_source(std::string(source));
    CHECK(std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& issue) {
        return issue.code == "unexpected-close-brace";
    }));
    CHECK(std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& issue) {
        return issue.code == "unexpected-token";
    }));
    CHECK(result.diagnostics.empty());
    CHECK(result.cpp1.find("for (; cpp2::impl::cmp_less(i,4); ++i)") != std::string::npos);
    CHECK(result.cpp1.find("++i") != std::string::npos);
    CHECK(result.cpp1.find("do {") == std::string::npos);
    CHECK(result.cpp1.find("while (false)") == std::string::npos);
}

TEST_CASE("local deduced declarations lower to Cpp1 declarations", "[pipeline][locals]") {
    const auto source = R"cpp2(
main: () -> int = {
    i := 0;
    s: std::string = "x";
    return i;
}
)cpp2";
    const auto result = cppfrontier::translator{}.translate_source(std::string(source));
    CHECK(result.diagnostics.empty());
    CHECK(result.cpp1.find("auto i {0};") != std::string::npos);
    CHECK(result.cpp1.find("std::string s {\"x\"};") != std::string::npos);
}

TEST_CASE("function-local static declarations emit static storage", "[pipeline][locals][static]") {
    const auto source = R"cpp2(
next_value: () -> int = {
    static typed: int = 0;
    static deduced := 0;
    ordinary := 0;
    typed += 1;
    deduced += 1;
    ordinary += 1;
    return typed + deduced + ordinary;
}
)cpp2";
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("static int typed {0};") != std::string::npos);
    CHECK(result.cpp1.find("static auto deduced {0};") != std::string::npos);
    CHECK(result.cpp1.find("auto ordinary {0};") != std::string::npos);
    CHECK(result.cpp1.find("auto auto") == std::string::npos);
}

TEST_CASE("typed local declarations reject trailing constructor syntax", "[pipeline][locals][diagnostics]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = { s: std::string(\"x\"); return 0; }\n");
    INFO(result.cpp1);
    CHECK(result.has_errors());
    CHECK(result.cpp1.empty());
}

TEST_CASE("function types lower at every declaration scope", "[pipeline][types][functions]") {
    const auto source = std::string{
        "intfuncptr: type == *(_: int) -> void;\n"
        "handler: (cb: intfuncptr) -> int = {\n"
        "    return 1;\n"
        "}\n"
        "Widget: type = {\n"
        "    cb: *(_: int) -> void;\n"
        "    f: std::function< (i: int) -> int >;\n"
        "}\n"
        "global_cb: *(_: int) -> void = handler;\n"
        "run: (cb: *(_: int) -> void) -> void = {\n"
        "    _ = cb;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find(
        "using intfuncptr = void(*)([[maybe_unused]] cpp2::impl::in<int> unnamed_param_1);") !=
        std::string::npos);
    CHECK(result.cpp1.find(
        "auto run(void(*cb)([[maybe_unused]] cpp2::impl::in<int> unnamed_param_1)) -> void") !=
        std::string::npos);
    CHECK(result.cpp1.find(
        "void(*cb)([[maybe_unused]] cpp2::impl::in<int> unnamed_param_1);") != std::string::npos);
    CHECK(result.cpp1.find(
        "std::function<int(cpp2::impl::in<int> i)> f;") != std::string::npos);
    CHECK(result.cpp1.find(
        "void(*global_cb)([[maybe_unused]] cpp2::impl::in<int> unnamed_param_1)") !=
        std::string::npos);
}

TEST_CASE("multi-character Cpp1 operators remain joined", "[pipeline][tokens]") {
    const auto source = "f: (a: int, b: int) -> int = { x := a; x >>= b * 2; return x >> 1; }\n";
    const auto result = cppfrontier::translator{}.translate_source(source);
    CHECK(result.diagnostics.empty());
    CHECK(result.cpp1.find("auto x {a};") != std::string::npos);
    CHECK(result.cpp1.find("x >>=") != std::string::npos);
    CHECK(result.cpp1.find("return cpp2::move(x) >> 1;") != std::string::npos);
}

TEST_CASE("mixed Cpp2 sources preserve Cpp1 includes", "[pipeline][source]") {
    const auto source = R"cpp2(#define CPP2_INCLUDE_STD Yes
#include "cpp2util.h"
main: () -> int = { return 0; }
)cpp2";
    const auto result = cppfrontier::translator{}.translate_source(source);
    CHECK(result.diagnostics.empty());
    CHECK(result.cpp1.find("#define CPP2_INCLUDE_STD Yes") != std::string::npos);
    CHECK(result.cpp1.find("#include \"cpp2util.h\"") != std::string::npos);
}

TEST_CASE("generated declarations precede a conditional the first code line is in",
          "[pipeline][source][preprocessor][review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "#include <iostream>\n"
        "#ifdef WIDE\n"
        "int g(long x) {\n"
        "#else\n"
        "int g(int x) {\n"
        "#endif\n"
        "    return x;\n"
        "}\n"
        "f: () -> int = g(3);\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto declaration = result.cpp1.find("auto f() -> int;");
    REQUIRE(declaration != std::string::npos);
    CHECK(declaration < result.cpp1.find("#ifdef WIDE"));

    // A header guard encloses everything after it, so the declarations follow its includes.
    const auto guarded = cppfrontier::translator{}.translate_source(
        "#ifndef G_H\n#define G_H\n#include <vector>\nf: () -> int = 3;\n#endif\n");
    INFO(guarded.cpp1);
    REQUIRE_FALSE(guarded.has_errors());
    CHECK(guarded.cpp1.find("#include <vector>") < guarded.cpp1.find("auto f() -> int;"));
}

TEST_CASE("generated Cpp2 output includes its support header", "[pipeline][source][includes]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = { return 0; }\n");
    INFO(result.cpp1);
    CHECK(result.cpp1.find("#include \"cpp2util.h\"") != std::string::npos);
}

TEST_CASE("restricted runtime defines precede the support header",
          "[pipeline][options][exceptions][rtti]") {
    auto options = cppfrontier::translation_options{};
    options.exceptions = false;
    options.rtti = false;
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = { return 0; }\n", options);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto exceptions = result.cpp1.find("#define CPP2_NO_EXCEPTIONS");
    const auto rtti = result.cpp1.find("#define CPP2_NO_RTTI");
    const auto include = result.cpp1.find("#include \"cpp2util.h\"");
    REQUIRE(exceptions != std::string::npos);
    REQUIRE(rtti != std::string::npos);
    REQUIRE(include != std::string::npos);
    CHECK(exceptions < include);
    CHECK(rtti < include);
}

TEST_CASE("default runtime options omit restricted defines",
          "[pipeline][options][exceptions][rtti]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = { return 0; }\n");
    CHECK(result.cpp1.find("CPP2_NO_EXCEPTIONS") == std::string::npos);
    CHECK(result.cpp1.find("CPP2_NO_RTTI") == std::string::npos);
}

TEST_CASE("restricted runtime options leave Cpp1-only input unchanged",
          "[pipeline][options][exceptions][rtti]") {
    auto options = cppfrontier::translation_options{};
    options.exceptions = false;
    options.rtti = false;
    const auto result = cppfrontier::translator{}.translate_source(
        "int main() { return 0; }\n", options);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("CPP2_NO_EXCEPTIONS") == std::string::npos);
    CHECK(result.cpp1.find("CPP2_NO_RTTI") == std::string::npos);
    CHECK(result.cpp1.find("#include \"cpp2util.h\"") == std::string::npos);
}

TEST_CASE("standalone Cpp2 sources receive required standard headers", "[pipeline][source]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = { std::cout << \"x\"; return 0; }\n");
    CHECK(result.cpp1.find("#include <iostream>") != std::string::npos);
}

TEST_CASE("empty local initializers lower to value initialization", "[pipeline][initializers]") {
    const auto source = std::string{
        "main: () -> int = {\n"
        "    typed: std::string = ();\n"
        "    _ = typed;\n"
        "    return 0;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("std::string typed {};") != std::string::npos);
    CHECK(result.cpp1.find("std::string typed ();") == std::string::npos);
}

TEST_CASE("Cpp2 object initializers use direct-list initialization",
          "[pipeline][initializers][compile]") {
    const auto source = std::string{
        "struct Explicit { int value; explicit Explicit(int v) : value(v) {} };\n"
        "\n"
        "main: () -> int = {\n"
        "    item: Explicit = (7);\n"
        "    return item.value - 7;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("Explicit item {7};") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-initializer-form");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "initializer-form.cpp";
    test_support::write_file(cpp1_path, result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("parenthesized call arguments remain expressions",
          "[pipeline][initializers][upstream-542]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (a: int, b: int) -> int = a + b;\n"
        "main: () -> int = { return f((1, 2)); }\n");
    // Cpp2 has no comma operator, so a list is no argument.
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) { return issue.code == "comma-operator"; }));
}

TEST_CASE("typed and deduced Point construction keep their distinct forms",
          "[pipeline][initializers][upstream-823]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Point: type = {\n"
        "    value: i32 = 0;\n"
        "    operator=: (out this, a: i32, b: i32) = { value = a * b; }\n"
        "}\n"
        "main: () -> int = {\n"
        "    var1: Point = (1, 2);\n"
        "    var2: = Point(1, 2);\n"
        "    return var1.value + var2.value;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("Point var1 {1, 2};") != std::string::npos);
    CHECK(result.cpp1.find("auto var2 {Point(1, 2)};") != std::string::npos);
}

TEST_CASE("explicit source includes are emitted once", "[pipeline][source][includes]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "#include <string>\n"
        "main: () -> int = { std::cout << std::string{\"x\"}; return 0; }\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find("#include <iostream>") != std::string::npos);
    CHECK(result.cpp1.find("#include <string>") != std::string::npos);
    CHECK(result.cpp1.find("#include <iostream>", result.cpp1.find("#include <iostream>") + 1) ==
          std::string::npos);
    CHECK(result.cpp1.find("#include <string>", result.cpp1.find("#include <string>") + 1) ==
          std::string::npos);
}

TEST_CASE("raw string passthrough is not interrupted by line directives", "[pipeline][source][raw]") {
    const auto source = std::string{
        "auto text = R\"(first\n"
        "second)\";\n"
        "main: () -> int = { return 0; }\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find("R\"(first\nsecond)\";") != std::string::npos);
    CHECK(result.cpp1.find("R\"(first\n#line") == std::string::npos);
}

TEST_CASE("multiline raw strings preserve following Cpp2 statements",
          "[pipeline][source][raw][upstream-1425]") {
    const auto source = std::string{
        "raw_text: std::string = R\"test(first (line)\n"
        "\n"
        "second (line))test\";\n"
        "\n"
        "main: () -> int = {\n"
        "    local: std::string = R\"test(first (line)\n"
        "\n"
        "second (line))test\";\n"
        "    std::cout << local << raw_text;\n"
        "    return 0;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("std::cout << CPP2_FORWARD(local) << raw_text;") != std::string::npos);
    CHECK(result.cpp1.find("first (line)") != std::string::npos);
    CHECK(result.cpp1.find("second (line)") != std::string::npos);
}

TEST_CASE("file-scope multiline raw strings preserve their contents and following Cpp2",
          "[pipeline][source][raw][upstream-442]") {
    const auto source = std::string{
        "raw_str_multi_filescope: std::string = R\"test(this is raw string literal\n"
        "\n"
        "that has (an open paren) and can last for multiple\n"
        "\n"
        "lines)test\";\n"
        "main: () -> int = { return 0; }\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find(
        "R\"test(this is raw string literal\n\n"
        "that has (an open paren) and can last for multiple\n\n"
        "lines)test\"") != std::string::npos);
    CHECK(result.cpp1.find("auto main() -> int") != std::string::npos);
}

TEST_CASE("comment markers in strings remain ordinary code",
          "[pipeline][source][raw][upstream-442]") {
    const auto source = std::string{
        "main: () = {\n"
        "    std::cout << \"//123\";\n"
        "    a := \"*/ ok\";\n"
        "    { c := \"*/ ok\"; }\n"
        "    b := \"/* error even without {} block\";\n"
        "    raw := R\"(/* // raw text */)\";\n"
        "    std::cout << a << c << b << raw;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("\"//123\"") != std::string::npos);
    CHECK(result.cpp1.find("\"/* error even without {} block\"") != std::string::npos);
    CHECK(result.cpp1.find("R\"(/* // raw text */)\"") != std::string::npos);
    CHECK(result.cpp1.find("std::cout << cpp2::move(a) << c << cpp2::move(b) << cpp2::move(raw);") != std::string::npos);
}

TEST_CASE("include policy does not infer headers from quoted source text", "[pipeline][source]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = { std::cout << \"std::string\"; return 0; }\n");
    CHECK(result.cpp1.find("#include <iostream>") != std::string::npos);
    CHECK(result.cpp1.find("#include <string>") == std::string::npos);
}

TEST_CASE("include policy does not infer headers from comments", "[pipeline][source]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = { // std::cout\n return 0; }\n");
    CHECK(result.cpp1.find("#include <iostream>") == std::string::npos);
}

TEST_CASE("include policy sees types in nested declarations", "[pipeline][source][includes][namespaces]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "tools: namespace = {\n"
        "    name: std::string = \"x\";\n"
        "}\n");
    INFO(result.cpp1);
    CHECK(result.cpp1.find("#include <string>") != std::string::npos);
}

TEST_CASE("include policy sees types in nested member functions", "[pipeline][source][includes][types]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    name: (this) -> std::string = \"x\";\n"
        "}\n");
    INFO(result.cpp1);
    CHECK(result.cpp1.find("#include <string>") != std::string::npos);
}

TEST_CASE("try and catch preserve typed and catch-all handlers", "[pipeline][statements][exceptions]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: () = {\n"
        "    try {\n"
        "        throw(42);\n"
        "    } catch (i: int) {\n"
        "        _ = i;\n"
        "    } catch (_) {\n"
        "        return;\n"
        "    }\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.code << ": " << diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("try {") != std::string::npos);
    CHECK(result.cpp1.find("catch (cpp2::impl::in<int> i) {") != std::string::npos);
    CHECK(result.cpp1.find("catch (...) {") != std::string::npos);
    CHECK(result.cpp1.find("throw(42)") != std::string::npos);
}

TEST_CASE("loop clauses use the full expression renderer", "[pipeline][loops][division][safety]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = {\n"
        "    divisor := 2;\n"
        "    i := 0;\n"
        "    while i / divisor < 4 next i += 1 {\n"
        "        i++;\n"
        "    }\n"
        "    return i;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find(
        "for (; cpp2::impl::cmp_less(i / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(i),divisor),4); "
        "i += 1) {") != std::string::npos);
}

TEST_CASE("unbraced range-for bodies lower as a single statement",
          "[pipeline][loops][upstream-1426]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (x: std::vector<double>) -> int = {\n"
        "    result := 0;\n"
        "    for x do (cur)\n"
        "    if cur != 0.0 {\n"
        "        result += 1;\n"
        "    }\n"
        "    for x next _ = 0 do (cur)\n"
        "    for x do (unused) { _ = unused; }\n"
        "    return result;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("if (cur != 0.0)") != std::string::npos);
    CHECK(result.cpp1.find("for (auto const& cur : x) {") != std::string::npos);
}

TEST_CASE("unbraced range-for requires a body", "[pipeline][loops][diagnostics]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: () = { for values do (item) }\n");
    const auto has_diagnostic = [&](std::string_view code) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    CHECK(has_diagnostic("expected-body"));
}

TEST_CASE("concepts lower to Cpp1 concept definitions", "[pipeline][concepts][upstream-1388]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Foo: type = {}\n"
        "FooType: <T> concept = std::is_base_of_v<Foo, T>;\n"
        "Bar: <T: FooType> type = {}\n"
        "main: () -> int = { return 0; }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("template<typename T> concept FooType = "
                           "std::is_base_of_v<Foo, T>;") != std::string::npos);
    CHECK(result.cpp1.find("template<FooType T> struct Bar") != std::string::npos);
    // The concept precedes the forward declaration that uses it.
    CHECK(result.cpp1.find("concept FooType") < result.cpp1.find("template<FooType T> struct Bar;"));

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-concept-order");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "concept-order.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("constexpr functions are usable from file-scope static_assert",
          "[pipeline][constexpr][upstream-1074]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "add: (a: int, b: int) -> int == { return a + b; }\n"
        "twice: <T> (x: T) -> T == x + x;\n"
        "\n"
        "static_assert(add(5, 6) == 11);\n"
        "main: () -> int = { static_assert(twice(2) == 4); return add(1, 2) - 3; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("template<typename T> [[nodiscard]] constexpr auto twice(") != std::string::npos);
    CHECK(result.cpp1.find("constexpr auto add(") < result.cpp1.find("static_assert(add"));
    CHECK(result.cpp1.find("return a + b;") < result.cpp1.find("static_assert(add"));

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-constexpr");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "constexpr.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("parameters depending on a non-type template parameter stay deducible",
          "[pipeline][templates][parameters][upstream-595]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "total: <N: std::size_t> (values: std::array<int, N>) -> int = {\n"
        "  sum := 0;\n"
        "  for values do (v) { sum += v; }\n"
        "  return sum;\n"
        "}\n"
        "main: () -> int = { a: std::array<int, 3> = (1, 2, 3); return total(a) - 6; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("std::array<int, N> const& values") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-nontype-deduction");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "nontype-deduction.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("expression-bodied constructors do not return a value",
          "[pipeline][constructors][upstream-822]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "holder: type = {\n"
        "  view: * int;\n"
        "  operator=: (out this, inout v: int) = view = v&;\n"
        "  operator=: (inout this, inout v: int) = view = v&;\n"
        "}\n"
        "main: () -> int = { a := 1; b := 2; h: holder = a; h = b; _ = b; return a - 1; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("holder(int& v) : view{&(v)} {\n}") != std::string::npos);
    CHECK(result.cpp1.find("{\n    view = &(v); \n    return *this;\n}") != std::string::npos);
}

TEST_CASE("concepts reject scopes and missing template parameters",
          "[pipeline][concepts][diagnostics]") {
    const auto has_diagnostic = [](const cppfrontier::translation_result& result,
                                   std::string_view code) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; });
    };

    const auto no_template = cppfrontier::translator{}.translate_source(
        "bad: concept = true;\n");
    CHECK(has_diagnostic(no_template, "concept-requires-template-parameters"));

    const auto type_scope = cppfrontier::translator{}.translate_source(
        "Box: type = { bad: <T> concept = true; }\n");
    CHECK(has_diagnostic(type_scope, "concept-declaration-scope"));

    const auto function_scope = cppfrontier::translator{}.translate_source(
        "main: () -> int = { bad: <T> concept = true; return 0; }\n");
    CHECK(has_diagnostic(function_scope, "concept-declaration-scope"));
}

TEST_CASE("dependent type template arguments lower with typename",
          "[pipeline][templates][upstream-727]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: <T> () -> std::vector<type T::value_type> = { return (); }\n"
        "g: <T> (value: type T::value_type) -> void = { }\n"
        "h: <T> () -> _ = std::vector<type T::value_type>();\n"
        "main: () -> int = { return h<std::vector<int>>().size(); }\n");
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("std::vector<typename T::value_type>") != std::string::npos);
    CHECK(result.cpp1.find("typename T::value_type const& value") != std::string::npos);
    CHECK(result.cpp1.find("type T::value_type") == std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-dependent-type");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "dependent-type.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("the type marker and dependent aliases lower with typename",
          "[pipeline][templates][upstream-727]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: <T> () -> int = {\n"
        "    a := sizeof(type T::x);\n"
        "    d: type T::x = 4;\n"
        "    e := (type T::x)();\n"
        "    A: type == T;\n"
        "    g: A::x = 5;\n"
        "    return a + d + e + g;\n"
        "}\n"
        "n: <T> type = { A: type == T; z: A::x = 1; }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("sizeof(typename T::x)") != std::string::npos);
    CHECK(result.cpp1.find("typename T::x d {4}") != std::string::npos);
    CHECK(result.cpp1.find("auto e {typename T::x()}") != std::string::npos);
    CHECK(result.cpp1.find("typename A::x g {5}") != std::string::npos);
    CHECK(result.cpp1.find("typename A::x z {1}") != std::string::npos);
}

TEST_CASE("deferred initialization preserves brace narrowing for #1446",
          "[pipeline][initializers][upstream-1446]") {
    const auto source = std::string{
        "#include <cstdint>\n"
        "#include <array>\n"
        "struct S {\n"
        "    std::int8_t x;\n"
        "    std::int8_t y;\n"
        "};\n"
        "main: () -> int = {\n"
        "    small: u8;\n"
        "    small = 'a';\n"
        "    negative: i8;\n"
        "    negative = -1;\n"
        "    values: std::array<i8, 3>;\n"
        "    values = (5, 6, 7);\n"
        "    pair: S;\n"
        "    pair = (100, 8);\n"
        "    return values[0] - 5;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find(
              "small.construct(std::type_identity_t<unsigned char>{'a'})") !=
          std::string::npos);
    CHECK(result.cpp1.find(
              "values.construct_from([&]{ return "
              "std::type_identity_t<std::array<signed char, 3>>{5, 6, 7}; })") !=
          std::string::npos);
    CHECK(result.cpp1.find("pair.construct_from([&]{ return std::type_identity_t<S>{100, 8}; })") !=
          std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-1446");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "deferred-narrowing.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("a comment line separates adjacent Cpp2 declarations",
          "[pipeline][source][comments][upstream-1449]") {
    const auto source = std::string{
        "first: (out value: i8) = { value = 1; }\n"
        "// between declarations\n"
        "second: (out value: i8) = { value = 2; }\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto first(") != std::string::npos);
    CHECK(result.cpp1.find("auto second(") != std::string::npos);
}

TEST_CASE("statement parameters scope a statement",
          "[pipeline][statement-parameters][upstream-544][upstream-1003]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = {\n"
        "  local := 42;\n"
        "  (inout i := local) { i++; }\n"
        "  (x := 1) (y := 2) local += x + y;\n"
        "  m: std::mutex = ();\n"
        "  (_: std::unique_lock = (m)) { }\n"
        "  _ = m&;\n"
        "  (copy v: std::vector<int> = (1, 2, 3)) v.push_back(4);\n"
        "  return local - 46;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("auto& i{local};") != std::string::npos);
    CHECK(result.cpp1.find("auto const& y{2};") != std::string::npos);
    CHECK(result.cpp1.find("std::unique_lock const unnamed_param_1{m};") != std::string::npos);
    CHECK(result.cpp1.find("std::vector<int> v{1, 2, 3};") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-statement-parameters");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "statement-parameters.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("statement parameters require an initializer",
          "[pipeline][statement-parameters][diagnostics][upstream-826][upstream-1135]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () = { (copy x: int) { } }\n");
    CHECK(result.has_errors());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "statement-parameter-initializer";
        }));
    const auto expression = cppfrontier::translator{}.translate_source(
        "f: (b: int, c: int) -> int = { (b + c); return (b); }\n");
    CHECK_FALSE(expression.has_errors());
}

TEST_CASE("operator functions lower with their operator names",
          "[pipeline][operators][upstream-1096][upstream-1214][upstream-1283]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "V: type = {\n"
        "    public x: int = 0;\n"
        "    operator=: (out this, v: int) = { x = v; }\n"
        "    operator(): (this, y: int) -> int = x + y;\n"
        "    operator[]: (this, i: int) -> int = x * i;\n"
        "    operator==: (this, that) -> bool = x == that.x;\n"
        "    operator+=: (inout this, v: int) -> forward V = { x += v; return this; }\n"
        "}\n"
        "operator+: (a: V, b: V) -> V = (a.x + b.x);\n"
        "main: () -> int = {\n"
        "    using std::operator\"\"sv;\n"
        "    v: V = (2);\n"
        "    v += 1;\n"
        "    std::cout << \"x\"sv;\n"
        "    return v(1) + v[2] - (v + v).x - 4 + (v == v);\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("auto operator()(cpp2::impl::in<int> y) const& -> int") !=
          std::string::npos);
    CHECK(result.cpp1.find("auto operator==([[maybe_unused]] V const& that) const& -> bool") != std::string::npos);
    CHECK(result.cpp1.find("auto operator+(cpp2::impl::in<V> a, cpp2::impl::in<V> b) -> V;") !=
          std::string::npos);
    CHECK(result.cpp1.find("using std::operator\"\"sv;") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-operator-names");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "operator-names.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("a type after := is diagnosed", "[pipeline][declarations][diagnostics][upstream-436]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () = { my_int := i32 = 300; my_char := char = -1; }\n");
    CHECK(result.has_errors());
    CHECK(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "deduced-declaration-type"; }) == 2);
    CHECK(result.diagnostics.size() == 2);
    const auto assignment = cppfrontier::translator{}.translate_source(
        "main: () = { y := 0; x := y = 3; _ = x; }\n");
    REQUIRE(assignment.diagnostics.size() == 1);
    CHECK(assignment.diagnostics.front().code == "chained-assignment");
}

TEST_CASE("named return values return a struct",
          "[pipeline][named-returns][upstream-543][upstream-1109]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (c: bool) -> (i: int, s: std::string) = {\n"
        "    i = 10;\n"
        "    if c { s = \"xyzzy\"; }\n"
        "    else { s = \"plugh\"; }\n"
        "}\n"
        "g: () -> (x: int = 5, y: int) = { y = x * 2; return; }\n"
        "A: type = { operator(): (this) -> (x: int, y: int) = { x = 1; y = 2; } }\n"
        "main: () -> int = { r := f(true); a: A = (); return r.i + g().y + a().y - 22; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("struct f_ret { int i; std::string s; };\n"
                           "[[nodiscard]] auto f(cpp2::impl::in<bool> c) -> f_ret;") != std::string::npos);
    CHECK(result.cpp1.find("return { std::move(i.value()), std::move(s.value()) };") !=
          std::string::npos);
    CHECK(result.cpp1.find("return { std::move(x), std::move(y.value()) };") !=
          std::string::npos);
    CHECK(result.cpp1.find("struct operator_call_ret { int x; int y; };") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-named-returns");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "named-returns.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);
}

TEST_CASE("a single named return value returns its type",
          "[pipeline][named-return][named-returns]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: () -> (ri: int = 0) = { ri = 42; }\n"
        "g: (c: bool) -> (out ri: int) = { if c { ri = 1; return; } ri = 2; }\n"
        "h: (forward s: std::string) -> (forward r: std::string) = { r = s; }\n"
        "A: type = { get: (this) -> (r: int) = { r = 3; } }\n"
        "main: () -> int = { a: A = (); return f() + g(true) + g(false) + a.get() + "
        "std::ssize(h(\"x\")) - 49; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("using f_ret = int;\n[[nodiscard]] auto f() -> f_ret;") != std::string::npos);
    CHECK(result.cpp1.find("    int ri {0};\n#line 1\n    ri = 42;\n    return ri;\n}") !=
          std::string::npos);
    CHECK(result.cpp1.find("using g_ret = int;") != std::string::npos);
    CHECK(result.cpp1.find("return std::move(ri.value());\n    }") != std::string::npos);
    CHECK(result.cpp1.find("using h_ret = std::string;") != std::string::npos);
    CHECK(result.cpp1.find("using get_ret = int;\n[[nodiscard]] auto get() const& -> get_ret") !=
          std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-named-return");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "single-named-return.cpp";
    const auto binary_path = scratch / "single-named-return";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, cpp1_path.string(), "-o",
         binary_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    REQUIRE(compiler_result.exit_status == 0);
    const auto run = test_support::run_process({binary_path.string()}, scratch,
                                               std::chrono::seconds{10});
    CHECK(run.exit_status == 0);
}

TEST_CASE("named return values reject value returns and expression bodies",
          "[pipeline][named-returns][diagnostics]") {
    const auto has = [](const std::string& source, std::string_view code) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    CHECK(has("f: () -> (x: int) = { x = 1; return x; }\n", "named-return-value"));
    CHECK(has("f: () -> (x: int) = 1;\n", "named-return-body"));
    CHECK(has("f: () -> (x) = { }\n", "invalid-named-return"));
    CHECK(has("f: () -> (in x: int) = { x = 1; }\n", "invalid-named-return"));
    CHECK(has("f: () -> (move x: int) = { x = 1; }\n", "invalid-named-return"));
}

TEST_CASE("assert statements check their contract group",
          "[pipeline][contracts][upstream-686]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () = {\n"
        "  i := 42;\n"
        "  assert(i == 42);\n"
        "  assert<testing, true>(i < 43, \"small\");\n"
        "  assert<unevaluated>(anything);\n"
        "  (j := 1) assert(j == 1);\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("if (cpp2::cpp2_default.is_active() && !(i == 42) ) "
                           "{ cpp2::cpp2_default.report_violation(\"\"); }") != std::string::npos);
    CHECK(result.cpp1.find("if (true && cpp2::testing.is_active() && !(cpp2::impl::cmp_less(i,43)) ) "
                           "{ cpp2::testing.report_violation(CPP2_CONTRACT_MSG(\"small\")); }") !=
          std::string::npos);
    CHECK(result.cpp1.find("anything") == std::string::npos);
    CHECK(result.cpp1.find("!(j == 1)") != std::string::npos);

    const auto uninitialized = cppfrontier::translator{}.translate_source(
        "main: () = { x: int; assert(x == 1); x = 1; }\n");
    CHECK(uninitialized.has_errors());
    const auto malformed = cppfrontier::translator{}.translate_source(
        "main: () = { assert(); }\n");
    CHECK(malformed.has_errors());
}

TEST_CASE("nested expression lists are nested initializers",
          "[pipeline][initializers][upstream-1037]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = {\n"
        "  m: std::map<std::string, int> = ((\"CPU\", 10), (\"GPU\", 15));\n"
        "  v: std::vector<std::pair<int, int>> = ((1, 2));\n"
        "  x := (1 + 2) * (3);\n"
        "  return m[\"GPU\"] + v[0].second + x - 26;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("m {{\"CPU\", 10}, {\"GPU\", 15}};") != std::string::npos);
    CHECK(result.cpp1.find("v {{1, 2}};") != std::string::npos);
    CHECK(result.cpp1.find("auto x {(1 + 2) * (3)};") != std::string::npos);
}

TEST_CASE("default arguments appear only where a function is first declared",
          "[pipeline][parameters][defaults][upstream-1305]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "P: type = {\n"
        "    public l: int = 0;\n"
        "    operator=: (implicit out this, l_: int = 1) = { l = l_; }\n"
        "    scaled: (this, k: int = 10) -> int = l * k;\n"
        "}\n"
        "add: (a: int, b: int = 5) -> int = a + b;\n"
        "sum: (items: std::vector<int> = (1, 2)) -> int = items.ssize();\n"
        "main: () -> int = { p: P = (); return add(1) + p.scaled() + sum() - 18; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("auto add(cpp2::impl::in<int> a, cpp2::impl::in<int> b = 5) -> int;") !=
          std::string::npos);
    CHECK(result.cpp1.find("auto add(cpp2::impl::in<int> a, cpp2::impl::in<int> b) -> int {") !=
          std::string::npos);
    CHECK(result.cpp1.find("items = {1, 2}) -> int;") != std::string::npos);
    CHECK(result.cpp1.find("P(cpp2::impl::in<int> l_ = 1)") != std::string::npos);
    CHECK(result.cpp1.find("auto scaled(cpp2::impl::in<int> k = 10) const& -> int") !=
          std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-default-arguments");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "default-arguments.cpp";
    test_support::write_file(cpp1_path,
        "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + result.cpp1);
    const auto compiler_result = test_support::run_process(
        {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO(compiler_result.output);
    CHECK_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status == 0);

    const auto misplaced = cppfrontier::translator{}.translate_source(
        "f: (implicit x: int) = { }\n");
    CHECK(misplaced.has_errors());
}

TEST_CASE("struct types are public aggregates", "[pipeline][metaclasses][struct]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "point: @struct type = {\n"
        "    x: i32 = 0;\n"
        "    y: i32 = 0;\n"
        "    len2: (this) -> i32 = x * x + y * y;\n"
        "}\n"
        "main: () -> int = { p: point = (3, 4); c := p; return c.len2() - 25; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("struct point {\nint x {0};\nint y {0};\n"
                           "[[nodiscard]] auto len2() const& -> int") != std::string::npos);
    CHECK(result.cpp1.find("point(") == std::string::npos);
    CHECK(result.cpp1.find("point p {3, 4};") != std::string::npos);

    const auto has = [](const std::string& source, std::string_view code) {
        const auto rejected = cppfrontier::translator{}.translate_source(source);
        return std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    CHECK(has("s: @struct type = { private x: int = 0; }\n", "struct-member-access"));
    CHECK(has("s: @struct type = { operator=: (out this) = { } }\n", "struct-operator-assign"));

    const auto templated = cppfrontier::translator{}.translate_source(
        "quantity: @struct <Number: type> type = { number: Number; }\n"
        "main: () -> int = { q: quantity<i32> = (5); return q.number - 5; }\n");
    for (const auto& diagnostic : templated.diagnostics) INFO(diagnostic.message);
    CHECK_FALSE(templated.has_errors());
    CHECK(templated.cpp1.find("template<typename Number> struct quantity { Number number;") !=
          std::string::npos);
}

TEST_CASE("discard expression bodies return nothing", "[pipeline][discard][upstream-572]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (x: int) = _ = x;\n"
        "g: (x) -> void = _ = x;\n"
        "main: () = { f(1); g(2); }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("{ return static_cast<void>( x); }") != std::string::npos);
    CHECK(result.cpp1.find("-> void { static_cast<void>( x); }") != std::string::npos);
}

TEST_CASE("deduced declarations use :== outside functions", "[pipeline][declarations]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "x: int = 5;\n"
        "y :== 7;\n"
        "main: () -> int = { return x + y - (12 as i32); }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("int x {5};") != std::string::npos);
    CHECK(result.cpp1.find("inline constexpr auto y = 7;") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::as_<cpp2::i32, 12>()") != std::string::npos);
}

TEST_CASE("a namespace-scope object needs a declared type unless it is a constant",
          "[pipeline][declarations][review]") {
    for (const auto* source : {"x := 5;\n", "x: _ = 5;\n", "N: namespace = { x: _ is std::integral = 5; }\n",
                               "v: <T> _ = T(4);\n"}) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        CAPTURE(source);
        CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.code == "namespace-scope-object-requires-type";
            }));
    }
    for (const auto* source : {"x :== 5;\n", "x: _ is std::integral == 5;\n", "x: int = 5;\n"}) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        CAPTURE(source);
        CHECK_FALSE(result.has_errors());
    }
}

TEST_CASE("unnamed function expressions are lambdas", "[pipeline][function-expressions]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () = {\n"
        "    n := 2;\n"
        "    c := std::count_if(v.begin(), v.end(), :(e) = e > n$;);\n"
        "    g := :() = { std::cout << n; };\n"
        "    h := :<T> (t: T) -> T = t;\n"
        "    k := :(a) = :(b) = a$ + b;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("std::count_if(CPP2_UFCS(begin)(v), CPP2_UFCS(end)(v), [_0 = cpp2::move(n)](auto const& e) mutable "
                           "-> decltype(auto) { return cpp2::impl::cmp_greater(e,_0); })") != std::string::npos);
    CHECK(result.cpp1.find("auto g {[]() -> void") != std::string::npos);
    CHECK(result.cpp1.find("[]<typename T>(T const& t) -> T") != std::string::npos);
    CHECK(result.cpp1.find("[](auto const& a) -> decltype(auto) { return [_0 = a](auto const& b) "
                           "mutable -> decltype(auto) { return _0 + b; }; }") != std::string::npos);
    CHECK(result.cpp1.find("__cpp2_function_expression") == std::string::npos);
}

TEST_CASE("a function expression body may end in a less-than comparison",
          "[pipeline][function-expressions][ports]") {
    // functions.md: `vec.std::ranges::sort( :(x,y) = y<x );`
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () = {\n"
        "    std::ranges::sort(v, :(x, y) = y<x);\n"
        "    c := std::count_if(v.begin(), v.end(), :(i: int) -> bool = i < 7);\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("{ return cpp2::impl::cmp_less(y,x); })") != std::string::npos);
    CHECK(result.cpp1.find("-> bool { return cpp2::impl::cmp_less(i,7); })};") != std::string::npos);
}

TEST_CASE("the address of a qualified member name is a pointer to member",
          "[pipeline][expressions][ports]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "person: @struct type = { age: int; }\n"
        "main: () = { p := person::age&; x := 1; q := x&; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("auto p {&person::age};") != std::string::npos);
    CHECK(result.cpp1.find("auto q {&(x)};") != std::string::npos);
}

TEST_CASE("function expressions need = before the body", "[pipeline][function-expressions][upstream-1096]") {
    const auto terse = cppfrontier::translator{}.translate_source(
        "f: (r, pred) -> bool = std::ranges::find_if(r, :(x) pred$(x)) != r.end();\n");
    CHECK(terse.has_errors());
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (r, pred) -> bool = std::ranges::find_if(r, :(x) = pred$(x)) != r.end();\n"
        "g: (p) -> int = p*();\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("[_0 = pred](auto const& x) mutable -> decltype(auto) { return _0(x); }") !=
          std::string::npos);
    CHECK(result.cpp1.find("(*cpp2::impl::assert_not_null(p))()") != std::string::npos);
}

TEST_CASE("is lowers types and values to cpp2::impl::is", "[pipeline][is]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (x: std::any) -> bool = x is i32 && !(x is std::string) && x is *std::vector<int>;\n"
        "g: (x: int) -> bool = x is -1 || x is (2) || x is 3 || x is _;\n"
        "h: (v) -> bool = v is (:(x: int) = x > 3;);\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("cpp2::impl::is<cpp2::i32>(x) && !(cpp2::impl::is<std::string>(x)) && "
                           "cpp2::impl::is<std::vector<int>*>(x)") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::is(x, -1) || cpp2::impl::is(x, (2)) || "
                           "cpp2::impl::is(x, 3) || true") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::is(v, ([](cpp2::impl::in<int> x)") != std::string::npos);
}

TEST_CASE("typed temporaries construct braced values", "[pipeline][typed-temporary]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "u: type == std::array<i32, 2>;\n"
        "t: @struct type = {\n"
        "    this: std::integral_constant<u, :u = (17, 29)>;\n"
        "}\n"
        "f: () -> i32 = { return :i32 = 0; }\n"
        "main: () = {\n"
        "    v: std::vector<int> = ();\n"
        "    v = (2, 3);\n"
        "    print(:std::vector<int> = (1, 2, 3), : std::string = \"s\");\n"
        "    (w := t()) { w[:() -> _ = 0]; }\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("std::integral_constant<u, u{17, 29}>") != std::string::npos);
    CHECK(result.cpp1.find("return cpp2::i32{0};") != std::string::npos);
    CHECK(result.cpp1.find("v = { 2, 3 };") != std::string::npos);
    CHECK(result.cpp1.find("print(std::vector<int>{1, 2, 3}, std::string{\"s\"})") !=
          std::string::npos);
    CHECK(result.cpp1.find("CPP2_ASSERT_IN_BOUNDS(w, ([]() -> auto { return 0; }))") !=
          std::string::npos);
}

TEST_CASE("string interpolations capture and expand raw strings", "[pipeline][interpolation]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () = {\n"
        "    x := 1;\n"
        "    s := $R\"d(a (x)$ b)d\";\n"
        "    f := :() -> std::string = \"x=(x$ + 1)$ (x$:>3)$\";\n"
        "    g := :() -> std::string = $R\"((x$)$!)\";\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("R\"d(a )d\" + cpp2::to_string(x) + R\"d( b)d\"") != std::string::npos);
    CHECK(result.cpp1.find("[_0 = x]() mutable -> std::string { return \"x=\" + "
                           "cpp2::to_string(_0 + 1) + \" \" + cpp2::to_string(_0,\"{:>3}\") + \"\"; }") !=
          std::string::npos);
    CHECK(result.cpp1.find("{ return cpp2::to_string(_0) + R\"(!)\"; }") != std::string::npos);
}

TEST_CASE("inspect expressions lower to immediately invoked lambdas", "[pipeline][inspect]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (x: _) -> std::string = {\n"
        "    return inspect x -> std::string {\n"
        "        is int = std::to_string(x as int);\n"
        "        is (in(1, 2)) = \"range\";\n"
        "        is _ = inspect constexpr 1 -> std::string { is (1) = \"one\"; is _ = \"\"; };\n"
        "    };\n"
        "}\n"
        "g: <T> () = { if constexpr std::is_integral_v<T> { } }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("return [&] () -> std::string { [[maybe_unused]] auto&& __cpp2_expr = x;") != std::string::npos);
    CHECK(result.cpp1.find(
              "if (cpp2::impl::is<int>(__cpp2_expr)) { if constexpr( requires{std::to_string("
              "cpp2::impl::as<int>(x));} ) if constexpr( std::is_convertible_v<CPP2_TYPEOF(("
              "std::to_string(cpp2::impl::as<int>(x)))),std::string> ) return std::to_string("
              "cpp2::impl::as<int>(x)); else return std::string{}; else return std::string{}; }") !=
          std::string::npos);
    CHECK(result.cpp1.find("else if (cpp2::impl::is(__cpp2_expr, (in(1, 2))))") != std::string::npos);
    CHECK(result.cpp1.find("else return [&] () -> std::string { [[maybe_unused]] constexpr auto&& __cpp2_expr = 1; "
                           "if constexpr (cpp2::impl::is(__cpp2_expr, (1)))") != std::string::npos);
    CHECK(result.cpp1.find("}();") != std::string::npos);
    CHECK(result.cpp1.find("if constexpr (std::is_integral_v<T>)") != std::string::npos);
}

TEST_CASE("inspect reports cppfront's restrictions", "[pipeline][inspect]") {
    const auto diagnosed = [](std::string_view body, std::string_view message) {
        const auto result = cppfrontier::translator{}.translate_source(
            "f: (x: int) = {\n" + std::string{body} + "\n}\n");
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) {
                return diagnostic.code == "invalid-inspect" &&
                    diagnostic.message.find(message) != std::string::npos;
            });
    };
    CHECK(diagnosed("inspect x -> int { is _ = 1; };", "only inspect expressions"));
    CHECK(diagnosed("a := inspect x { is _ = 1; };", "explicit '-> result_type'"));
    CHECK(diagnosed("a := inspect x -> int { is int = 1; };", "`is _` match-anything"));
    CHECK(diagnosed("a := inspect x -> int { };", "cannot be empty"));
    CHECK(diagnosed("a := inspect x -> int { as int = 1; is _ = 2; };", "simple 'is'"));
    CHECK(diagnosed("a := inspect x -> int { is int = { 1; } is _ = 2; };", "braced block"));
    CHECK_FALSE(diagnosed("a := inspect x -> int { is int = 1; is _ = 2; };", ""));
}

TEST_CASE("Cpp2 type names lower in expression and declaration positions", "[pipeline][type-names]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "v: <T> == 0;\n"
        "f: (r: _ is std::regular, s: _ is std::convertible_to<i32>) = {}\n"
        "main: () = {\n"
        "    a := v<i32> == v<i64>;\n"
        "    b := std::is_void_v<* i32> && std::is_void_v<const i32>;\n"
        "    c := sizeof(i16) + alignof(u8);\n"
        "    d := unchecked_narrow<i16>(a);\n"
        "    e := 3 as u8;\n"
        "    g := -3 as i8;\n"
        "    h := 1.5 as i32;\n"
        "    w: longdouble = 1.0;\n"
        "    x: const _ = 2;\n"
        "    p: * const _ = x&;\n"
        "    n: int = 1;\n"
        "    q: const * int = n&;\n"
        "    ok: _ is std::regular = 42;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("v<cpp2::i32> == v<cpp2::i64>") != std::string::npos);
    CHECK(result.cpp1.find("std::is_void_v<cpp2::i32*> && std::is_void_v<cpp2::i32 const >") !=
          std::string::npos);
    CHECK(result.cpp1.find("sizeof(cpp2::i16) + alignof(cpp2::u8)") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::unchecked_narrow<cpp2::i16>(cpp2::move(a))") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::as_<cpp2::u8, 3>()") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::as_<cpp2::i8, -3>()") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::as_<cpp2::i32>(1.5)") != std::string::npos);
    CHECK(result.cpp1.find("long double w {1.0};") != std::string::npos);
    CHECK(result.cpp1.find("auto const x {2};") != std::string::npos);
    CHECK(result.cpp1.find("auto const* p {&(x)};") != std::string::npos);
    CHECK(result.cpp1.find("int* const q {&(n)};") != std::string::npos);
    CHECK(result.cpp1.find("std::regular auto ok {42};") != std::string::npos);
    CHECK(result.cpp1.find("auto f(std::regular auto const& r, "
                           "std::convertible_to<int> auto const& s) -> void") != std::string::npos);
}

TEST_CASE("requires clauses follow return types and type declarations", "[pipeline][misc-fixes]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "X: <T: type> type requires std::is_same_v<T, int> = { }\n"
        "f: <T: type> (forward a: int) -> int requires std::is_same_v<T, int> = { return a; }\n"
        "main: () = { std::cout << f<int>(2); }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("template<typename T> requires (std::is_same_v<T, int>) struct X;") !=
          std::string::npos);
    CHECK(result.cpp1.find("-> int requires (std::is_same_v<T, int> && std::is_convertible_v<") !=
          std::string::npos);
}

TEST_CASE("function type-ids render reference parameters and forward returns", "[pipeline][misc-fixes]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "A: type == (inout s: std::string) -> forward std::string;\n"
        "main: () = {\n"
        "    f: std::function< (move s: std::string, in_ref t: int) -> void > = :(move s: std::string, in_ref t: int) = { };\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("using A = std::string&(std::string& s);") != std::string::npos);
    CHECK(result.cpp1.find("std::function<void(std::string&& s, int const& t)>") !=
          std::string::npos);
}

TEST_CASE("trailing commas are dropped from call arguments", "[pipeline][misc-fixes]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "g: <T, U, > (a: T, b: U) = a + b;\n"
        "main: () = { _ = g(1, g(2, 3,),); v: std::vector = (0, 1,); _ = v; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("g(1, g(2, 3))") != std::string::npos);
}

TEST_CASE("memberwise base copies cast that to the base", "[pipeline][misc-fixes]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Base: type = {\n"
        "  operator=: (out this) = { }\n"
        "  operator=: (out this, that) = std::cout << \"copy\\n\";\n"
        "  operator=: (implicit out this, _) = std::cout << \"any\\n\";\n"
        "}\n"
        "Derived: type = {\n"
        "  this: Base = ();\n"
        "  operator=: (out this) = { }\n"
        "  operator=: (out this, that) = { }\n"
        "  operator=: (inout this, move that) = { }\n"
        "}\n"
        "main: () = { d := Derived(); d2 := d; d2 = (move d); }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("Base([[maybe_unused]] Base const& that)  {\n    std::cout << \"copy\\n\";") !=
          std::string::npos);
    CHECK(result.cpp1.find(": Base{static_cast<Base const&>(that)}") != std::string::npos);
    CHECK(result.cpp1.find(": Base{static_cast<Base&&>(that)}") != std::string::npos);
    CHECK(result.cpp1.find("Base::operator=(static_cast<Base&&>(that));") != std::string::npos);
}

TEST_CASE("member functions without this are static", "[pipeline][misc-fixes]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "t: type = {\n"
        "    nested: type = { g: () = { } }\n"
        "    f: <T> (x: T) = x;\n"
        "    h: (this) = { }\n"
        "}\n"
        "main: () = { t::nested::g(); }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("static auto g() -> void") != std::string::npos);
    CHECK(result.cpp1.find("template<typename T> static auto f(") != std::string::npos);
    CHECK(result.cpp1.find("static auto h") == std::string::npos);
}

TEST_CASE("converting assignments set members in declaration order",
          "[pipeline][misc-fixes]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "t: type = {\n"
        "    v: std::vector<int> = (1, 2, 3);\n"
        "    s: std::string = ();\n"
        "    n: int = 5;\n"
        "    m: int = 6;\n"
        "    operator=: (implicit out this, x: int) = { n = x; }\n"
        "}\n"
        "main: () = { x: t = 1; x = 7; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("v = std::vector<int>{1, 2, 3}; s = std::string{}; n = x; m = 6; ") !=
          std::string::npos);
}

TEST_CASE("function parameter packs and fold expressions", "[pipeline][misc-fixes]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "all: <Args...: type> (args...: Args) -> bool =\n"
        "    // a comment between the signature and the body\n"
        "    (... && args);\n"
        "make: <T, Args...> (forward args...: Args) = :T = args...;\n"
        "pred: (_...) = true;\n"
        "main: () = { std::cout << all(true, false) << make<std::string>(\"ab\", 1) << pred(1, 2); }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("auto all(Args const& ...args) -> bool") != std::string::npos);
    CHECK(result.cpp1.find("auto make(Args&& ...args)") != std::string::npos);
    CHECK(result.cpp1.find("auto pred([[maybe_unused]] auto const& ...unnamed_param_1)") !=
          std::string::npos);
}

TEST_CASE("preconditions are checked on function entry", "[pipeline][misc-fixes]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "first: (forward rng) -> forward _\n"
        "  pre<bounds_safety>( !std::empty(rng) )\n"
        "= std::begin(rng)*;\n"
        "f: (x: int) -> int pre(x > 0, \"positive\") = { return x; }\n"
        "main: () = { v: std::vector = (1, 2); _ = first(v); _ = f(1); }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("if (cpp2::bounds_safety.is_active() && !(!std::empty(rng)) ) "
                           "{ cpp2::bounds_safety.report_violation(\"\"); }") !=
          std::string::npos);
    CHECK(result.cpp1.find("auto f(cpp2::impl::in<int> x) -> int  {\n#line 4\n    if (cpp2::cpp2_default"
                           ".is_active() && !(cpp2::impl::cmp_greater(x,0)) ) { cpp2::cpp2_default.report_violation("
                           "CPP2_CONTRACT_MSG(\"positive\")); }\n#line 4\n    return x;") != std::string::npos);
}

TEST_CASE("template defaults and statement lambdas", "[pipeline][misc-fixes]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "myclass: <T: type = int> type = { nested: <U: type = int> type = { } }\n"
        "myfunc: <T: type = int> (fn: *const char = \"x\") = { _ = fn; }\n"
        "main: () = {\n"
        "    : <TT: type = int> (MM: int = 43) = { _ = MM; };\n"
        "    : <V: bool = true> () = {\n"
        "        if V { for (2 ..< 5) do (e) { std::cout << e; } }\n"
        "    } ();\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("template<typename T = int> struct myclass;") != std::string::npos);
    CHECK(result.cpp1.find("template<typename T> struct myclass {") != std::string::npos);
    CHECK(result.cpp1.find("template<typename U = int> struct nested {") != std::string::npos);
    CHECK(result.cpp1.find("template<typename T = int> auto myfunc(cpp2::impl::in<char const*> "
                           "fn = \"x\") -> void;") != std::string::npos);
    CHECK(result.cpp1.find("template<typename T> auto myfunc(cpp2::impl::in<char const*> fn) "
                           "-> void") != std::string::npos);
    CHECK(result.cpp1.find("[]<typename TT = int>(cpp2::impl::in<int> MM = 43) -> void") !=
          std::string::npos);
}

TEST_CASE("final types, defaulted comparisons, and local aliases", "[pipeline][misc-fixes]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "widget: final type = {\n"
        "    v: int;\n"
        "    operator=: (out this, value: int) = { v = value; }\n"
        "    operator==: (this, that) -> bool;\n"
        "    operator<=>: (this, that) -> std::strong_ordering;\n"
        "}\n"
        "f: () = { }\n"
        "g: (out x: int) -> int = { x = 1; return 2; }\n"
        "main: () = {\n"
        "    N4: namespace == std::literals;\n"
        "    f2 :== f;\n"
        "    f2();\n"
        "    y: int;\n"
        "    z: int = g(out y);\n"
        "    _ = y + z;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("struct widget final {") != std::string::npos);
    CHECK(result.cpp1.find("auto operator==([[maybe_unused]] widget const& that) const& -> bool = default;") !=
          std::string::npos);
    CHECK(result.cpp1.find("-> std::strong_ordering = default;") != std::string::npos);
    CHECK(result.cpp1.find("namespace N4 = std::literals;") != std::string::npos);
    CHECK(result.cpp1.find("constexpr auto f2 {f};") != std::string::npos);
}

namespace {

// "line:column: message" for each error diagnostic.
auto error_lines(const std::string& source) -> std::vector<std::string> {
    const auto result = cppfrontier::translator{}.translate_source(source);
    auto lines = std::vector<std::string>{};
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.severity != cppfrontier::diagnostic_severity::error) continue;
        lines.push_back(std::to_string(diagnostic.span.begin.line) + ":" +
                        std::to_string(diagnostic.span.begin.column) + ": " + diagnostic.message);
    }
    return lines;
}

auto has_error(const std::vector<std::string>& lines, const std::string& expected) -> bool {
    for (const auto& line : lines) INFO(line);
    return std::find(lines.begin(), lines.end(), expected) != lines.end();
}

}

TEST_CASE("pointer safety rejects null assignment and arithmetic", "[diagnostics-parity]") {
    const auto lines = error_lines(
        "fun: (inout i: int) -> *int = { return i&; }\n"
        "main: () = {\n"
        "    a: int = 2;\n"
        "    pa: *int = a&;\n"
        "    ppa: **int = pa&;\n"
        "    pa = 0;\n"
        "    pa2 := ppa*;\n"
        "    pa2 += 2;\n"
        "    pa3 := pa2;\n"
        "    pa3 = nullptr;\n"
        "    fun(a)++;\n"
        "    _ = pa + 1;\n"
        "    _ = pa[1];\n"
        "    if pa == pa2 { a = 3; }\n"
        "}\n");
    CHECK(has_error(lines, "6:8: = - pointer assignment from null or integer is illegal"));
    CHECK(has_error(lines, "8:9: += - pointer assignment from null or integer is illegal"));
    CHECK(has_error(lines, "10:9: = - pointer assignment from null or integer is illegal"));
    CHECK(has_error(lines,
        "11:11: ++ - pointer arithmetic is illegal - use std::span or gsl::span instead"));
    CHECK(has_error(lines,
        "12:12: + - pointer arithmetic is illegal - use std::span or gsl::span instead"));
    CHECK(has_error(lines,
        "13:11: [ - pointer arithmetic is illegal - use std::span or gsl::span instead"));
    CHECK(lines.size() == 6);
}

TEST_CASE("pointer declarations cannot be initialized to null", "[diagnostics-parity]") {
    const auto lines = error_lines("g: *int = nullptr;\nh: (p: *int = 0) = {}\n");
    const auto message = std::string{"pointer cannot be initialized to null or int - leave it "
        "uninitialized and then set it to a non-null value when you have one"};
    CHECK(has_error(lines, "1:11: " + message));
    CHECK(has_error(lines, "2:15: " + message));
}

TEST_CASE("upstream parse diagnostics for invalid declarations", "[diagnostics-parity]") {
    CHECK(has_error(error_lines("main: () = {\n  using\n}\n"),
                    "3:1: expected valid id-expression after 'using'"));
    CHECK(has_error(error_lines("main: () { }\n"),
                    "1:10: expected '=' before '{' - did you mean '= {' ?"));
    CHECK(has_error(error_lines("print: (inout out: std::ostream = args: int) = {}\n"),
                    "1:44: parameter must be initialized with an expression"));
    CHECK(has_error(error_lines("t: type = {\n    x: requires true == 42;\n}\n"),
                    "2:5: invalid alias declaration - expected 'type', 'namespace', or a "
                    "type-id after ':'"));
    CHECK(has_error(error_lines("b: type = {}\nt: type = {\n  f: (this) = {}\n  this: b;\n}\n"),
                    "4:3: a type cannot declare a parent after defining a function"));
    CHECK(has_error(error_lines("f: () -> decltype(auto) = 0;\n"),
                    "1:10: decltype(auto) is not needed in Cpp2 - for return types, use "
                    "'-> forward _' instead"));
    CHECK(has_error(error_lines("f: (p: *int) = { delete p; }\n"),
                    "1:18: 'delete' and owning raw pointers are not supported in Cpp2 - use "
                    "unique.new<T> or shared.new<T> instead in that order (or, in the future, "
                    "gc.new<T>, but that is not yet implemented)"));
    CHECK(has_error(error_lines("#else\n#else\nmain: () = {}\n"),
                    "1:1: #else does not match a prior #if"));
}

TEST_CASE("forward returns cannot return references into locals or by-value parameters",
          "[diagnostics-parity]") {
    const auto message = std::string{cppfrontier::forward_return_message};
    // `-> forward _` deduces a by-value return for a name that is not a reference.
    CHECK(error_lines("f: () -> forward _ = { i: int = 42; return i; }\n"
                      "g: (x: int) -> forward _ = { return x; }\n").empty());
    CHECK(has_error(error_lines("f: (copy x: int) -> forward int = x;\n"), "1:35: " + message));
    CHECK(error_lines("g: int = 1;\nf: (inout x: int) -> forward int = { return x; }\n"
                      "h: () -> forward int = { return g; }\n"
                      "k: (inout x: int) -> forward int = x;\n").empty());

    // Member accesses, subscripts, calls and parentheses refer into the leading name.
    const auto local = std::string{
        "t: type = { public x: int = 0; get: (inout this) -> forward int = x; }\n"
        "f: () -> forward int = {\n    s: t = ();\n    v: std::vector<int> = ();\n    "};
    for (const auto* body : {"return s;", "return s.x;", "return v[0];", "return s.get();",
                             "return (s).x;"}) {
        CAPTURE(body);
        CHECK(has_error(error_lines(local + body + "\n}\n"), "5:5: " + message));
    }
    CHECK(has_error(error_lines("f: (copy p: std::pair<int, int>) -> forward int = p.first;\n"),
                    "1:51: " + message));
    // A postfix `*` dereferences, leaving the local or parameter.
    CHECK(error_lines("f: (inout m: std::map<int, std::string>) -> forward std::string = {\n"
                      "    it := m.begin();\n    return it*.second;\n}\n"
                      "g: (p: *std::pair<int, int>) -> forward int = (p*).first;\n"
                      "h: (p: *int) -> forward int = p*;\n").empty());
}

TEST_CASE("branch initialization mismatch is reported at the declaration",
          "[diagnostics-parity]") {
    const auto lines = error_lines(
        "main: () = {\n    x: int;\n    if true { x = 1; }\n    else { }\n    _ = x;\n}\n");
    CHECK(has_error(lines,
        "2:5: local variable x must be initialized on both branches or neither branch"));
    CHECK(has_error(lines, "3:5: \"if\" initializes x on one branch but not the other"));
    CHECK(lines.size() == 2);
}

TEST_CASE("local namespace and deduced object aliases lower", "[diagnostics-parity]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: () = 1;\nmain: () = {\n    N: namespace == std::literals;\n    g :== f;\n}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("namespace N = std::literals;") != std::string::npos);
    CHECK(result.cpp1.find("constexpr auto g {f};") != std::string::npos);
}

namespace {

// Compiles and runs `cpp1` against upstream's cpp2util.h, returning its output.
auto run_mixed_program(const std::string& cpp1, const std::string& name) -> std::string {
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-" + name);
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (name + ".cpp");
    const auto executable_path = scratch / name;
    test_support::write_file(cpp1_path, cpp1);
    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
        scratch, std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);
    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    REQUIRE_FALSE(run_result.timed_out);
    return run_result.output;
}

}

TEST_CASE("Cpp1 aliases in Cpp1 scopes stay Cpp1 and shadow fixed-width names", "[mixed]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "#include <iostream>\n"
        "namespace my {\n"
        "    using u16 = float;\n"
        "}\n"
        "using word = int;\n"
        "holder: @struct <T> type = { flag: <U> bool == true; }\n"
        "main: () = {\n"
        "    y: my::u16 = 42;\n"
        "    z: u16 = 42;\n"
        "    std::cout << y << z << holder<int>::flag<int>;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("    using u16 = float;") != std::string::npos);
    CHECK(result.cpp1.find("using word = int;") != std::string::npos);
    CHECK(result.cpp1.find("my::u16 y {42};") != std::string::npos);
    CHECK(result.cpp1.find("unsigned short z {42};") != std::string::npos);
    CHECK(result.cpp1.find("template<typename U> static constexpr bool flag {true};") !=
          std::string::npos);
}

TEST_CASE("subscripts and postfix dereferences are checked unless disabled", "[mixed]") {
    const auto source = std::string{
        "f: (v: std::vector<int>, i: int, p: *int) -> int = v[i] + v[0] + p* + v[0, 1];\n"};
    const auto checked = cppfrontier::translator{}.translate_source(source);
    INFO(checked.cpp1);
    REQUIRE_FALSE(checked.has_errors());
    CHECK(checked.cpp1.find("CPP2_ASSERT_IN_BOUNDS(v, i) + CPP2_ASSERT_IN_BOUNDS_LITERAL(v, 0) + "
                            "*cpp2::impl::assert_not_null(p) + v[0, 1]") != std::string::npos);

    auto options = cppfrontier::translation_options{};
    options.check_subscripts = false;
    options.check_null_dereference = false;
    const auto unchecked = cppfrontier::translator{}.translate_source(source, options);
    INFO(unchecked.cpp1);
    CHECK(unchecked.cpp1.find("v[i] + v[0] + *p + v[0, 1]") != std::string::npos);
}

TEST_CASE("mixed programs keep source lines, chained comparisons, and contracts", "[mixed]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "#include <iostream>\n"
        "#include <vector>\n"
        "struct X { int i; };\n"
        "vec: std::vector<int> = ();\n"
        "insert_at: (where: int, val: int)\n"
        "    pre( 0 <= where <= vec.ssize(), \"where is in range\" )\n"
        "    post( vec.ssize() == vec.ssize()$ + 1 )\n"
        "= {\n"
        "    vec.push_back(val);\n"
        "}\n"
        "make: () -> (s: std::string = \"ab\") post (s.length() == s.length()$ + 1) = {\n"
        "    s += \"c\";\n"
        "}\n"
        "twice: (x: X) -> int = x.i * 2;\n"
        "thrower: () throws = { }\n"
        "main: () -> int = {\n"
        "    insert_at(0, 42);\n"
        "\n"
        "    std::cout << vec[0] << make() << twice(X{2}) << (1 < 2 <= 2) << (3 >= 3 > 1);\n"
        "    thrower();\n"
        "    v: std::vector<int> = (1);\n"
        "\n"
        "    std::cout << v[3];\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("cpp2_finally_presuccess.add([&, _0 = CPP2_UFCS(ssize)(vec)]{") !=
          std::string::npos);
    CHECK(result.cpp1.find("[_0 = 0, _1 = where, _2 = CPP2_UFCS(ssize)(vec)]{ return "
                           "cpp2::impl::cmp_less_eq(_0,_1) && cpp2::impl::cmp_less_eq(_1,_2); }()") !=
          std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::cmp_greater_eq(_0,_1) && cpp2::impl::cmp_greater(_1,_2)") !=
          std::string::npos);
    CHECK(result.cpp1.find("cpp2_finally_presuccess.run(); return s;") != std::string::npos);
    // `twice` names the Cpp1 type X, so it is declared after X.
    CHECK(result.cpp1.find("struct X") < result.cpp1.find("auto twice("));
    const auto output = run_mixed_program(result.cpp1, "mixed-contracts");
    CHECK(output.starts_with("42abc411"));
    // The bounds violation reports the source line of `v[3]`.
    CHECK(output.find("(23) int main(): Bounds safety violation") != std::string::npos);
}

TEST_CASE("forward arguments, member calls, out parameters, and increments lower as upstream",
          "[mixed]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "#include <iostream>\n"
        "#include <utility>\n"
        "struct X {\n"
        "    X() { std::cout << \"+X \"; }\n"
        "    X(X const&) { std::cout << \"copy \"; }\n"
        "};\n"
        "pick: (x: int) -> int = x;\n"
        "first: (forward p: std::pair<int, int>) -> int = pick(forward p.first);\n"
        "make: (out x: X) = { x = (); }\n"
        "set: (out x: _) = { x = 42; }\n"
        "counter: @value type = {\n"
        "    public n: int = 0;\n"
        "    operator++: (inout this) -> forward counter = { n++; return this; }\n"
        "}\n"
        "auto bump(counter& c) -> int { return (c++).n; }\n"
        "main: () = {\n"
        "    x: X;\n"
        "    make(out x);\n"
        "    a: int;\n"
        "    set(out a);\n"
        "    v: std::vector<int> = ();\n"
        "    v..push_back(a);\n"
        "    c: counter = ();\n"
        "    old := bump(c);\n"
        "    std::cout << first(std::pair(1, 2)) << v..size() << old << c.n << (10LL as i32);\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("pick(CPP2_FORWARD(p).first)") != std::string::npos);
    CHECK(result.cpp1.find("x.construct();") != std::string::npos);
    CHECK(result.cpp1.find("auto set(auto x_) -> void") != std::string::npos);
    CHECK(result.cpp1.find("v.push_back(cpp2::move(a.value()));") != std::string::npos);
    CHECK(result.cpp1.find("{ auto ret = *this; ++*this; return ret; }") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::as_<cpp2::i32, 10LL>()") != std::string::npos);
    CHECK(run_mixed_program(result.cpp1, "mixed-forwarding") == "+X 110110");
}

TEST_CASE("a capture of sizeof... takes the whole pack size", "[pipeline][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "count: (args...: int) -> int = :() -> int = sizeof...(args)$ + 1;();\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("[_0 = sizeof...(args)]() mutable -> int { return _0 + 1; }()") !=
          std::string::npos);
}

TEST_CASE("function expressions inside string interpolations become lambdas",
          "[pipeline][interpolation]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "#include <iostream>\n"
        "main: () = {\n"
        "    y := 10;\n"
        "    std::cout << \"(:(x) = x + 1;(2))$ (:(x) = x + y$;(2))$ (:() = 42;():>4)$\\n\";\n"
        "    f := :() = { std::cout << \"(:(x) = x * 3;(1))$ (:(x) = x;(y$))$\\n\"; };\n"
        "    f();\n"
        "    std::cout << $R\"(raw (:(x) = x * 2;(4))$ \\n)\" << \"\\n\";\n"
        "    std::cout << \"(std::string(\\\"a\\\") + 'b')$('\\n')$\";\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("\"\" + cpp2::to_string([](auto const& x) -> decltype(auto) "
                           "{ return x + 1; }(2)) + \" \" + cpp2::to_string([_0 = y](auto const& x) "
                           "mutable -> decltype(auto) { return x + _0; }(2))") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::to_string([]() -> decltype(auto) { return 42; }(),\"{:>4}\")") !=
          std::string::npos);
    // A `y$` outside the inner lambda is a capture of `f`.
    CHECK(result.cpp1.find("cpp2::to_string([](auto const& x) -> decltype(auto) { return x; }(_0))") !=
          std::string::npos);
    CHECK(result.cpp1.find("R\"(raw )\" + cpp2::to_string([](auto const& x) -> decltype(auto) "
                           "{ return x * 2; }(4)) + R\"( \\n)\"") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::to_string(std::string(\"a\") + 'b') + cpp2::to_string('\\n')") !=
          std::string::npos);
    CHECK(run_mixed_program(result.cpp1, "interpolated-lambdas") == "3 12   42\n3 10\nraw 8 \\n\nab\n");
}

TEST_CASE("a labeled break or continue may skip a local's initialization",
          "[pipeline][loops][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "#include <iostream>\n"
        "main: () = {\n"
        "    i := 0;\n"
        "    outer: while i < 3 next i++ {\n"
        "        if i == 1 { continue outer; }\n"
        "        x: int = i * 10;\n"
        "        std::cout << x << \" \";\n"
        "        if i == 2 { break outer; }\n"
        "        y: int = x + 1;\n"
        "        std::cout << y << \" \";\n"
        "    }\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(run_mixed_program(result.cpp1, "labeled-jumps") == "0 1 20 ");
}

namespace {

auto last_use_output(std::string_view source) -> std::string {
    const auto result = cppfrontier::translator{}.translate_source(std::string{source});
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    return result.cpp1;
}

}

TEST_CASE("definite last uses of locals and parameters move or forward", "[last-use]") {
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "f: (copy a: int, move b: int, forward c: _, in d: int, inout e: int) = {\n"
        "    g(a); g(a); g(b); g(c); g(d); g(e);\n"
        "    x := 1; g(x);\n"
        "    guard_y := 2; g(guard_y);\n"
        "    s: std::string = \"s\"; std::cout << \"(s)$\";\n"
        "}\n"
        "r: (copy p: int) -> int = { y := p; return y; }\n"
        "t: (copy p: int) -> int = p;\n");
    INFO(cpp1);
    CHECK(cpp1.find("g(a);\n#line 3\n    g(cpp2::move(a));") != std::string::npos);
    CHECK(cpp1.find("g(cpp2::move(b));") != std::string::npos);
    CHECK(cpp1.find("g(CPP2_FORWARD(c));") != std::string::npos);
    CHECK(cpp1.find("g(d);") != std::string::npos);
    CHECK(cpp1.find("g(e);") != std::string::npos);
    CHECK(cpp1.find("g(cpp2::move(x));") != std::string::npos);
    CHECK(cpp1.find("g(guard_y);") != std::string::npos);
    CHECK(cpp1.find("cpp2::to_string(CPP2_FORWARD(s))") != std::string::npos);
    // A returned local or parameter is left to Cpp1's implicit move.
    CHECK(cpp1.find("auto y {cpp2::move(p)};\n#line 8\n    return y;") != std::string::npos);
    CHECK(cpp1.find("{ return p; }") != std::string::npos);
}

TEST_CASE("a move parameter lowered to a forwarding reference takes only rvalues",
          "[last-use][hunt]") {
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "a: (move x: _) = g(x);\n"
        "b: <T> (move x: T) = g(x);\n"
        "c: (move xs...: std::string) = (g(xs), ...);\n"
        "d: (move x: std::string) = g(x);\n"
        "e: <T> (move x: std::vector<T>) = g(x);\n"
        "S: type = { operator=: (out this, move that) = { } }\n");
    INFO(cpp1);
    CHECK(cpp1.find("auto a(auto&& x) -> decltype(auto) requires (!std::is_lvalue_reference_v<decltype(x)>)") !=
          std::string::npos);
    CHECK(cpp1.find("auto b(T&& x) -> decltype(auto) requires (!std::is_lvalue_reference_v<decltype(x)>)") !=
          std::string::npos);
    CHECK(cpp1.find("(!std::is_lvalue_reference_v<decltype(xs)> && ...)") != std::string::npos);
    CHECK(cpp1.find("auto d(std::string&& x) -> decltype(auto) {") != std::string::npos);
    CHECK(cpp1.find("decltype(that)") == std::string::npos);
    CHECK(cpp1.find("std::vector<T>&& x) -> decltype(auto) {") != std::string::npos);
}

TEST_CASE("last uses follow branches and stop at loops", "[last-use]") {
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "branches: (copy x: int, copy y: int) = {\n"
        "    g(y);\n"
        "    if x > 0 { g(x); } else { g(x); g(x); g(y); }\n"
        "}\n"
        "loops: (copy x: int, copy v: std::vector<int>) = {\n"
        "    g(x);\n"
        "    while true { g(x); }\n"
        "    for v do (e) { g(e); }\n"
        "}\n"
        "inner: () = { while true { z := 1; g(z); } }\n");
    INFO(cpp1);
    CHECK(cpp1.find("g(y);\n    if (cpp2::impl::cmp_greater(x,0)) {\n#line 4\n"
                    "        g(cpp2::move(x));") != std::string::npos);
    CHECK(cpp1.find("g(x);\n#line 4\n        g(cpp2::move(x));\n#line 4\n"
                    "        g(cpp2::move(y));") !=
          std::string::npos);
    CHECK(cpp1.find("g(x);\n    while (true) {\n#line 8\n        g(x);") != std::string::npos);
    CHECK(cpp1.find("for (auto const& e : CPP2_FORWARD(v))") != std::string::npos);
    CHECK(cpp1.find("g(cpp2::move(z));") != std::string::npos);
}

TEST_CASE("last uses as lvalues or shared operands are not moved", "[last-use]") {
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "S: type = { public n: int = 0; }\n"
        "f: (copy a: int, copy b: int, copy c: S, copy d: int, copy e: int, forward h: _) = {\n"
        "    a = 1;\n"
        "    g(b + b);\n"
        "    c.n = 2;\n"
        "    g(d&);\n"
        "    e++;\n"
        "    g(forward h);\n"
        "}\n"
        "m: (copy s: S) = { g(s.n); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("    a = 1;") != std::string::npos);
    CHECK(cpp1.find("g(b + b);") != std::string::npos);
    CHECK(cpp1.find("    c.n = 2;") != std::string::npos);
    CHECK(cpp1.find("g(&(d));") != std::string::npos);
    CHECK(cpp1.find("++e;") != std::string::npos);
    CHECK(cpp1.find("g(CPP2_FORWARD(h));") != std::string::npos);
    // A member access moves its receiver.
    CHECK(cpp1.find("g(cpp2::move(s).n);") != std::string::npos);
}

TEST_CASE("captures and move-this members move from their last use", "[last-use]") {
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "S: type = {\n"
        "    n: int = 0;\n"
        "    f: (move this) = { g(n); g(n); }\n"
        "}\n"
        "c: (copy x: int, copy y: int) = {\n"
        "    l := :() = { g(x$); };\n"
        "    l();\n"
        "    g(y);\n"
        "    k := :() = { g(y$); };\n"
        "    k();\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("g(n);\n#line 4\n    g(cpp2::move(*this).n);") != std::string::npos);
    CHECK(cpp1.find("[_0 = cpp2::move(x)]") != std::string::npos);
    CHECK(cpp1.find("cpp2::move(l)();") != std::string::npos);
    CHECK(cpp1.find("g(y);") != std::string::npos);
    CHECK(cpp1.find("[_0 = cpp2::move(y)]") != std::string::npos);
}

TEST_CASE("moves from last uses compile and run with move-only types", "[last-use][mixed]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <memory>\n"
        "take: (copy p: std::unique_ptr<int>) -> int = p*;\n"
        "pass: (forward p: _) -> int = take(p);\n"
        "main: () = {\n"
        "    p := std::make_unique<int>(4);\n"
        "    q := std::make_unique<int>(2);\n"
        "    if p* > 0 { std::cout << take(p); } else { std::cout << pass(p); }\n"
        "    f := :() -> int = q$*;\n"
        "    std::cout << f();\n"
        "}\n");
    CHECK(run_mixed_program(cpp1, "last-use") == "42");
}

TEST_CASE("member calls lower to UFCS macros", "[ufcs]") {
    const auto cpp1 = last_use_output(
        "ns: namespace = { g: (x: int) -> int = x; h: <T> (x: int) -> int = x; }\n"
        "t: type = {\n"
        "    f: (this) -> int = 1;\n"
        "    g: (this) -> int = this.f() + f();\n"
        "    s: (this, o: t) -> int = o.f() + o..f();\n"
        "}\n"
        "main: () = {\n"
        "    x: std::vector<int> = (1, 2);\n"
        "    x.push_back(3);\n"
        "    a := x.front<int>();\n"
        "    b := x.ns::g();\n"
        "    c := x.ns::h<int>();\n"
        "    d := x.begin()*.foo(1, 2,);\n"
        "    e := x.data;\n"
        "    p := unique.new<int>(1);\n"
        "    std::cout << a << b << c << d << e << p << x.size();\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("return CPP2_UFCS(f)((*this)) + f();") != std::string::npos);
    CHECK(cpp1.find("CPP2_UFCS(f)(o) + o.f()") != std::string::npos);
    CHECK(cpp1.find("CPP2_UFCS(push_back)(x, 3);") != std::string::npos);
    CHECK(cpp1.find("CPP2_UFCS_TEMPLATE(front<int>)(x)") != std::string::npos);
    CHECK(cpp1.find("ns::g(x)") != std::string::npos);
    CHECK(cpp1.find("ns::h<int>(x)") != std::string::npos);
    CHECK(cpp1.find("CPP2_UFCS(foo)((*cpp2::impl::assert_not_null(CPP2_UFCS(begin)(x))), 1, 2)") !=
          std::string::npos);
    CHECK(cpp1.find("auto e {x.data};") != std::string::npos);
    CHECK(cpp1.find("CPP2_UFCS_TEMPLATE(cpp2_new<int>)(cpp2::unique, 1)") != std::string::npos);
    CHECK(cpp1.find("CPP2_UFCS(size)(CPP2_FORWARD(x))") != std::string::npos);
}

TEST_CASE("UFCS outside function bodies uses the nonlocal macros", "[ufcs]") {
    const auto cpp1 = last_use_output(
        "v: std::vector<int> = (1, 2);\n"
        "n: int = v.size();\n"
        "_: int = v.size();\n"
        "f: (a: int = v.size()) = { }\n"
        "g: <T> (x: T) -> int requires (x.size() > 0) == 0;\n"
        "t: type = { m: int = v.size(); a: int == v.size(); }\n"
        "main: () = { }\n");
    INFO(cpp1);
    CHECK(cpp1.find("extern int n;") != std::string::npos);
    CHECK(cpp1.find("int n {CPP2_UFCS_NONLOCAL(size)(v)};") != std::string::npos);
    CHECK(cpp1.find("int auto_3_1 {CPP2_UFCS_NONLOCAL(size)(v)};") != std::string::npos);
    CHECK(cpp1.find("a = CPP2_UFCS_NONLOCAL(size)(v)") != std::string::npos);
    CHECK(cpp1.find("requires ((cpp2::impl::cmp_greater(CPP2_UFCS_NONLOCAL(size)(x),0)))") != std::string::npos);
    CHECK(cpp1.find("int m {CPP2_UFCS(size)(v)};") != std::string::npos);
    CHECK(cpp1.find("a {CPP2_UFCS_NONLOCAL(size)(v)}") != std::string::npos);
}

TEST_CASE("a local's initializer calls its member", "[ufcs]") {
    const auto cpp1 = last_use_output(
        "t: @struct type = { f: (this) -> int = 0; }\n"
        "main: () = {\n"
        "    f := t().f();\n"
        "    (g := t().g()) { }\n"
        "    h := :() = { _ = t().h(); };\n"
        "    k := :(k) = { _ = t().k(); };\n"
        "    m := :() = { using ns::m; _ = t().m(); };\n"
        "    n: int = t().n();\n"
        "    std::cout << f << h << k << m << n;\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("auto f {t().f()};") != std::string::npos);
    CHECK(cpp1.find("g{t().g()}") != std::string::npos);
    CHECK(cpp1.find("static_cast<void>(t().h());") != std::string::npos);
    CHECK(cpp1.find("static_cast<void>(CPP2_UFCS(k)(t()));") != std::string::npos);
    CHECK(cpp1.find("static_cast<void>(CPP2_UFCS(m)(t()));") != std::string::npos);
    CHECK(cpp1.find("int n {t().n()};") != std::string::npos);
}

TEST_CASE("UFCS moves a receiver or function name at its last use", "[ufcs][last-use]") {
    const auto cpp1 = last_use_output(
        "f: (copy r: std::string, copy size: int) = { _ = r.size(); }\n"
        "g: (copy s: std::string) -> _ = s.substr(1);\n");
    INFO(cpp1);
    CHECK(cpp1.find("CPP2_UFCS_MOVE(size)(CPP2_FORWARD(r))") != std::string::npos);
    CHECK(cpp1.find("CPP2_UFCS(substr)(CPP2_FORWARD(s), 1)") != std::string::npos);
}

TEST_CASE("UFCS calls members and falls back to free functions", "[ufcs][mixed]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <vector>\n"
        "twice: (x: int) -> int = 2 * x;\n"
        "sum: (v: std::vector<int>) -> int = { s := 0; for v do (e) { s += e; } return s; }\n"
        "t: @struct type = { n: int = 5; get: (this) -> int = n; }\n"
        "v: std::vector<int> = (1, 2, 3);\n"
        "total: int = v.sum();\n"
        "main: () = {\n"
        "    x := 21;\n"
        "    std::cout << x.twice() << ' ' << total << ' ' << t().get() << ' ' << 0.twice();\n"
        "    std::cout << ' ' << v.ssize() << '\\n';\n"
        "}\n");
    CHECK(run_mixed_program(cpp1, "ufcs") == "42 6 5 0 3\n");
}

TEST_CASE("prefixed and raw string literals keep their encoding prefixes", "[misc-remaining]") {
    const auto cpp1 = last_use_output(
        "main: () = {\n"
        "    a := u8\"u8\\\"\";\n"
        "    b := uR\"(uR\")\";\n"
        "    c := u8R\"x(u8R\")x\";\n"
        "    d := L\"L\";\n"
        "    f(a, b, c, d);\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("auto a {u8\"u8\\\"\"};") != std::string::npos);
    CHECK(cpp1.find("auto b {uR\"(uR\")\"};") != std::string::npos);
    CHECK(cpp1.find("auto c {u8R\"x(u8R\")x\"};") != std::string::npos);
    CHECK(cpp1.find("auto d {L\"L\"};") != std::string::npos);
}

TEST_CASE("for ranges may be function expressions and next clauses may discard",
          "[misc-remaining]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <vector>\n"
        "main: () = {\n"
        "    v: std::vector<int> = (1, 2);\n"
        "    for :() = v$;() next _ = :(forward x) = x;(v) do (i) std::cout << i;\n"
        "    for v next _ = 0 do (j) std::cout << j;\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("for (auto const& i : [_0 = v]() mutable -> decltype(auto) { return _0; }())") !=
          std::string::npos);
    CHECK(cpp1.find("static_cast<void>([](auto&& x) -> decltype(auto) { return CPP2_FORWARD(x); }(v));") !=
          std::string::npos);
    CHECK(cpp1.find("static_cast<void>(0);") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "for-function-range") == "1212");
}

TEST_CASE("requires clauses map Cpp2 fixed-width type names", "[misc-remaining]") {
    const auto cpp1 = last_use_output(
        "v: <T> const T requires std::same_as<T, i32> = 0;\n"
        "f: <T> (x: T) -> T requires std::is_same_v<T, u8> = x;\n"
        "X: <T> type requires std::is_same_v<T, i64> = { }\n");
    INFO(cpp1);
    CHECK(cpp1.find("requires (std::same_as<T, cpp2::i32>) T const v {0};") != std::string::npos);
    CHECK(cpp1.find("requires (std::is_same_v<T, cpp2::u8>)") != std::string::npos);
    CHECK(cpp1.find("requires (std::is_same_v<T, cpp2::i64>) struct X") != std::string::npos);
}

TEST_CASE("typed temporaries of fixed-width types name the cpp2 alias", "[misc-remaining]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "main: () = { std::cout << (:u8 = 3) + (:longdouble = 0.5) << (:std::string = \"x\"); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("(cpp2::u8{3}) + (cpp2::longdouble{0.5})") != std::string::npos);
    CHECK(cpp1.find("std::string{\"x\"}") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "fixed-typed-temporary") == "3.5x");
}

TEST_CASE("function expressions in non-local declarations", "[misc-remaining]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "v: <T> concept = :() -> bool = true;();\n"
        "u: type == decltype(:() = {});\n"
        "t: @struct type = {\n"
        "    this: decltype(:() -> int = 7;);\n"
        "    n: int = :() -> int = 3;();\n"
        "}\n"
        "main: () = { x: t = (); std::cout << x() << x.n << v<int>; }\n");
    INFO(cpp1);
    CHECK(cpp1.find("template<typename T> concept v = []() -> bool { return true; }();") !=
          std::string::npos);
    CHECK(cpp1.find("struct t : public decltype([]() -> int { return 7; })") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "non-local-function-expression") == "731");
}

TEST_CASE("pure2-last-use lowering details", "[misc-remaining][last-use]") {
    const auto cpp1 = last_use_output(
        "#include <array>\n"
        "#include <memory>\n"
        "identity: <T> (forward x: T) -> forward _ requires std::is_reference_v<T> == { return x; }\n"
        "S: type = {\n"
        "    a: std::unique_ptr<int>;\n"
        "    operator=: (out this, move that) = { }\n"
        "}\n"
        "main: () = {\n"
        "    f := new<int>(0);\n"
        "    _ = f is decltype(f);\n"
        "    i := 0;\n"
        "    do { } while i > 0;\n"
        "    for (i + 1, 2) do (_) { }\n"
        "    _ = :() = std::array<int, :(x) = x;(1)>()$;\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("constexpr auto identity(T&& x) -> decltype(auto) requires (std::is_reference_v<T>)") !=
          std::string::npos);
    CHECK(cpp1.find("S([[maybe_unused]] S&& that) noexcept") != std::string::npos);
    CHECK(cpp1.find("operator=") == std::string::npos);
    CHECK(cpp1.find("cpp2::impl::is<decltype(f)>(cpp2::move(f))") != std::string::npos);
    CHECK(cpp1.find("} while (cpp2::impl::cmp_greater(i,0));\n") != std::string::npos);
    CHECK(cpp1.find("while (while") == std::string::npos);
    CHECK(cpp1.find(": { cpp2::move(i) + 1, 2 })") != std::string::npos);
    CHECK(cpp1.find("[_0 = std::array<int,[](auto const& x) -> decltype(auto) { return x; }(1)>()]") !=
          std::string::npos);
}

TEST_CASE("pure2-print declaration forms", "[misc-remaining]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "outer: type = {\n"
        "    object_alias: <T> T requires true == 42;\n"
        "    variadic: (x...: int) = { (std::cout << ... << x); }\n"
        "    y: (_: type_of(0)) = { }\n"
        "}\n"
        "main: () = {\n"
        "    v: ::std::string = \"s\";\n"
        "    n: type_of(1) = outer::object_alias<int>;\n"
        "    outer::variadic(n, 1);\n"
        "    outer::y(2);\n"
        "    std::cout << v;\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("template<typename T> requires (true) static constexpr T object_alias {42};") !=
          std::string::npos);
    CHECK(cpp1.find("auto variadic(auto const& ...x) -> void requires ((std::is_convertible_v<CPP2_TYPEOF(x), int> && ...))") !=
          std::string::npos);
    CHECK(cpp1.find("cpp2::impl::in<CPP2_TYPEOF(0)> unnamed_param_1") != std::string::npos);
    CHECK(cpp1.find("::std::string v {\"s\"};") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "print-forms") == "421s");
}

TEST_CASE("@sample_traverser adds add_1 to the enclosing namespace", "[misc-remaining][metafunction]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "ns: namespace = {\n"
        "    t: @sample_traverser type = { f: (a: int) -> int = a * 2; }\n"
        "}\n"
        "main: () = { std::cout << ns::add_1(41) << ns::t::f(2); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("auto add_1(auto const& x) -> decltype(auto) { return x+1; }") !=
          std::string::npos);
    CHECK(run_mixed_program(cpp1, "sample-traverser") == "424");
}

TEST_CASE("only a type in a namespace that requests @sample_traverser adds add_1",
          "[misc-remaining][metafunction]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "ns: namespace = { u: type = { f: (a: int) -> int = a; } }\n"
        "t: @sample_traverser type = { f: (a: int) -> int = a * 2; }\n"
        "main: () = { std::cout << ns::u::f(1) << t::f(2); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("add_1") == std::string::npos);
    CHECK(run_mixed_program(cpp1, "sample-traverser-unrequested") == "14");
}

TEST_CASE("member functions are defined after all type definitions", "[misc-remaining]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "X: type = {\n"
        "    py: *Y;\n"
        "    operator=: (out this, out y: Y) = { y = this&; py = y&; }\n"
        "    exx: (this, count: int) = {\n"
        "        _: finally = :() = std::cout << \"[(count)$]\";\n"
        "        if count < 3 { py*.why(count + 1); }\n"
        "    }\n"
        "}\n"
        "Y: type = {\n"
        "    px: *X;\n"
        "    operator=: (out this, x: *X) = { px = x; }\n"
        "    why: (this, count: int) = { px*.exx(count + 1); }\n"
        "}\n"
        "A: <T> type = {\n"
        "    B: <I: int> type = { f: <V> (v: V) -> V = v + I; }\n"
        "}\n"
        "main: () = {\n"
        "    y: Y;\n"
        "    x: X = (out y);\n"
        "    x.exx(1);\n"
        "    std::cout << A<int>::B<2>::f(40);\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("auto exx(cpp2::impl::in<int> count) const& -> void;") != std::string::npos);
    CHECK(cpp1.find("auto X::exx(cpp2::impl::in<int> count) const& -> void") != std::string::npos);
    CHECK(cpp1.find("y.construct(&((*this)));") != std::string::npos);
    CHECK(cpp1.find("[[maybe_unused]] cpp2::finally auto_6_9 {[&]()") != std::string::npos);
    CHECK(cpp1.find("template<typename T> template<int I> template<typename V> [[nodiscard]] auto A<T>::B<I>::f(") !=
          std::string::npos);
    CHECK(run_mixed_program(cpp1, "out-of-line-members") == "[3][1]42");
}

namespace {

// Checks that `cpp1` compiles with `-Wall -Wextra -Wpedantic -Werror`, as
// tools/strict-compile.sh does.
auto check_compiles_without_warnings(const std::string& cpp1, const std::string& name) -> void {
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-" + name);
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (name + ".cpp");
    test_support::write_file(cpp1_path, cpp1);
    const auto result = test_support::run_process({compiler, "-std=c++23", "-Wall", "-Wextra",
        "-Wpedantic", "-Werror", "-fsyntax-only", "-I", CPPFRONTIER_RUNTIME_INCLUDE,
        cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO("compiler output: " << result.output);
    REQUIRE_FALSE(result.timed_out);
    CHECK(result.exit_status == 0);
}

}

TEST_CASE("local objects named _ get unique names and may be unused", "[hunt][lowering]") {
    const auto cpp1 = last_use_output(
        "#include <string>\n"
        "point: @struct type = { x: int = 0; }\n"
        "main: () = {\n"
        "    _: std::string = \"a\";\n"
        "    _: std::string = ();\n"
        "    _: point = ();\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("[[maybe_unused]] std::string auto_4_5 {\"a\"};") != std::string::npos);
    CHECK(cpp1.find("[[maybe_unused]] std::string auto_5_5 {};") != std::string::npos);
    CHECK(cpp1.find("[[maybe_unused]] point auto_6_5 {};") != std::string::npos);
    check_compiles_without_warnings(cpp1, "local-wildcard-names");
}

TEST_CASE("data members named _ get unique names", "[hunt][lowering]") {
    const auto cpp1 = last_use_output(
        "s: @struct type = { _: int = 3; _: int = 4; v: int = 5; }\n"
        "main: () = { x: s = (); std::cout << x.v; }\n");
    INFO(cpp1);
    CHECK(cpp1.find("int auto_1_21 {3}; int auto_1_33 {4};") != std::string::npos);
    check_compiles_without_warnings(cpp1, "member-wildcard-names");
}

TEST_CASE("the subject of a wildcard-only inspect may be unused", "[hunt][lowering]") {
    const auto cpp1 = last_use_output(
        "f: (x: int) -> int = {\n"
        "    return inspect x -> int { is _ = 0; };\n"
        "}\n"
        "main: () -> int = f(1);\n");
    INFO(cpp1);
    CHECK(cpp1.find("[[maybe_unused]] auto&& __cpp2_expr = x;") != std::string::npos);
    check_compiles_without_warnings(cpp1, "inspect-wildcard");
}

TEST_CASE("that may be unused", "[hunt][lowering]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "Base: type = {\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, that) = std::cout << \"copy\\n\";\n"
        "    swap: (inout this, that) = { }\n"
        "}\n"
        "main: () = { b: Base = (); c := b; _ = c; }\n");
    INFO(cpp1);
    CHECK(cpp1.find("Base::Base([[maybe_unused]] Base const& that)") != std::string::npos);
    CHECK(cpp1.find("auto Base::operator=([[maybe_unused]] Base&& that)") != std::string::npos);
    CHECK(cpp1.find("auto Base::swap([[maybe_unused]] Base const& that)& noexcept") != std::string::npos);
    check_compiles_without_warnings(cpp1, "unused-that");
}

TEST_CASE("deferred locals are initialized in declaration order", "[audit-gaps][safety]") {
    const auto message = [](const std::string& source) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().message;
    };
    CHECK(message("main: () = { a: int; b: int; b = 1; a = 2; _ = a + b; }\n") ==
          "local variable a must be initialized before b (local variables must be initialized "
          "in the order they are declared)");
    CHECK(message("f: (out x: int, out y: int) = { y = 1; x = 2; }\n") ==
          "local variable x must be initialized before y (local variables must be initialized "
          "in the order they are declared)");
    CHECK(message("main: () = { a: int; b: int; if true { b = 1; a = 2; } "
                  "else { a = 1; b = 2; } _ = a + b; }\n").find("a must be initialized before b") !=
          std::string::npos);
    CHECK(message("main: () = { b: int; a: int; b = 1; a = 2; _ = a + b; }\n").empty());
    CHECK(message("f: () -> (x: int, y: int) = { y = 1; x = 2; }\n").empty());
    CHECK(message("f: (out x: int, out y: int) = { x = 1; y = 2; }\n"
                  "main: () = { a: int; b: int; f(out a, out b); _ = a + b; }\n").empty());
    CHECK(message("k: (out x: int) -> int = { x = 3; return 5; }\n"
                  "main: () = { d: int; _ = k(out d); _ = d; }\n").empty());
    CHECK(message("main: () = { d: int; _ = d; d = 1; }\n") ==
          "local variable d is used before it was initialized");
}

TEST_CASE("raw string interpolations accept format specifications", "[audit-gaps][strings]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "main: () = {\n"
        "    x := 42;\n"
        "    s := $R\"(x=(x:04)$ // /* (x)$ */ \"q\")\";\n"
        "    std::cout << s << R\"( (x)$)\";\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("R\"(x=)\" + cpp2::to_string(x,\"{:04}\") + R\"( // /* )\" + "
                    "cpp2::to_string(x) + R\"( */ \"q\")\"") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "raw-interpolation") == "x=0042 // /* 42 */ \"q\" (x)$");
}

TEST_CASE("type-scope object aliases of the enclosing type are defined after it",
          "[audit-gaps][aliases]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "X: type = {\n"
        "    public i: int = 4;\n"
        "    zero: X == ();\n"
        "    w :== 7;\n"
        "    w2: _ == 8;\n"
        "    N: type = { one: N == (); public k: int = 1; }\n"
        "}\n"
        "Y: <T> type = {\n"
        "    y0: Y<T> == ();\n"
        "    public k: T = 9;\n"
        "    Z: type = { z: Z == (); public k: int = 2; }\n"
        "}\n"
        "main: () = {\n"
        "    std::cout << X::zero.i << X::w + X::w2 << X::N::one.k << Y<int>::y0.k\n"
        "              << Y<int>::Z::z.k;\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("static const X zero;\nstatic constexpr auto w {7};\n"
                    "static constexpr auto w2 {8};") != std::string::npos);
    CHECK(cpp1.find("inline constexpr X X::zero {};") != std::string::npos);
    CHECK(cpp1.find("inline constexpr X::N X::N::one {};") != std::string::npos);
    CHECK(cpp1.find("template<typename T> inline constexpr Y<T> Y<T>::y0 {};") !=
          std::string::npos);
    CHECK(cpp1.find("template<typename T> inline constexpr typename Y<T>::Z Y<T>::Z::z {};") !=
          std::string::npos);
    CHECK(run_mixed_program(cpp1, "type-scope-alias") == "415192");
}

TEST_CASE("as conversions lower as upstream", "[audit-gaps][as]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <variant>\n"
        "main: () = {\n"
        "    x: int = 5;\n"
        "    v: std::variant<int, std::string> = 3;\n"
        "    a := x as i32 as i64;\n"
        "    b := 10 as i16;\n"
        "    p := x&;\n"
        "    e := p as *int;\n"
        "    std::cout << a << b << v as int << e* << (x + 1) as long;\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("cpp2::impl::as_<cpp2::i64>(cpp2::impl::as_<cpp2::i32>(x))") != std::string::npos);
    CHECK(cpp1.find("cpp2::impl::as_<cpp2::i16, 10>()") != std::string::npos);
    CHECK(cpp1.find("cpp2::impl::as_<int*>(cpp2::move(p))") != std::string::npos);
    CHECK(cpp1.find("cpp2::impl::as_<int>(CPP2_FORWARD(v))") != std::string::npos);
    CHECK(cpp1.find("cpp2::impl::as_<long>((cpp2::move(x) + 1))") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "as-conversions") == "510356");
}

TEST_CASE("@value checks its functions and destructor as upstream", "[audit-gaps][value]") {
    const auto codes = [](const std::string& source) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        auto found = std::vector<std::string>{};
        for (const auto& diagnostic : result.diagnostics) found.push_back(diagnostic.code);
        return found;
    };
    CHECK(codes("P: @value type = { x: int = 0; protected f: (this) = { } }\n") ==
          std::vector<std::string>{"value-protected-or-virtual"});
    CHECK(codes("P: @value type = { x: int = 0; f: (virtual this) = { } }\n") ==
          std::vector<std::string>{"value-protected-or-virtual"});
    CHECK(codes("P: @basic_value type = { x: int = 0; private operator=: (move this) = { } }\n") ==
          std::vector<std::string>{"value-non-public-destructor"});

    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "W: @value <U> type = { public u: U = 2; }\n"
        "main: () = { a: W<int> = (); b := a; std::cout << (a == b) << b.u; }\n");
    INFO(cpp1);
    CHECK(cpp1.find("W() = default;") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "value-template") == "12");
}

TEST_CASE("polymorphic types get no generated converting assignment", "[audit-gaps][value-set]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "B: type = {\n"
        "    public b: int = 0;\n"
        "    operator=: (out this, v: int) = { b = v; }\n"
        "    operator=: (out this, that) = { b = that.b; }\n"
        "    f: (virtual this) -> int = b;\n"
        "}\n"
        "D: type = {\n"
        "    this: B = ();\n"
        "    public d: int = 0;\n"
        "    operator=: (out this, v: int) = { B = v; d = v; }\n"
        "    operator=: (out this, that) = { d = that.d; }\n"
        "    f: (override this) -> int = d;\n"
        "}\n"
        "P: type = { public p: int = 0; operator=: (out this, v: int) = { p = v; } }\n"
        "main: () = { d: D = 3; e: D = d; e = d; x: P = 1; x = 2; std::cout << e.f() << e.b << x.p; }\n");
    INFO(cpp1);
    CHECK(cpp1.find("auto operator=(cpp2::impl::in<int> v) -> B&;") == std::string::npos);
    CHECK(cpp1.find("auto operator=(cpp2::impl::in<int> v) -> D&;") == std::string::npos);
    CHECK(cpp1.find("auto operator=(cpp2::impl::in<int> v) -> P&;") != std::string::npos);
    CHECK(cpp1.find("B::operator=(static_cast<B const&>(that));") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "polymorphic-value-set") == "332");
}

TEST_CASE("constructors and assignments set members in declaration order",
          "[audit-gaps][members]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "X: type = {\n"
        "    public a: int = 1;\n"
        "    public b: std::string = \"b\";\n"
        "    public c: int = 3;\n"
        "    operator=: (out this, move v: std::string) = {\n"
        "        b = v;\n"
        "        std::cout << \"body \";\n"
        "        c = 4;\n"
        "    }\n"
        "}\n"
        "main: () = { x: X = \"v\"; x = \"w\"; std::cout << x.a << x.b << x.c; }\n");
    INFO(cpp1);
    CHECK(cpp1.find("X::X(std::string&& v) : b{CPP2_FORWARD(v)} {") != std::string::npos);
    CHECK(cpp1.find("a = 1; b = CPP2_FORWARD(v); c = 3; \n    std::cout << \"body \";") !=
          std::string::npos);
    CHECK(run_mixed_program(cpp1, "member-order") == "body body 1w4");

    const auto rejected = cppfrontier::translator{}.translate_source(
        "X: type = {\n"
        "    a: int;\n"
        "    operator=: (out this) = { std::cout << 1; a = 2; }\n"
        "}\n");
    REQUIRE_FALSE(rejected.diagnostics.empty());
    CHECK(rejected.diagnostics.front().message ==
          "in operator=, expected 'a = ...' initialization statement (because type scope "
          "object 'a' does not have a default initializer)");
}

TEST_CASE("@flag_enum conversions name enumerators that collide with locals", "[review]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "F: @flag_enum<u8> type = { x; s; ret; }\n"
        "E: @enum type = { s; a; }\n"
        "main: () = {\n"
        "    std::cout << F::from_string(\"(x, ret)\").to_string() << E::from_string(\"s\").to_string();\n"
        "}\n");
    INFO(cpp1);
    CHECK(run_mixed_program(cpp1, "review-flag-enum-names") == "(x, ret)s");
}

TEST_CASE("a type-scope == function without this is static", "[review]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "B: type = {\n"
        "    c: i32 == 7;\n"
        "    sc: () -> i32 == c + 1;\n"
        "}\n"
        "main: () = { static_assert(B::sc() == 8); std::cout << B::sc(); }\n");
    INFO(cpp1);
    CHECK(run_mixed_program(cpp1, "review-static-constexpr-member") == "8");
}

TEST_CASE("a returned expression list is list-initialized for a typed return", "[review]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <vector>\n"
        "A: <T: type> type = {\n"
        "    Al: type == std::vector<T>;\n"
        "    mk: (this) -> Al = (1, 2);\n"
        "}\n"
        "g: () -> std::vector<int> = { return (1, 2, 3); }\n"
        "h: () -> std::vector<int> = ();\n"
        "k: () -> _ = (5);\n"
        "main: () = { std::cout << A<int>().mk().size() << g().size() << h().size() << k(); }\n");
    INFO(cpp1);
    CHECK(run_mixed_program(cpp1, "review-return-expression-list") == "2305");
}

TEST_CASE("function-local type definitions are diagnosed", "[review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: () -> int = {\n"
        "    L: @struct type = { v: int = 2; }\n"
        "    M: type = { v: int = 2; }\n"
        "    N: type == int;\n"
        "    return L().v;\n"
        "}\n");
    REQUIRE(result.has_errors());
    auto local_types = 0;
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.message);
        CHECK(diagnostic.code == "local-type");
        if (diagnostic.code == "local-type") ++local_types;
    }
    CHECK(local_types >= 1);
}

TEST_CASE("enumerators of an @enum nested in a template are defined with its template head", "[review]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "O: <T: type> type requires std::is_integral_v<T> = {\n"
        "    E: @enum type = { a; b; }\n"
        "    I: <U: type> type = { F: @flag_enum type = { x; y; } }\n"
        "    e: (this) -> E = E::b;\n"
        "}\n"
        "main: () = {\n"
        "    std::cout << O<int>().e().to_string() << O<int>::I<char>::F::y.to_string();\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("template<typename T> requires (std::is_integral_v<T>) inline constexpr "
                    "typename O<T>::E O<T>::E::a {0};") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "review-nested-template-enum") == "b(y)");
}

TEST_CASE("each out-of-line member definition maps to its own source line", "[review]") {
    const auto cpp1 = last_use_output(
        "N: namespace = {\n"
        "X: type = {\n"
        "    f: (this) -> int = 1;\n"
        "\n"
        "    g: (this) -> int = {\n"
        "        x := 1;\n"
        "        return x;\n"
        "    }\n"
        "}\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("#line 3 \"input.cpp2\"\n[[nodiscard]] auto X::f()") != std::string::npos);
    CHECK(cpp1.find("#line 5 \"input.cpp2\"\n[[nodiscard]] auto X::g()") != std::string::npos);
}

TEST_CASE("member declarations in a type begin on their source lines", "[review]") {
    const auto cpp1 = last_use_output(
        "O: <T: type> type = {\n"
        "    f: (this) -> int = 1;\n"
        "\n"
        "    g: (this) -> int = 2;\n"
        "    I: type = {\n"
        "        h: (this) -> int = 3;\n"
        "    }\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("#line 1 \"input.cpp2\"\ntemplate<typename T> struct O {\n"
                    "[[nodiscard]] auto f() const& -> int;\n\n[[nodiscard]] auto g() const& -> int;\n"
                    "struct I {\n[[nodiscard]] auto h() const& -> int;\n") != std::string::npos);
}

TEST_CASE("UFCS calls an operator function by its full name", "[review][ufcs]") {
    const auto cpp1 = last_use_output(
        "X: type = {\n"
        "    operator(): (this, a: int) -> int = a;\n"
        "    twice: (this) -> int = this.operator()(2) + operator()(3);\n"
        "}\n"
        "g: (x: X, y: X) -> bool = x.operator()(5) == 5 && x.operator==(y);\n");
    INFO(cpp1);
    CHECK(cpp1.find("CPP2_UFCS(operator())((*this), 2) + operator()(3)") != std::string::npos);
    CHECK(cpp1.find("CPP2_UFCS(operator())(x, 5)") != std::string::npos);
    CHECK(cpp1.find("CPP2_UFCS(operator==)(x, y)") != std::string::npos);
}

TEST_CASE("a UFCS function name is not a member of a move this receiver", "[review][last-use]") {
    const auto cpp1 = last_use_output(
        "X: type = {\n"
        "    g: (this) -> int = 1;\n"
        "    size: (this) -> int = 3;\n"
        "    f: (move this, o: std::string) -> int = o.size();\n"
        "    h: (move this) -> int = this.g();\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("return CPP2_UFCS(size)(o);") != std::string::npos);
    CHECK(cpp1.find("return CPP2_UFCS(g)(cpp2::move((*this)));") != std::string::npos);
}

TEST_CASE("postconditions run before a returned value moves from a parameter", "[review][last-use]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "take: (copy s: std::string) -> int = s.ssize();\n"
        "f: (copy s: std::string) -> int post(s.ssize() == 2) = { return take(s); }\n"
        "g: (b: bool) post(b || !b) = { if b { return; } }\n"
        "main: () = { std::cout << f(\"ab\"); g(true); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("cpp2_finally_presuccess.run(); return take(CPP2_FORWARD(s));") !=
          std::string::npos);
    CHECK(cpp1.find("cpp2_finally_presuccess.run(); return;") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "postcondition-last-use") == "2");
}

TEST_CASE("a UFCS template argument's qualified name does not qualify the call", "[review][ufcs]") {
    const auto cpp1 = last_use_output(
        "f: <B: bool, T> (x: int) -> int = x + B;\n"
        "g: (x: int) -> int = x.f<(cpp2::impl::cmp_greater(1,0)), std::string>() + x.N::f<(cpp2::impl::cmp_greater(1,0)), std::string>();\n");
    INFO(cpp1);
    CHECK(cpp1.find("CPP2_UFCS_TEMPLATE(f<(cpp2::impl::cmp_greater(1,0)), std::string>)(x)") != std::string::npos);
    CHECK(cpp1.find("N::f<(cpp2::impl::cmp_greater(1,0)), std::string>(x)") != std::string::npos);
}

TEST_CASE("a returned expression list is list-initialized", "[review]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "P: @struct type = { a: int; b: int; }\n"
        "mk: (n: int) -> P = (n, n + 1);\n"
        "mk2: (n: int) -> P = { return (n, (n)); }\n"
        "main: () = { std::cout << mk(1).b << mk2(2).a; }\n");
    INFO(cpp1);
    CHECK(cpp1.find("return {n, n + 1}; }") != std::string::npos);
    CHECK(cpp1.find("return {n, (n)};") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "returned-list") == "22");
}

TEST_CASE("an unevaluated operand is not a last use", "[review][last-use]") {
    const auto cpp1 = last_use_output(
        "take: (copy s: std::string) -> int = s.ssize();\n"
        "f: (copy x: std::string) -> bool = std::is_same_v<decltype(x), std::string>;\n"
        "g: (copy x: std::string) -> int = { n := take(x); return n + sizeof(x); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("std::is_same_v<decltype(x), std::string>") != std::string::npos);
    CHECK(cpp1.find("auto n {take(CPP2_FORWARD(x))};") != std::string::npos);
    CHECK(cpp1.find("return cpp2::move(n) + sizeof(x);") != std::string::npos);
}

TEST_CASE("a mention in a compile-time context is not a use and is never moved",
          "[last-use]") {
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "h: <T> () = { }\n"
        "k: <N: int> () = { }\n"
        "a: (copy x: std::string) = {\n"
        "    g(x);\n"
        "    n := sizeof(x);\n"
        "    m := alignof(decltype(x));\n"
        "    e := noexcept(g(x));\n"
        "    r := requires { x.size(); };\n"
        "    s := requires (i: int) { x + i; };\n"
        "    h<decltype(x)>();\n"
        "}\n"
        "b: <Ts...: type> (copy x...: Ts) = { n := sizeof...(x); }\n"
        "c: (copy x: std::string) = { _ = decltype(x)(\"a\"); }\n"
        "d: (copy x: int) = { k<x>(); }\n"
        "e: (copy x: int) = { g(x); k<x>(); }\n"
        "f: (copy x: std::string) = { g(x, sizeof(x)); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("g(CPP2_FORWARD(x));\n    auto n {sizeof(x)};\n    auto m {alignof(decltype(x))};\n"
                    "    auto e {noexcept(g(x))};\n    auto r {requires { CPP2_UFCS_NONLOCAL(size)(x); }};\n"
                    "    auto s {requires (int const& i) { x + i; }};\n    h<decltype(x)>();") !=
          std::string::npos);
    CHECK(cpp1.find("auto n {sizeof...(x)};") != std::string::npos);
    CHECK(cpp1.find("decltype(x)(\"a\")") != std::string::npos);
    CHECK(cpp1.find("k<x>();") != std::string::npos);
    CHECK(cpp1.find("g(cpp2::move(x));\n#line 16\n    k<x>();") != std::string::npos);
    CHECK(cpp1.find("g(CPP2_FORWARD(x), sizeof(x));") != std::string::npos);
    for (const auto* context : {"<cpp2::move", "decltype(cpp2::move", "sizeof(cpp2::move",
                                "sizeof...(cpp2::move", "{ cpp2::move"}) {
        CHECK(cpp1.find(context) == std::string::npos);
    }
}

TEST_CASE("the operand of typeid is a use", "[last-use]") {
    // A polymorphic glvalue operand is evaluated, so `typeid` is not a compile-time context.
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "a: (copy x: std::string) = { g(x); n := typeid(x).name(); }\n"
        "b: (copy x: std::string) = { n := typeid(x).name(); g(x); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("g(x);\n#line 2\n    auto n {CPP2_UFCS(name)(typeid(CPP2_FORWARD(x)))};") != std::string::npos);
    CHECK(cpp1.find("auto n {CPP2_UFCS(name)(typeid(x))};\n#line 3\n    g(CPP2_FORWARD(x));") != std::string::npos);
}

TEST_CASE("an assignment target is never moved", "[last-use]") {
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "S: @struct type = { m: int = 0; n: S2 = (); }\n"
        "S2: @struct type = { k: int = 0; }\n"
        "f: (copy a: int, copy b: int, copy c: S, copy d: std::vector<int>, move e: S,\n"
        "    copy p: *int, v: int) = {\n"
        "    g(a); a = v;\n"
        "    b += v;\n"
        "    c.m = v;\n"
        "    d[0] = v;\n"
        "    e.n.k -= v;\n"
        "    p* = v;\n"
        "}\n"
        "l: () = { x: S = (); x.m = 1; y: std::vector<int> = (1); y[0] = 2; z := 0; z = 1; }\n"
        "o: (out r: std::string) = { r = \"r\"; g(r); }\n");
    INFO(cpp1);
    // An `out` parameter is never moved.
    CHECK(cpp1.find("g(r.value());") != std::string::npos);
    CHECK(cpp1.find("g(a);\n#line 6\n    a = v;") != std::string::npos);
    CHECK(cpp1.find("    b += v;") != std::string::npos);
    CHECK(cpp1.find("    c.m = v;") != std::string::npos);
    CHECK(cpp1.find("CPP2_ASSERT_IN_BOUNDS_LITERAL(d, 0) = v;") != std::string::npos);
    CHECK(cpp1.find("    e.n.k -= v;") != std::string::npos);
    CHECK(cpp1.find("*cpp2::impl::assert_not_null(p) = v;") != std::string::npos);
    CHECK(cpp1.find("    x.m = 1;") != std::string::npos);
    CHECK(cpp1.find("CPP2_ASSERT_IN_BOUNDS_LITERAL(y, 0) = 2;") != std::string::npos);
    CHECK(cpp1.find("    z = 1;") != std::string::npos);
    CHECK(cpp1.find("cpp2::move") == std::string::npos);
}

TEST_CASE("a definite last use is the only use of the name in its statement", "[last-use]") {
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "g2: (x: _, y: _) = { }\n"
        "a: (copy x: std::string) = { g2(x, x); }\n"
        "b: (copy x: std::string) = { g2(x, x.size()); }\n"
        "c: (copy x: std::string) = { _ = x.substr(x.size()); }\n"
        "d: (copy x: std::string, copy y: std::string) = { g2(x, y); }\n"
        "e: (copy x: int) = { g(x + x); }\n"
        "h: (copy v: std::vector<int>) = { g(v[v[0]]); }\n"
        "k: (copy x: std::string) = { g(\"(x)$ (x)$\"); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("g2(x, x);") != std::string::npos);
    CHECK(cpp1.find("g2(x, CPP2_UFCS(size)(x));") != std::string::npos);
    CHECK(cpp1.find("CPP2_UFCS(substr)(x, CPP2_UFCS(size)(x))") != std::string::npos);
    CHECK(cpp1.find("g2(CPP2_FORWARD(x), CPP2_FORWARD(y));") != std::string::npos);
    CHECK(cpp1.find("g(x + x);") != std::string::npos);
    CHECK(cpp1.find("CPP2_ASSERT_IN_BOUNDS(v, CPP2_ASSERT_IN_BOUNDS_LITERAL(v, 0))") !=
          std::string::npos);
    CHECK(cpp1.find("cpp2::to_string(x) + \" \" + cpp2::to_string(x)") != std::string::npos);
}

TEST_CASE("last uses in loop conditions, next clauses and loop parameters", "[last-use]") {
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "w: (copy x: std::string) = { while x.empty() { } }\n"
        "n: (copy x: int) = { i := 0; while i < 2 next g(x) { i++; } }\n"
        "d: (copy x: int) = { i := 0; do { i++; } while i < x; }\n"
        "f: (copy v: std::vector<int>, copy x: int) = { for v next g(x) do (e) { } }\n"
        "after: (copy x: int) = { i := 0; while i < x { i++; } g(x); }\n"
        "r: (copy v: std::vector<std::string>) = {\n"
        "    for v do (copy e) { g(e); }\n"
        "    for v do (move m) { g(m); g(m); }\n"
        "    for v do (forward q) { g(q); }\n"
        "    for v do (inout i) { g(i); }\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("while (CPP2_UFCS(empty)(x))") != std::string::npos);
    CHECK(cpp1.find("g(x);\n    }") != std::string::npos);
    CHECK(cpp1.find("} while (cpp2::impl::cmp_less(i,x));") != std::string::npos);
    CHECK(cpp1.find("while (cpp2::impl::cmp_less(i,x)) {\n#line 6\n        ++i;\n    }\n"
                    "#line 6\n    g(cpp2::move(x));") !=
          std::string::npos);
    CHECK(cpp1.find("for (auto e : v) {\n#line 8\n        g(cpp2::move(e));") != std::string::npos);
    CHECK(cpp1.find("g(m);\n#line 9\n        g(cpp2::move(m));") != std::string::npos);
    CHECK(cpp1.find("g(CPP2_FORWARD(q));") != std::string::npos);
    CHECK(cpp1.find("g(i);") != std::string::npos);
    // Only the uses after the loops, and the loop parameters, are moved.
    CHECK(cpp1.find("cpp2::move(x)") == cpp1.rfind("cpp2::move(x)"));
}

TEST_CASE("last uses through pointers, lambdas and the guard prefix", "[last-use]") {
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "p1: (copy x: int) = { p := x&; g(p*); }\n"
        "p2: (copy x: std::unique_ptr<int>) = { g(x*); }\n"
        "l1: (copy x: std::string) = { l := :(y: std::string) = g(y); l(x); }\n"
        "l2: () = { l := :(copy y: std::string) = { g(y); g(y); }; l(\"a\"); }\n"
        "l3: () = { l := :(forward y) = g(y); l(1); }\n"
        "gd: () = { guard := 1; g(guard); guard_lock := 2; g(guard_lock); guardian := 3; g(guardian); }\n"
        "gc: (copy guard_x: std::string, move guard_y: std::string) = { g(guard_x); g(guard_y); }\n"
        "gf: (forward guard_x: std::string) = { g(guard_x); }\n"
        "u: () = { unguarded := 4; g(unguarded); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("auto p {&(x)};\n#line 2\n"
                    "    g(*cpp2::impl::assert_not_null(cpp2::move(p)));") !=
          std::string::npos);
    CHECK(cpp1.find("g(*cpp2::impl::assert_not_null(CPP2_FORWARD(x)));") != std::string::npos);
    CHECK(cpp1.find("return g(y); }};\n#line 4\n    cpp2::move(l)(CPP2_FORWARD(x));") != std::string::npos);
    CHECK(cpp1.find("g(y);\n#line 5\n    g(CPP2_FORWARD(y));") != std::string::npos);
    CHECK(cpp1.find("return g(CPP2_FORWARD(y)); }") != std::string::npos);
    CHECK(cpp1.find("g(guard);") != std::string::npos);
    CHECK(cpp1.find("g(guard_lock);") != std::string::npos);
    CHECK(cpp1.find("g(guardian);") != std::string::npos);
    CHECK(cpp1.find("g(guard_x);\n#line 8\n    g(guard_y);") != std::string::npos);
    CHECK(cpp1.find("g(CPP2_FORWARD(guard_x));") == std::string::npos);
    CHECK(cpp1.find("{\n#line 9\n    g(guard_x);\n}") != std::string::npos);
    CHECK(cpp1.find("g(cpp2::move(unguarded));") != std::string::npos);
}

TEST_CASE("a guard-prefixed local is not moved into a parameter or another object",
          "[last-use]") {
    const auto cpp1 = last_use_output(
        "use: (copy l: std::string) = { }\n"
        "passed: () = { l: std::string = \"l\"; use(l); }\n"
        "guard_passed: () = { guard_l: std::string = \"l\"; use(guard_l); }\n"
        "initializes: () = { l: std::string = \"l\"; { other := l; } }\n"
        "guard_initializes: () = { guard_l: std::string = \"l\"; { other := guard_l; } }\n");
    INFO(cpp1);
    CHECK(cpp1.find("use(CPP2_FORWARD(l));") != std::string::npos);
    CHECK(cpp1.find("use(guard_l);") != std::string::npos);
    CHECK(cpp1.find("auto other {CPP2_FORWARD(l)};") != std::string::npos);
    CHECK(cpp1.find("auto other {guard_l};") != std::string::npos);
}

TEST_CASE("last uses in contracts, inspect and statement parameters", "[last-use]") {
    const auto cpp1 = last_use_output(
        "g: (x: _) = { }\n"
        "pre1: (copy x: int) pre(x > 0) = { g(x); }\n"
        "post1: (copy x: int) post(x > 0) = { g(x); }\n"
        "post2: (copy x: int) -> int post(x > 0) = x;\n"
        "post3: (copy x: int, copy y: int) post(x > 0) = { g(y); }\n"
        "assert1: (copy x: int) = { g(x); assert(x > 0); }\n"
        "assert2: (copy x: int) = { assert(x > 0); g(x); }\n"
        "insp: (copy x: std::string) -> int = { return inspect x -> int { is \"a\" = 1; is _ = 0; }; }\n"
        "insp2: (copy x: std::string) -> int = inspect x -> int { is _ = x.ssize(); };\n"
        "stmt: (copy x: std::string) = {\n"
        "    (copy y: std::string = \"y\") { g(y); }\n"
        "    (move z: std::string = \"z\") g(z);\n"
        "    (s: std::string = \"s\") g(s);\n"
        "    (inout t := x) g(t);\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("!(cpp2::impl::cmp_greater(x,0)) ) { cpp2::cpp2_default.report_violation(\"\"); }\n"
                    "#line 2\n    g(cpp2::move(x));") !=
          std::string::npos);
    // A postcondition is checked after the body, so it is the last use.
    CHECK(cpp1.find("!(cpp2::impl::cmp_greater(x,0)) ) { cpp2::cpp2_default.report_violation(\"\"); }} );\n"
                    "#line 3\n    g(x);\n}") !=
          std::string::npos);
    CHECK(cpp1.find("report_violation(\"\"); }} );\n    return x; }") != std::string::npos);
    CHECK(cpp1.find("g(cpp2::move(y));\n}") != std::string::npos);
    // A mention in an assertion is a use that is never moved.
    CHECK(cpp1.find("g(x);\n#line 6\n"
                    "    if (cpp2::cpp2_default.is_active() && !(cpp2::impl::cmp_greater(x,0)) )") !=
          std::string::npos);
    CHECK(cpp1.find("auto&& __cpp2_expr = CPP2_FORWARD(x);") != std::string::npos);
    // The subject and an alternative's value are in one statement.
    CHECK(cpp1.find("auto&& __cpp2_expr = x; return CPP2_UFCS(ssize)(x);") != std::string::npos);
    CHECK(cpp1.find("g(CPP2_FORWARD(y));\n        }") != std::string::npos);
    CHECK(cpp1.find("g(CPP2_FORWARD(z));") != std::string::npos);
    CHECK(cpp1.find("g(s);") != std::string::npos);
    // An `inout` statement parameter binds its initializer as an lvalue.
    CHECK(cpp1.find("auto& t{x};\n#line 14\n        g(t);") != std::string::npos);
}

TEST_CASE("a capture beside another mention of its name is not moved", "[review][last-use]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "take: (copy s: std::string) -> int = s.ssize();\n"
        "both: (copy s: std::string, copy l) -> int = take(s) * 10 + l();\n"
        "f: () -> int = { x: std::string = \"abcde\"; return both(x, :() -> int = take(x$);); }\n"
        "g: () -> int = { x: std::string = \"abcde\"; return :() -> int = take(x$);() + take(x); }\n"
        "main: () = { std::cout << f(); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("return both(x, [_0 = x]()") != std::string::npos);
    CHECK(cpp1.find("cpp2::move(x)") == std::string::npos);
    CHECK(run_mixed_program(cpp1, "capture-beside-mention") == "55");
}

TEST_CASE("a typed value without an initializer is diagnosed", "[upstream-355]") {
    const auto expected = "an anonymous object declaration must have '=' and an initializer";
    CHECK(has_error(error_lines("alias1: == std::function<(:int)>;\n"), "1:27: " + std::string{expected}));
    CHECK(has_error(error_lines("alias2: == (:int);\n"), "1:13: " + std::string{expected}));
    CHECK(has_error(error_lines("f: () = g(1, :std::map<int, int>);\n"), "1:14: " + std::string{expected}));
    CHECK(error_lines("f: () -> int = g(1, :std::map<int, int> = (), (:int = 2));\n").empty());
}

TEST_CASE("inspect value-initializes a compound result type", "[upstream-724]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "main: () = {\n"
        "    v := 3;\n"
        "    s: * const char = \"a\";\n"
        "    s = inspect v -> * const char {\n"
        "        is 3 = \"three\";\n"
        "        is _ = \"\";\n"
        "    };\n"
        "    i := inspect v -> int { is 4 = 1; is _ = 7; };\n"
        "    std::cout << s << i;\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("else return std::type_identity_t<char const*>{};") != std::string::npos);
    CHECK(cpp1.find("else return int{};") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "inspect-pointer-result") == "three7");
}

TEST_CASE("a call named after the local being initialized is a member call",
          "[upstream-863][upstream-864][ufcs]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <functional>\n"
        "#include <optional>\n"
        "decl: @struct type = {\n"
        "    require: (this, b: bool) -> int = b;\n"
        "    value: (this) -> int = 5;\n"
        "}\n"
        "require_fn: type == std::function<int(bool)>;\n"
        "f: (inout t: decl) = {\n"
        "    require: require_fn = :(args...) = t&$*.require(args...);\n"
        "    value: std::optional = t.value();\n"
        "    std::cout << require(true) << value*;\n"
        "}\n"
        "main: () = { t: decl = (); f(t); _ = t; }\n");
    INFO(cpp1);
    CHECK(cpp1.find("return (*cpp2::impl::assert_not_null(_0)).require(args...);") !=
          std::string::npos);
    CHECK(cpp1.find("std::optional value {t.value()};") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "initializing-local-member-call") == "15");
}

TEST_CASE("a templated call whose name finds an object is a member call",
          "[upstream-889][ufcs]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "t: @struct type = { g: <T> (this) -> int = 7; f: (this) -> int = 1; }\n"
        "f: int = t().f();\n"
        "h: () -> int = {\n"
        "    g := 0;\n"
        "    return t().g<int>() + g;\n"
        "}\n"
        "k: () -> int = t().g<int>();\n"
        "main: () = std::cout << h() << f << k();\n");
    INFO(cpp1);
    CHECK(cpp1.find("t().template g<int>() + cpp2::move(g)") != std::string::npos);
    CHECK(cpp1.find("int f {t().f()};") != std::string::npos);
    CHECK(cpp1.find("CPP2_UFCS_TEMPLATE(g<int>)(t())") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "templated-member-call-object-name") == "717");
}

TEST_CASE("a local hides a wrapped declaration and a last-use candidate of its name",
          "[last-use]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "f: (out x: int, copy s: std::string) = {\n"
        "    x = 1;\n"
        "    {\n"
        "        x := 2;\n"
        "        s := 3;\n"
        "        std::cout << x << s;\n"
        "    }\n"
        "    std::cout << s;\n"
        "}\n"
        "main: () = { y: int; f(out y, \"a\"); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("std::cout << cpp2::move(x) << cpp2::move(s);") != std::string::npos);
    CHECK(cpp1.find("std::cout << CPP2_FORWARD(s);\n}") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "local-hides-outer-names") == "23a");
}

TEST_CASE("a last use inside a fold or pack expansion is not moved from", "[upstream-1279][last-use]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "foo: (forward item: _) -> int = std::is_rvalue_reference_v<decltype(item)>;\n"
        "sum: (args...: int) -> int = (0 + ... + args);\n"
        "bar_fold: <Is...: std::size_t> () = {\n"
        "    test := 42;\n"
        "    (std::cout << ... << (Is * 0 + foo(test)));\n"
        "    other := 1;\n"
        "    std::cout << sum((Is * 0 + foo(other))...);\n"
        "    last := 2;\n"
        "    std::cout << foo(last);\n"
        "}\n"
        "main: () = { bar_fold<0, 1>(); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("(std::cout << ... << (Is * 0 + foo(test)));") != std::string::npos);
    CHECK(cpp1.find("sum((Is * 0 + foo(other))...)") != std::string::npos);
    CHECK(cpp1.find("foo(cpp2::move(last))") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "fold-last-use") == "0001");
}

TEST_CASE("a module unit keeps its module declaration ahead of generated code",
          "[upstream-1190][upstream-1358][mixed]") {
    const auto fragment = last_use_output(
        "module;\n"
        "#include <string>\n"
        "export module utils:part2;\n"
        "import :part1;\n"
        "f: () -> int = 1;\n");
    INFO(fragment);
    const auto include = fragment.find("#include \"cpp2util.h\"");
    const auto string_include = fragment.find("#include <string>");
    const auto declaration = fragment.find("export module utils:part2;");
    const auto import = fragment.find("import :part1;");
    const auto forward = fragment.find("auto f() -> int;");
    CHECK(fragment.starts_with("module;\n"));
    CHECK(include < string_include);
    CHECK(string_include < declaration);
    CHECK(declaration < import);
    CHECK(import < forward);
    CHECK(forward != std::string::npos);

    const auto plain = last_use_output("module foo;\nf: () -> int = 1;\n");
    INFO(plain);
    CHECK(plain.starts_with("module;\n"));
    CHECK(plain.find("#include \"cpp2util.h\"") < plain.find("module foo;"));
    CHECK(plain.find("module foo;") < plain.find("auto f() -> int;"));
}

TEST_CASE("@print pretty-prints declarations as upstream", "[audit-gaps][print]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "outer: @print type = {\n"
        "    f: <T> (x: T, inout v: std::vector<int>) -> int pre(v.empty(), \"n (x)$\") = {\n"
        "        s := :() = { };\n"
        "        for v next s() do (e) { if e < 0 { break; } else { if e > 9 { continue; } } }\n"
        "        return v.ssize() as int;\n"
        "    }\n"
        "    g: () -> (a: int) = { a = 1; }\n"
        "}\n"
        "main: () = { }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.printed ==
          "\n\nouter:/* @print */ type = \n"
          "{\n"
          "    f: <T: type, >(\n"
          "        in x: T, \n"
          "        inout v: std::vector<int>, \n"
          "    ) -> move int\n"
          "        pre( v.empty(), \"n \" + cpp2::to_string(x) + \"\" ) = \n"
          "    {\n"
          "        s: _ = :() = \n"
          "            {\n"
          "            };\n"
          "        for v\n"
          "        next s()\n"
          "        do (in e: _)\n"
          "            {\n"
          "                if e < 0\n"
          "                {\n"
          "                    break;\n"
          "                }\n"
          "                else if e > 9\n"
          "                {\n"
          "                    continue;\n"
          "                }\n"
          "            }\n"
          "        return v.ssize() as int;\n"
          "    }\n"
          "\n"
          "    g:() -> (out a: int, ) = \n"
          "    {\n"
          "        a = 1;\n"
          "        return;\n"
          "    }\n"
          "}\n");
}

TEST_CASE("statements that share a source line keep its line number", "[review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () = {\n"
        "    a := 0; b := 0;\n"
        "    c := a + b;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("    auto a {0};\n#line 2\n    auto b {0};\n    auto c {") !=
          std::string::npos);
    const auto one_line = cppfrontier::translator{}.translate_source(
        "main: () = { a := 0; b := a; }\n");
    INFO(one_line.cpp1);
    CHECK(one_line.cpp1.find(" {\n#line 1\n    auto a {0};\n#line 1\n    auto b {cpp2::move(a)};\n}") !=
          std::string::npos);
    // A `next` clause follows the body, so its `#line` goes back.
    const auto loop = cppfrontier::translator{}.translate_source(
        "main: () = {\n"
        "    i := 0;\n"
        "    for (1, 2) next i++ do (e) {\n"
        "        i += e;\n"
        "    }\n"
        "}\n");
    INFO(loop.cpp1);
    CHECK(loop.cpp1.find("} while (false);\n#line 3\n        ++i;\n") != std::string::npos);
}

TEST_CASE("an escaped quote character literal is one token", "[realworld]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "main: () = { c := '\\''; if c == '\\'' { std::cout << \"q\"; } else { std::cout << \"x\"; } }\n");
    CHECK(run_mixed_program(cpp1, "escaped-quote") == "q");
}

TEST_CASE("a deferred local declared inside a loop may be initialized there", "[realworld]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <string>\n"
        "#include <vector>\n"
        "main: () = {\n"
        "    v: std::vector<int> = (1, 2);\n"
        "    for v do (e) {\n"
        "        s: std::string;\n"
        "        if e == 1 { s = \"one\"; } else { s = \"other\"; }\n"
        "        std::cout << s;\n"
        "    }\n"
        "}\n");
    CHECK(run_mixed_program(cpp1, "loop-deferred-local") == "oneother");
}

TEST_CASE("metafunctions in reflect.h2 style declarations", "[realworld]") {
    // `@polymorphic_base` checks copy functions before a later `@copy_constructible`
    // adds them, a function taking `that` is not a copy, and `@struct<noforward>` takes
    // an argument while `@copy_constructible <T>` declares template parameters.
    const auto result = cppfrontier::translator{}.translate_source(
        "B: @polymorphic_base @copy_constructible <T> type = {\n"
        "    n: T = ();\n"
        "    append: (inout this, that) = { n += that.n; }\n"
        "}\n"
        "P: @struct<noforward> type = { a: int = 0; b: int = 0; }\n"
        "main: () = { p: P = (1, 2); _ = p; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("template<typename T> struct B") != std::string::npos);
    CHECK(result.cpp1.find("B([[maybe_unused]] B const& that)") != std::string::npos);
    CHECK(result.cpp1.find("template<typename noforward>") == std::string::npos);
}

TEST_CASE("a file-scope using-directive is Cpp1", "[realworld]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "#include <iostream>\nusing namespace std;\nmain: () = { cout << \"hi\"; }\n");
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find("using namespace std;") != std::string::npos);
    CHECK(cppfrontier::translator{}.translate_source(
        "main: () = { using namespace std; }\n").has_errors());
}

TEST_CASE("a function naming a qualified type alias is declared after it", "[realworld]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "n: namespace = {\n"
        "    holder: @struct <T: type> type = { v: T = (); }\n"
        "    alias_t: type == holder<int>;\n"
        "    show: (a: n::alias_t) -> int = a.v;\n"
        "}\n"
        "main: () = { a: n::alias_t = (7); std::cout << n::show(a); }\n");
    CHECK(run_mixed_program(cpp1, "qualified-alias-order") == "7");
}

TEST_CASE("a base initialized from a list in a derived constructor", "[realworld]") {
    // `append` takes `that` but is not an assignment, so it gets no memberwise
    // assignments, and `type` names a parameter.
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <string>\n"
        "Base: @polymorphic_base type = {\n"
        "    public a: int = 0;\n"
        "    public b: int = 0;\n"
        "    operator=: (out this, x: int, y: int) = { a = x; b = y; }\n"
        "}\n"
        "D: type = {\n"
        "    this: Base = ();\n"
        "    s: std::string = \"\";\n"
        "    operator=: (out this, x: int, y: int) = { Base = (x, y); }\n"
        "    append: (inout this, that) = { s += that.s; }\n"
        "    sum: (this) -> int = a + b;\n"
        "}\n"
        "has_double: (type: std::string) -> bool = type.find(\"double\") != type.npos;\n"
        "main: () = { d: D = (3, 4); std::cout << d.sum() << has_double(\"a double\"); }\n");
    CHECK(run_mixed_program(cpp1, "base-list-initializer") == "71");
}

TEST_CASE("a contextual word after an operator names a function", "[realworld][hunt]") {
    // From upstream's reflect.h2 `sample_print`, which calls parse.h's `pre`.
    const auto result = cppfrontier::translator{}.translate_source(
        "pre: (n: int) -> int = n;\n"
        "post: (n: int) -> int = n;\n"
        "f: (n: int) -> int = { std::cout\n << pre(n); return pre(n) + post<int>(n); }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("std::cout\n << pre(n);") != std::string::npos);
    CHECK(result.cpp1.find("return pre(n) + post<int>(n);") != std::string::npos);
}

TEST_CASE("a contract group may be an expression", "[realworld]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "checker: type = {\n"
        "    public active: bool = true;\n"
        "    operator=: (out this) = { }\n"
        "    is_active: (this) -> bool = active;\n"
        "    report_violation: (this, msg: * const char) = { std::cout << msg; }\n"
        "    f: (this, x: int) pre<this>(x > 0, \"positive\") = { }\n"
        "}\n"
        "main: () = { c: checker = (); c.f(1); c.f(0); }\n");
    CHECK(cpp1.find("(*this).is_active()") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "contract-group-this") == "positive");
}

TEST_CASE("a statement before a block's closing brace needs its semicolon", "[realworld]") {
    // As upstream and the grammar, `expression-statement: expression ';'`.
    for (const auto* source : {"main: () = { w := 1 }\n", "main: () = { f(1) }\n",
                               "main: () = { x := :() = {} }\n"}) {
        CAPTURE(source);
        CHECK(cppfrontier::translator{}.translate_source(source).has_errors());
    }
    CHECK_FALSE(cppfrontier::translator{}.translate_source(
        "main: () = { x := 1; _ = :() -> int = x$; }\n").has_errors());
}

TEST_CASE("a captured out parameter or deferred local is read through value()",
          "[review][captures]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "f: (out x: int) = {\n"
        "    x = 1;\n"
        "    n := :() -> int = x$ * 2;\n"
        "    m := :() = { x := 7; std::cout << x; };\n"
        "    m();\n"
        "    std::cout << n() << x;\n"
        "}\n"
        "main: () = {\n"
        "    a: int;\n"
        "    f(out a);\n"
        "    s: std::string;\n"
        "    s = \"hi\";\n"
        "    l := :() -> std::string = s$ + \"!\";\n"
        "    std::cout << l() << a;\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("[_0 = x.value()]") != std::string::npos);
    CHECK(cpp1.find("[_0 = cpp2::move(s.value())]") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "review-captured-out") == "721hi!1");
}

TEST_CASE("pointer safety checks statement and range parameters", "[review][diagnostics-parity]") {
    const auto lines = error_lines(
        "#include <vector>\n"
        "main: () = {\n"
        "    a: int = 1;\n"
        "    q: *int = a&;\n"
        "    (copy p: *int = q) { p++; }\n"
        "    (copy d := q) { d--; }\n"
        "    v: std::vector<*int> = (q);\n"
        "    for v do (r: *int) { r++; }\n"
        "    (copy n: *int = nullptr) { }\n"
        "}\n");
    CHECK(has_error(lines,
        "5:27: ++ - pointer arithmetic is illegal - use std::span or gsl::span instead"));
    CHECK(has_error(lines,
        "6:22: -- - pointer arithmetic is illegal - use std::span or gsl::span instead"));
    CHECK(has_error(lines,
        "8:27: ++ - pointer arithmetic is illegal - use std::span or gsl::span instead"));
    CHECK(has_error(lines,
        "9:21: pointer cannot be initialized to null or int - leave it uninitialized and then "
        "set it to a non-null value when you have one"));
    CHECK(lines.size() == 4);
}

TEST_CASE("pointer safety follows resolved names", "[diagnostics-parity]") {
    // `this.p` names the member, not the parameter `p`; a capture `p$` names `p`.
    const auto lines = error_lines(
        "X: type = {\n"
        "    p: *int;\n"
        "    operator=: (out this, v: *int) = { p = v; }\n"
        "    bump: (inout this, p: int) = { this.p++; _ = p + 1; }\n"
        "}\n"
        "main: () = {\n"
        "    x := 1;\n"
        "    p := x&;\n"
        "    f := :() = { q := p$; q++; };\n"
        "    f();\n"
        "}\n");
    CHECK(has_error(lines,
        "4:42: ++ - pointer arithmetic is illegal - use std::span or gsl::span instead"));
    CHECK(has_error(lines,
        "9:28: ++ - pointer arithmetic is illegal - use std::span or gsl::span instead"));
    CHECK(lines.size() == 2);
}

TEST_CASE("a do statement's condition does not see the locals of its body", "[review]") {
    const auto lines = error_lines(
        "p: int = 0;\n"
        "f: (q: *int) = {\n"
        "    do { p: *int = q; } while p++ == 0;\n"
        "}\n");
    CHECK(lines.empty());
}

TEST_CASE("a templated call is a member call when using names an object", "[review][ufcs]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "N: namespace = { g: int = 3; }\n"
        "t: @struct type = { g: <T> (this) -> int = sizeof(T); }\n"
        "main: () = {\n"
        "    using N::g;\n"
        "    std::cout << t().g<int>() << g;\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("t().template g<int>()") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "review-using-object-template-call") == "43");
}

TEST_CASE("a members-only call with template arguments uses the template keyword",
          "[ports][templates]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "holder: type = { get: <N: int> (this) -> int = N; }\n"
        "call: <H> (h: H) -> int = h..get<4>();\n"
        "main: () = { std::cout << call(:holder = ()) << (:holder = ())..get<2>(); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("h.template get<4>()") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "ports-members-only-template") == "42");
}

TEST_CASE("the type of an unnamed object can be a dependent name", "[ports][templates]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <vector>\n"
        "first: <C> (c: C) -> C::value_type = {\n"
        "    if c.empty() { return :C::value_type = (); }\n"
        "    return c.front() as C::value_type;\n"
        "}\n"
        "main: () = { std::cout << first(:std::vector<int> = ()) << first(:std::vector<int> = (7)); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("typename C::value_type{}") != std::string::npos);
    CHECK(cpp1.find("as_<typename C::value_type>") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "ports-dependent-unnamed-object") == "07");
}

TEST_CASE("a type alias marks a dependent member template", "[ports][templates]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <vector>\n"
        "holder: <T> type = { rebind: <U> type == std::vector<U>; }\n"
        "a: <H> type == H::rebind<int>;\n"
        "f: <H> () -> std::size_t = {\n"
        "    b: type == H::rebind<char>;\n"
        "    x: b = (1, 2);\n"
        "    return x.size() + a<H>(3).size();\n"
        "}\n"
        "main: () = { std::cout << f<holder<long>>(); }\n");
    INFO(cpp1);
    CHECK(cpp1.find("using a = typename H::template rebind<int>;") != std::string::npos);
    CHECK(cpp1.find("using b = typename H::template rebind<char>;") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "ports-dependent-alias") == "5");
}

TEST_CASE("a function alias returning an empty list value-initializes", "[ports][templates]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <string>\n"
        "empty: () -> std::string == ();\n"
        "pair: () -> std::pair<int, int> == (3, 4);\n"
        "main: () = { std::cout << empty().size() << pair().second; }\n");
    INFO(cpp1);
    CHECK(cpp1.find("return {}; }") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "ports-alias-empty-list") == "04");
}

TEST_CASE("a marked dependent decltype member is a type template argument", "[ports][templates]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <tuple>\n"
        "#include <vector>\n"
        "f: <T> (_: T) -> std::vector<type decltype(T())::value_type> = (1, 2, 3);\n"
        "g: <Ts...> (_...: Ts) -> std::tuple<type decltype(Ts())::value_type...> = ();\n"
        "main: () = {\n"
        "    v: std::vector<int> = ();\n"
        "    std::cout << f(v).size() << std::tuple_size_v<decltype(g(v, v))>;\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("std::vector<typename decltype(T())::value_type>") != std::string::npos);
    CHECK(cpp1.find("std::tuple<typename decltype(Ts())::value_type...>") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "ports-marked-decltype-argument") == "32");
}

TEST_CASE("requires clauses on templated declarations", "[ports][templates]") {
    // A clause on a non-template is left to the Cpp1 compiler.
    CHECK_FALSE(cppfrontier::translator{}.translate_source(
        "f: () -> int requires true = 1;\n").has_errors());
    const auto templated = cppfrontier::translator{}.translate_source(
        "holder: <T> type = { g: (this) -> int requires std::is_integral_v<T> = 1;\n"
        "    inner: type = { k: () -> int requires true = 3; } }\n"
        "h: (x) -> int requires std::is_integral_v<decltype(x)> = 2;\n"
        "v: <T> const T requires std::same_as<T, i32> = 0;\n"
        "c: (x: _ is std::integral) -> int requires true = x;\n");
    for (const auto& diagnostic : templated.diagnostics) INFO(diagnostic.code);
    CHECK_FALSE(templated.has_errors());
}

TEST_CASE("pack indexing is not a bounds-checked subscript", "[ports][templates]") {
    const auto cpp1 = last_use_output(
        "first: <Ts...> (xs...: Ts) -> _ = xs...[0];\n"
        "at: (v: std::vector<int>) -> int = v[0];\n");
    CHECK(cpp1.find("return xs...[0];") != std::string::npos);
    CHECK(cpp1.find("CPP2_ASSERT_IN_BOUNDS_LITERAL(v, 0)") != std::string::npos);
}

TEST_CASE("a pack parameter is moved or forwarded at its last use in an expansion",
          "[last-use][review]") {
    const auto cpp1 = last_use_output(
        "g: (xs...: _) = { }\n"
        "f: <Ts...> (forward xs...: Ts) = { g(xs...); }\n"
        "m: <Ts...> (move xs...: Ts) = { g(xs...); }\n"
        "twice: <Ts...> (forward xs...: Ts) = { g((xs + xs)...); }\n"
        "outer: (copy x: int, ys...: _) = { g((x + ys)...); }\n");
    INFO(cpp1);
    // `Ts` may be given explicitly as a reference, so a `move` pack is
    // forwarded too, which moves an object.
    const auto forwarded = cpp1.find("g(CPP2_FORWARD(xs)...);");
    REQUIRE(forwarded != std::string::npos);
    CHECK(cpp1.find("g(CPP2_FORWARD(xs)...);", forwarded + 1) != std::string::npos);
    CHECK(cpp1.find("g(cpp2::move(xs)...);") == std::string::npos);
    // Two mentions in one expansion, or a name that is not the pack, stay.
    CHECK(cpp1.find("g((xs + xs)...);") != std::string::npos);
    CHECK(cpp1.find("g((x + ys)...);") != std::string::npos);
}

namespace {

// "line:column code" for each error diagnostic.
auto error_codes(const std::string& source) -> std::vector<std::string> {
    const auto result = cppfrontier::translator{}.translate_source(source);
    auto codes = std::vector<std::string>{};
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.severity != cppfrontier::diagnostic_severity::error) continue;
        codes.push_back(std::to_string(diagnostic.span.begin.line) + ":" +
                        std::to_string(diagnostic.span.begin.column) + " " + diagnostic.code);
    }
    return codes;
}

using codes = std::vector<std::string>;

}

TEST_CASE("a function needs a body unless it is virtual or defaultable", "[parser][review]") {
    CHECK(error_codes("f: () -> int;\n") == codes{"1:1 missing-function-body"});
    CHECK(error_codes("S: type = { f: (this) -> int; }\n") == codes{"1:13 missing-function-body"});
    CHECK(error_codes("S: type = { f: (virtual this) -> int; "
                      "operator==: (this, that) -> bool; }\n").empty());
    CHECK(error_codes("I: @interface type = { f: (this) -> int; }\n").empty());
}

TEST_CASE("a type uses all its tokens", "[parser][review]") {
    CHECK(error_codes("main: () = { x: int const = 1; }\n") ==
          codes{"1:21 unexpected-type-token"});
    CHECK(error_codes("f: (x: int y) = { }\n") == codes{"1:12 unexpected-type-token"});
}

TEST_CASE("every non-block statement ends in ';'", "[parser][statements][review]") {
    CHECK(error_codes("main: () = { while true { break 1; } }\n") ==
          codes{"1:33 missing-semicolon"});
    CHECK(error_codes("f: () -> int = { return 1 }\n") == codes{"1:25 missing-semicolon"});
    CHECK(error_codes("f: () -> int = 1\n") == codes{"1:16 missing-semicolon"});
    CHECK(error_codes("main: () = { assert(true) }\n") == codes{"1:25 missing-semicolon"});
    CHECK(error_codes("main: () = { x := 0; do { x++; } while x < 3 }\n") ==
          codes{"1:44 missing-semicolon"});
}

TEST_CASE("break and continue name an enclosing loop", "[parser][statements][review]") {
    // A jump outside every loop is left to the Cpp1 compiler, as upstream does.
    CHECK(error_codes("main: () = { break; }\n").empty());
    CHECK(error_codes("main: () = { outer: while true { while true { break inner; } "
                      "break outer; } }\n") == codes{"1:47 unknown-loop-label"});
    CHECK(error_codes("main: () = { outer: while true { while true { break outer; } } }\n")
              .empty());
}

TEST_CASE("a trailing comma in a template argument list is dropped", "[parser][review]") {
    const auto source = std::string{"main: () = { v: std::vector<int,> = (); }\n"};
    CHECK(error_codes(source).empty());
    CHECK(cppfrontier::translator{}.translate_source(source).cpp1.find("std::vector<int>") !=
          std::string::npos);
}

TEST_CASE("'->' is not member access", "[parser][review]") {
    CHECK(error_codes("f: (p: *std::string) -> std::size_t = p->size();\n") ==
          codes{"1:40 cpp1-member-arrow"});
}

TEST_CASE("a condition may call a function expression", "[parser][statements][review]") {
    CHECK(error_codes("main: () = { if :() -> bool = { return true; }() { } }\n").empty());
}

TEST_CASE("an unbraced body gets one diagnostic", "[parser][statements][review]") {
    CHECK(error_codes("main: () = { x := 1; if (x == 1) x = 2; }\n") ==
          codes{"1:34 unbraced-body"});
    CHECK(error_codes("main: () = { x := 1; if x == 1 { } else x = 2; }\n") ==
          codes{"1:41 unbraced-body"});
}

TEST_CASE("only 'move' and 'forward' return passing are allowed", "[parser][review]") {
    CHECK(error_codes("f: () -> move int = 1;\n").empty());
    CHECK(error_codes("f: () -> in int = 1;\n") == codes{"1:10 invalid-return-passing"});
}

TEST_CASE("names declared twice are left to the Cpp1 compiler", "[parser][review]") {
    CHECK(error_codes("f: (x: int, x: int) = { }\n").empty());
    CHECK(error_codes("main: () = { x: int = 1; x: int = 2; }\n").empty());
    // An empty suffix would generate a second function with the same name.
    CHECK(error_codes("ad: @autodiff<\"suffix=\"> type = { "
                      "f: (x: double) -> (r: double) = { r = x * x; } }\n").size() == 1);
}

TEST_CASE("metafunctions come before template parameters", "[parser][review]") {
    CHECK(error_codes("tt: <T> @struct type = { x: T = (); }\n") ==
          codes{"1:9 metaclass-after-template-parameters"});
    CHECK(error_codes("tt: @struct <T> type = { x: T = (); }\n").empty());
}

TEST_CASE("Cpp1 'auto' is rejected in every type position", "[parser][review]") {
    CHECK(error_codes("main: () = { x: auto = 1; }\n") == codes{"1:17 cpp1-auto-type"});
    CHECK(error_codes("f: (x: auto) = { }\n") == codes{"1:8 cpp1-auto-type"});
}

TEST_CASE("a metafunction on a namespace gets one diagnostic", "[parser][review]") {
    CHECK(error_codes("N: @print namespace = { }\n") == codes{"1:11 namespace-metafunction"});
}

TEST_CASE("qualifiers alone before '=' omit the deduced type", "[pipeline][hunt]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "main: () = {\n"
        "    n: const = 6;\n"
        "    p: * = n&;\n"
        "    q: const * const = n&;\n"
        "    std::cout << n + p* + q*;\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("auto const n {6};") != std::string::npos);
    CHECK(cpp1.find("auto* p {&(n)};") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "qualifiers-without-type") == "18");
    // An object that deduces its type, qualified or not, needs a declared type
    // outside a function.
    CHECK(error_codes("a: const = 2;\n") ==
          codes{"1:1 namespace-scope-object-requires-type"});
    CHECK(error_codes("T: type = { b: const _ = 2; }\n") ==
          codes{"1:13 type-scope-object-requires-type"});
}

TEST_CASE("a base assigned a list binds it without constructing the base", "[pipeline][hunt]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "Shape: type = {\n"
        "    public id: int = 0;\n"
        "    operator=: (out this, i: int) = { id = i; }\n"
        "    operator=: (out this, that) = { id = that.id; }\n"
        "    area: (virtual this) -> int;\n"
        "    operator=: (virtual move this) = { }\n"
        "}\n"
        "Square: type = {\n"
        "    this: Shape;\n"
        "    side: int = 2;\n"
        "    operator=: (out this, i: int) = { Shape = (i); }\n"
        "    operator=: (out this, that) = { Shape = (that); side = that.side; }\n"
        "    area: (override this) -> int = side * side;\n"
        "}\n"
        "main: () = {\n"
        "    a: Square = 3;\n"
        "    b: Square = 5;\n"
        "    b = a;\n"
        "    std::cout << b.id << b.area();\n"
        "}\n");
    INFO(cpp1);
    CHECK(cpp1.find("Shape::operator=({that});") != std::string::npos);
    CHECK(run_mixed_program(cpp1, "abstract-base-assignment") == "34");
}

TEST_CASE("a literal keeps its user-defined suffix inside parentheses", "[pipeline][hunt]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "#include <string>\n"
        "main: () = {\n"
        "    using std::literals::_;\n"
        "    s := std::string(\"ab\"s + \"c\");\n"
        "    std::cout << (\"x\"sv).size() << s;\n"
        "}\n");
    INFO(cpp1);
    CHECK(run_mixed_program(cpp1, "literal-suffix") == "1abc");
}

TEST_CASE("a comment before a declaration's ';' stays out of the initializer",
          "[pipeline][hunt]") {
    const auto cpp1 = last_use_output(
        "#include <iostream>\n"
        "main: () = {\n"
        "    f := :() = { std::cout << 1; }  // a lambda\n"
        "    ;\n"
        "    g := 2  // a value\n"
        "    ;\n"
        "    a :== 3 // an alias\n"
        "    ;\n"
        "    _ = g // discarded\n"
        "    ;\n"
        "    f();\n"
        "    std::cout << g << a;\n"
        "}\n");
    INFO(cpp1);
    CHECK(run_mixed_program(cpp1, "comment-before-semicolon") == "123");
}

TEST_CASE("each contract check keeps the line of its clause", "[pipeline][contracts]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (x: int) -> (r: int)\n"
        "    pre(x > 0)\n"
        "    post(r > 0)\n"
        "= {\n"
        "    r = x;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto pre = result.cpp1.find("#line 2\n    if (cpp2::cpp2_default.is_active() && !(cpp2::impl::cmp_greater(x,0))");
    REQUIRE(pre != std::string::npos);
    // The postcondition follows on line 3, and the statement on line 5.
    CHECK(result.cpp1.find("\n    cpp2_finally_presuccess.add(", pre) != std::string::npos);
    CHECK(result.cpp1.find("\n\n    r.construct(x);", pre) != std::string::npos);
}

TEST_CASE("positions after not are those of the source", "[pipeline][names]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (b: bool) = {\n    x := not b && g(b = 3);\n}\n");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "chained-assignment");
    CHECK(result.diagnostics.front().span.begin.column == 23);
}
