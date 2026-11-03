#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cppfrontier/ir.hpp>
#include <cppfrontier/pipeline.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>

namespace {
auto function_with_parameters(std::initializer_list<cppfrontier::parameter> parameters) {
    auto declaration = cppfrontier::ir_declaration{};
    declaration.kind = cppfrontier::declaration_kind::function;
    declaration.name = "update";
    declaration.type = cppfrontier::parse_type("bool");
    declaration.parameters = parameters;
    declaration.span = {{1, 1}, {1, 16}};
    return declaration;
}

auto program_with_function(std::initializer_list<cppfrontier::parameter> parameters) {
    auto program = cppfrontier::ir_program{};
    program.declarations.push_back(function_with_parameters(parameters));
    return program;
}
}

TEST_CASE("function parameters lower from typed records", "[emitter][parameters]") {
    const auto program = program_with_function({
        {cppfrontier::passing_style::inout, cppfrontier::parse_type("std::string"), "text"},
        {cppfrontier::passing_style::in, cppfrontier::parse_type("int"), "count"},
        {cppfrontier::passing_style::out, cppfrontier::parse_type("int"), "value"},
    });
    const auto source = cppfrontier::source_text::from_bytes("update: () -> bool = { return true; }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto output = cppfrontier::emit_cpp1(program, source, tokens, {});
    REQUIRE(output.cpp1.find("auto update(std::string& text, cpp2::impl::in<int> count, cpp2::impl::out<int> value) -> bool") !=
            std::string::npos);
}

TEST_CASE("typed in parameters use the Cpp2 support type", "[emitter][parameters]") {
    const auto program = program_with_function({
        {cppfrontier::passing_style::in, cppfrontier::parse_type("i32"), "count"},
    });
    const auto source = cppfrontier::source_text::from_bytes(
        "update: () -> bool = { return true; }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto output = cppfrontier::emit_cpp1(program, source, tokens, {});
    INFO(output.cpp1);
    CHECK(output.cpp1.find("auto update(cpp2::impl::in<int> count) -> bool") != std::string::npos);
}

TEST_CASE("dependent typed in parameters remain const references",
          "[emitter][parameters][templates]") {
    auto declaration = function_with_parameters({
        {cppfrontier::passing_style::in, cppfrontier::parse_type("T"), "value"},
    });
    declaration.template_parameters.push_back(
        cppfrontier::template_parameter{.name = "T"});
    auto program = cppfrontier::ir_program{};
    program.declarations.push_back(std::move(declaration));

    const auto source = cppfrontier::source_text::from_bytes(
        "update: <T> (value: T) -> bool = { return true; }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto output = cppfrontier::emit_cpp1(program, source, tokens, {});
    INFO(output.cpp1);
    CHECK(output.cpp1.find("auto update(T const& value) -> bool") != std::string::npos);
    CHECK(output.cpp1.find("cpp2::impl::in<T>") == std::string::npos);
}

TEST_CASE("unnamed parameters receive deterministic unused names", "[emitter][parameters]") {
    const auto program = program_with_function({
        {cppfrontier::passing_style::in, cppfrontier::parse_type("int"), "_"},
        {cppfrontier::passing_style::inout, cppfrontier::parse_type("int"), "_"},
    });
    const auto source = cppfrontier::source_text::from_bytes(
        "update: () -> bool = { return true; }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto output = cppfrontier::emit_cpp1(program, source, tokens, {});
    INFO(output.cpp1);
    CHECK(output.cpp1.find("[[maybe_unused]] cpp2::impl::in<int> unnamed_param_1") !=
          std::string::npos);
    CHECK(output.cpp1.find("[[maybe_unused]] int& unnamed_param_2") != std::string::npos);
}

TEST_CASE("template return types remain intact", "[emitter][returns]") {
    const auto program = program_with_function({});
    auto declaration = program.declarations.front();
    declaration.type = cppfrontier::parse_type("std::vector<int>");
    auto templated = cppfrontier::ir_program{};
    templated.declarations.push_back(std::move(declaration));
    const auto source = cppfrontier::source_text::from_bytes(
        "update: () -> std::vector<int> = { return {}; }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto output = cppfrontier::emit_cpp1(templated, source, tokens, {});
    INFO(output.cpp1);
    CHECK(output.cpp1.find("auto update() -> std::vector<int>") != std::string::npos);
}

TEST_CASE("parameter passing styles lower to distinct Cpp1 forms", "[emitter][parameters]") {
    const auto program = program_with_function({
        {cppfrontier::passing_style::copy, cppfrontier::parse_type("std::string"), "copy_text"},
        {cppfrontier::passing_style::move, cppfrontier::parse_type("std::string"), "moved_text"},
        {cppfrontier::passing_style::forward, cppfrontier::parse_type("std::string"), "forwarded_text"},
    });
    const auto source = cppfrontier::source_text::from_bytes(
        "update: () -> bool = { return true; }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto output = cppfrontier::emit_cpp1(program, source, tokens, {});
    INFO(output.cpp1);
    CHECK(output.cpp1.find(
        "auto update(std::string copy_text, std::string&& moved_text, auto&& forwarded_text) -> bool") !=
        std::string::npos);
}

TEST_CASE("receiver qualifiers follow the this passing style",
          "[emitter][parameters][this]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    peek: (this) -> int = 1;\n"
        "    get: (inout this) -> int = 2;\n"
        "    consume: (move this) -> int = 3;\n"
        "    operator=: (inout this, that) = { }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto peek() const& -> int") != std::string::npos);
    CHECK(result.cpp1.find("auto get()& -> int") != std::string::npos);
    CHECK(result.cpp1.find("auto consume()&& -> int") != std::string::npos);
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] Widget const& that) -> Widget&") !=
          std::string::npos);
}

TEST_CASE("move this operator= lowers to a destructor",
          "[emitter][types][destructor]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 0;\n"
        "    operator=: (move this) = { }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("~Widget() noexcept") != std::string::npos);
    CHECK(result.cpp1.find("operator=()&&") == std::string::npos);
}

TEST_CASE("destructors reject additional parameters",
          "[emitter][types][destructor][diagnostics]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = { operator=: (move this, other: int) = { } }\n");
    CHECK(result.has_errors());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "invalid-destructor-parameters"; }));
}

TEST_CASE("member comparison operators return bool or an ordering",
          "[emitter][types][comparisons][diagnostics]") {
    const auto translate = [](const char* member) {
        return cppfrontier::translator{}.translate_source(
            std::string{"T: type = { x: int = 0; "} + member + " }\n");
    };
    const auto rejected = [](const auto& result, std::string_view message) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) {
                return diagnostic.code == "comparison-return-type" &&
                    diagnostic.message.find(message) != std::string::npos;
            });
    };
    CHECK(rejected(translate("operator<=>: (this, that) -> int = 0;"),
                   "operator<=> must return std::strong_ordering"));
    CHECK(rejected(translate("operator==: (this, that) -> int = 0;"),
                   "operator== must return bool"));
    CHECK(rejected(translate("operator==: (this, that) = { }"), "operator== must return bool"));
    CHECK_FALSE(translate("operator<=>: (this, that) -> std::weak_ordering;").has_errors());
    CHECK_FALSE(translate("operator==: (this, that) -> bool;").has_errors());
    CHECK_FALSE(translate("operator<=>: (this, that) -> _ = x <=> that.x;").has_errors());
    CHECK_FALSE(translate("operator<=>: (this, that) = x <=> that.x;").has_errors());
    CHECK_FALSE(translate("operator<=>: (this, that) -> ::std::partial_ordering;").has_errors());
    CHECK(rejected(translate("operator<: (this, that) -> _ = x < that.x;"),
                   "operator< must return bool"));
}

TEST_CASE("every comparison operator has its return type checked",
          "[emitter][comparisons][diagnostics]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "P: @struct type = { x: int = 0; }\n"
        "operator==: (a: P, b: P) -> int = a.x - b.x;\n"
        "operator!=: (a: P, b: P) = { }\n"
        "operator<: (a: P, b: P) -> _ = a.x < b.x;\n"
        "operator<=: (a: P, b: P) -> forward bool = true;\n"
        "operator>: (a: P, b: P) -> bool = a.x > b.x;\n"
        "operator>=: (a: P, b: P) -> std::string = \"\";\n"
        "operator<=>: (a: P, b: P) -> int = a.x - b.x;\n"
        "N: namespace = { operator<=>: (a: P, b: P) -> std::strong_ordering = a.x <=> b.x; }\n");
    auto messages = std::vector<std::string>{};
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == "comparison-return-type") messages.push_back(diagnostic.message);
    }
    CHECK(messages == std::vector<std::string>{
        "operator== must return bool", "operator!= must return bool",
        "operator< must return bool", "operator<= must return bool",
        "operator>= must return bool",
        "operator<=> must return std::strong_ordering, std::weak_ordering, or "
        "std::partial_ordering"});
}

TEST_CASE("a missing base initializer names the base type",
          "[emitter][types][constructors][diagnostics]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "B: type = { operator=: (out this, v: int) = { } }\n"
        "D: type = { this: B; operator=: (out this) = { } }\n");
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "member-not-initialized" &&
                diagnostic.message.find("expected 'B = ...'") != std::string::npos;
        }));
}

TEST_CASE("out this is restricted to operator= constructors",
          "[emitter][types][constructors][diagnostics]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = { build: (out this) = { } }\n");
    CHECK(result.has_errors());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "invalid-constructor-name"; }));
}

TEST_CASE("typed out parameters use the Cpp2 out wrapper",
          "[emitter][parameters][out]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "fill: (out value: int) = { value = 1; }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("cpp2::impl::out<int> value") != std::string::npos);
    CHECK(result.cpp1.find("value.construct(std::type_identity_t<int>{1})") !=
          std::string::npos);
}

TEST_CASE("untyped out parameters take the caller's out wrapper", "[emitter][parameters][out]") {
    for (const auto spelling : {"fill: (out value) = { value = 1; }\n",
                                "fill: (out value: _) = { value = 1; }\n"}) {
        const auto result = cppfrontier::translator{}.translate_source(spelling);
        INFO(result.cpp1);
        REQUIRE_FALSE(result.has_errors());
        CHECK(result.cpp1.find("auto fill(auto value_) -> void") != std::string::npos);
        CHECK(result.cpp1.find("auto value = cpp2::impl::out(value_);") != std::string::npos);
        CHECK(result.cpp1.find("value.construct(1);") != std::string::npos);
    }
}

TEST_CASE("only in, inout, out, and move are valid this passing styles",
          "[emitter][types][parameters][diagnostics]") {
    for (const auto style : {"in_ref", "forward", "copy"}) {
        const auto source = std::string{"Widget: type = { read: ("} + style +
            " this) -> int = 0; }\n";
        const auto result = cppfrontier::translator{}.translate_source(source);
        INFO(style);
        CHECK(result.has_errors());
        CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.code == "invalid-this-passing-style";
            }));
    }
}

TEST_CASE("reference-forced kinds lower to const references", "[emitter][parameters]") {
    const auto program = program_with_function({
        {cppfrontier::passing_style::in_ref, cppfrontier::parse_type("std::string"), "text"},
    });
    const auto source = cppfrontier::source_text::from_bytes(
        "update: () -> bool = { return true; }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto output = cppfrontier::emit_cpp1(program, source, tokens, {});
    INFO(output.cpp1);
    CHECK(output.cpp1.find("auto update(std::string const& text) -> bool") != std::string::npos);
}

TEST_CASE("forward_ref deduced returns lower to auto references", "[emitter][returns]") {
    const auto program = program_with_function({});
    auto declaration = program.declarations.front();
    declaration.deduction = cppfrontier::return_deduction::by_forwarding_ref;
    auto forwarded = cppfrontier::ir_program{};
    forwarded.declarations.push_back(std::move(declaration));
    const auto source = cppfrontier::source_text::from_bytes(
        "update: () -> forward_ref _ = { return value; }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto output = cppfrontier::emit_cpp1(forwarded, source, tokens, {});
    INFO(output.cpp1);
    CHECK(output.cpp1.find("auto update() -> auto&&") != std::string::npos);
}

TEST_CASE("typed forwarding returns retain reference qualification", "[emitter][returns][parameters]") {
    auto declaration = function_with_parameters({});
    declaration.type = cppfrontier::parse_type("std::string");
    declaration.deduction = cppfrontier::return_deduction::by_forwarding_typed;
    auto program = cppfrontier::ir_program{};
    program.declarations.push_back(std::move(declaration));
    const auto source = cppfrontier::source_text::from_bytes(
        "update: () -> forward std::string = { return value; }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto output = cppfrontier::emit_cpp1(program, source, tokens, {});
    INFO(output.cpp1);
    CHECK(output.cpp1.find("auto update() -> std::string&") != std::string::npos);
}

TEST_CASE("typed forwarding parameters contribute conversion constraints",
          "[emitter][parameters][requires]") {
    auto declaration = function_with_parameters({
        {cppfrontier::passing_style::forward, cppfrontier::parse_type("std::string"), "text"},
    });
    declaration.requires_clause = cppfrontier::parsed_expression{"true"};
    auto program = cppfrontier::ir_program{};
    program.declarations.push_back(std::move(declaration));
    const auto source = cppfrontier::source_text::from_bytes(
        "update: (forward text: std::string) requires true = { }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto output = cppfrontier::emit_cpp1(program, source, tokens, {});
    INFO(output.cpp1);
    CHECK(output.cpp1.find(
        "requires (true && std::is_convertible_v<CPP2_TYPEOF(text), "
        "std::add_const_t<std::string>&>)") != std::string::npos);
}

TEST_CASE("emitter source map covers generated and verbatim lines", "[emitter][source-map]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "auto text = R\"(first\nsecond)\";\n"
        "hello: () = \"hello\";\n");
    const auto tokens = cppfrontier::tokenize(source, 3, 4);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    REQUIRE(parsed.diagnostics.empty());
    const auto built = cppfrontier::build_ir(parsed.tree);
    const auto output = cppfrontier::emit_cpp1(built.program, source, tokens, {});

    const auto maps_line = [&](std::uint32_t generated_line, std::uint32_t source_line) {
        return std::any_of(output.source_map.begin(), output.source_map.end(),
            [&](const auto& entry) {
                return entry.first.line == generated_line && entry.second.line == source_line;
            });
    };
    REQUIRE(output.source_map.size() >= 2);
    // The forward declaration of `hello` comes first, on its line.
    CHECK(maps_line(output.source_map.front().first.line, 3));
    CHECK(maps_line(output.source_map.front().first.line + 3, 1));
    CHECK(maps_line(output.source_map.back().first.line, 3));
}

TEST_CASE("template declarations emit one shared template prefix", "[emitter][templates]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "box: <T: type = int, N: i32 = 3> type = { value: T = (); }\n"
        "values: <T> type == std::vector<T>;\n"
        "identity: <T> (x: T) = x;\n"
        "scale: <N: i32> () = N;\n"
        "v: <N: i32> int = N;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 6);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    for (const auto& issue : parsed.diagnostics) INFO(issue.code << ": " << issue.message);
    REQUIRE(parsed.diagnostics.empty());
    const auto built = cppfrontier::build_ir(parsed.tree);
    const auto output = cppfrontier::emit_cpp1(built.program, source, tokens, {});

    INFO(output.cpp1);
    CHECK(output.cpp1.find("template<typename T = int, int N = 3> struct box") != std::string::npos);
    CHECK(output.cpp1.find("template<typename T> using values = std::vector<T>;") != std::string::npos);
    CHECK(output.cpp1.find("template<typename T> auto identity(T const& x) -> decltype(auto)") !=
          std::string::npos);
    CHECK(output.cpp1.find("template<int N> auto scale() -> decltype(auto)") != std::string::npos);
    CHECK(output.cpp1.find("template<int N> int v {N};") != std::string::npos);
}

TEST_CASE("constraints follow the upstream declaration grammar", "[emitter][templates][requires]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "f: <T> () requires std::regular<T> = T();\n"
        "X: <T> type requires std::is_integral_v<T> = { value: T = (); }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 3);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    REQUIRE(parsed.diagnostics.empty());
    const auto built = cppfrontier::build_ir(parsed.tree);
    const auto output = cppfrontier::emit_cpp1(built.program, source, tokens, {});

    INFO(output.cpp1);
    CHECK(output.cpp1.find("auto f() -> decltype(auto) requires (std::regular<T>)") !=
          std::string::npos);
    CHECK(output.cpp1.find("template<typename T> requires (std::is_integral_v<T>) struct X") !=
          std::string::npos);
}

TEST_CASE("variadic template parameters render before their names", "[emitter][templates][variadic]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "count_types: <Ts...: type> () = sizeof...(Ts);\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    REQUIRE(parsed.diagnostics.empty());
    const auto built = cppfrontier::build_ir(parsed.tree);
    const auto output = cppfrontier::emit_cpp1(built.program, source, tokens, {});

    INFO(output.cpp1);
    CHECK(output.cpp1.find("template<typename ...Ts>") != std::string::npos);
}

TEST_CASE("unnamed template parameters receive deterministic names", "[emitter][templates]") {
    const auto source = cppfrontier::source_text::from_bytes("f: <_: i32, _: i32> () = 0;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    REQUIRE(parsed.diagnostics.empty());
    const auto built = cppfrontier::build_ir(parsed.tree);
    const auto output = cppfrontier::emit_cpp1(built.program, source, tokens, {});

    INFO(output.cpp1);
    CHECK(output.cpp1.find("template<int UnnamedTemplateParam1, int UnnamedTemplateParam2>") !=
          std::string::npos);
}

namespace {
auto translated(const char* source) -> std::string {
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) UNSCOPED_INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    return result.cpp1;
}
}

TEST_CASE("a Cpp1 using-directive in a mixed file is a Cpp1 line", "[emitter][review]") {
    const auto cpp1 = translated(
        "#include <iostream>\n"
        "f: () = { }\n"
        "using namespace std;\n"
        "main: () = { cout << 1; }\n");
    CHECK(cpp1.find("using namespace std;") != std::string::npos);
}

TEST_CASE("function declarations after a using-directive are not hoisted", "[emitter][review]") {
    for (const auto* directive : {"using namespace std;\n", "using std::_;\n"}) {
        const auto cpp1 = translated(
            (std::string{"#include <string>\n"} + directive + "f: (x: string) = { }\n").c_str());
        CHECK(cpp1.find("auto f(cpp2::impl::in<string> x) -> void;") >
              cpp1.find("using namespace std;"));
    }
}

TEST_CASE("function declarations naming what Cpp1 lines declare follow those lines",
          "[emitter][review]") {
    const auto cpp1 = translated(
        "namespace q { template<typename T> concept cq = true; using I = int; }\n"
        "using q::cq;\n"
        "n: namespace = { c: <T> concept = true; }\n"
        "ns: namespace == n;\n"
        "p: (x: int) = { }\n"
        "using namespace n;\n"
        "g: (x: _ is cq) = { }\n"
        "h: (x: q::I) = { }\n"
        "k: (x: _ is ns::c) = { }\n"
        "m: (x: _ is c) = { }\n");
    const auto after = [&](std::string_view declaration, std::string_view line) {
        const auto found = cpp1.find(declaration);
        return found != std::string::npos && found > cpp1.find(line);
    };
    CHECK(after("auto g(cq auto const& x) -> void;", "using q::cq;"));
    CHECK(after("auto h(cpp2::impl::in<q::I> x) -> void;", "namespace q {"));
    CHECK(after("auto k(ns::c auto const& x) -> void;", "namespace ns = n;"));
    CHECK(after("auto m(c auto const& x) -> void;", "using namespace n;"));
    CHECK_FALSE(after("auto p(cpp2::impl::in<int> x) -> void;", "namespace q {"));
}

TEST_CASE("a parameter named like a namespace-scope object does not stop hoisting",
          "[emitter][review]") {
    const auto cpp1 = translated(
        "s: type = { }\n"
        "f: (x: int) = { }\n"
        "x: int = 0;\n"
        "g: (y: int = x) = { }\n");
    CHECK(cpp1.find("auto f(cpp2::impl::in<int> x) -> void;") < cpp1.find("struct s {"));
    CHECK(cpp1.find("auto g(cpp2::impl::in<int> y = x) -> void;") > cpp1.find("struct s {"));
}

TEST_CASE("a concept in a namespace keeps its expression", "[emitter][review]") {
    const auto cpp1 = translated("n: namespace = { c: <T> concept = true; }\n");
    CHECK(cpp1.find("template<typename T> concept c = true;") != std::string::npos);
}
