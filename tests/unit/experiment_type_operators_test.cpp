#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

auto translate(const std::string& source, bool enabled = true) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("type-operators"));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line, std::uint32_t column) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& issue) {
        return issue.code == code && issue.span.begin.line == line &&
            issue.span.begin.column == column;
    });
}

auto contains(const std::string& text, std::string_view fragment) -> bool {
    return text.find(fragment) != std::string::npos;
}

auto run_program(const std::string& source, std::string_view name) -> std::string {
    const auto translation = translate(source);
    INFO(translation.cpp1);
    for (const auto& issue : translation.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(translation.has_errors());
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-type-operators");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (std::string{name} + ".cpp");
    const auto executable_path = scratch / name;
    test_support::write_file(cpp1_path, translation.cpp1);
    const auto compiled = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
        scratch, std::chrono::seconds{120});
    INFO("compiler output: " << compiled.output);
    REQUIRE(compiled.exit_status == 0);
    const auto ran = test_support::run_process({executable_path.string()}, scratch,
                                               std::chrono::seconds{10});
    REQUIRE_FALSE(ran.timed_out);
    REQUIRE(ran.exit_status == 0);
    return ran.output;
}

}

TEST_CASE("a union type lowers to std::variant in every type position",
          "[experiments][type-operators]") {
    const auto result = translate(
        "f: (x: int | std::string) -> int | std::string = x;\n"
        "g: (inout x: int | bool | double) = {}\n"
        "main: () = {\n"
        "    x: int | std::string = \"hi\";\n"
        "    v: std::vector<type int | std::string> = ();\n"
        "    p: *(int | std::string) = x&;\n"
        "    q: *int | std::string = \"s\";\n"
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result.cpp1, "auto f(cpp2::impl::in<std::variant<int, std::string>> x) "
                                "-> std::variant<int, std::string>"));
    CHECK(contains(result.cpp1, "auto g(std::variant<int, bool, double>& x) -> void"));
    CHECK(contains(result.cpp1, "std::variant<int, std::string> x {\"hi\"};"));
    CHECK(contains(result.cpp1, "std::vector<std::variant<int, std::string>> v {};"));
    CHECK(contains(result.cpp1, "std::variant<int, std::string>* p {&(x)};"));
    // `|` binds loosest: `*` applies to one operand.
    CHECK(contains(result.cpp1, "std::variant<int*, std::string> q {\"s\"};"));
}

TEST_CASE("nested unions flatten and duplicate alternatives are dropped",
          "[experiments][type-operators]") {
    const auto result = translate(
        "main: () = {\n"
        "    a: *(int | bool) | std::string = nullptr;\n"
        "    b: int | (bool | std::string) | int = 1;\n"
        "    c: i32 | int = 1;\n"
        "    d: int | bool | bool | int = 1;\n"
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result.cpp1, "std::variant<std::variant<int, bool>*, std::string> a {nullptr};"));
    CHECK(contains(result.cpp1, "std::variant<int, bool, std::string> b {1};"));
    CHECK(contains(result.cpp1, "int c {1};"));
    CHECK(contains(result.cpp1, "std::variant<int, bool> d {1};"));
}

TEST_CASE("a union holds each alternative and inspect and is/as see them",
          "[experiments][type-operators]") {
    const auto output = run_program(
        "#include <iostream>\n"
        "#include <string>\n"
        "describe: (v: int | std::string) -> std::string =\n"
        "    inspect v -> std::string {\n"
        "        is int = \"int \" + std::to_string(v as int);\n"
        "        is std::string = \"string \" + (v as std::string);\n"
        "        is _ = \"?\";\n"
        "    };\n"
        "main: () = {\n"
        "    x: int | std::string = \"hi\";\n"
        "    std::cout << describe(x) << \"\\n\";\n"
        "    x = 42;\n"
        "    std::cout << describe(x) << \" \" << (x is int) << (x is std::string) << \"\\n\";\n"
        "    v: std::vector<type int | std::string> = (1, \"two\");\n"
        "    for v do (e) std::cout << describe(e) << \"\\n\";\n"
        "}\n",
        "union-runtime");
    CHECK(output == "string hi\nint 42 10\nint 1\nstring two\n");
}

TEST_CASE("intersection and union constraints lower to requires clauses",
          "[experiments][type-operators]") {
    const auto result = translate(
        "small: <T> concept = sizeof(T) <= 4u;\n"
        "f: <T: std::integral & small> (x: T) -> T = x;\n"
        "g: <T: std::integral | std::floating_point, U: std::integral> (x: T, y: U) -> T = x;\n"
        "h: (x: _ is std::integral & small) -> int = 1;\n"
        "k: (move x: _ is std::integral | std::floating_point) -> int = 2;\n"
        "m: (x: _ is std::integral & small = 1) -> int = 3;\n"
        "Box: <T: std::integral & small> type = { public value: T = 0; }\n"
        "Pair: <T: std::integral & small> type requires sizeof(T) >= 2 = { public value: T = 0; }\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result.cpp1, "template<typename T> [[nodiscard]] auto f(T const& x) -> T "
                                "requires (std::integral<T> && small<T>);"));
    CHECK(contains(result.cpp1, "template<typename T, std::integral U> [[nodiscard]] auto "
                                "g(T const& x, U const& y) -> T requires (std::integral<T> || "
                                "std::floating_point<T>);"));
    CHECK(contains(result.cpp1, "auto h(auto const& x) -> int requires "
                                "(std::integral<CPP2_TYPEOF(x)> && small<CPP2_TYPEOF(x)>);"));
    CHECK(contains(result.cpp1, "auto k(auto&& x) -> int requires "
                                "((std::integral<CPP2_TYPEOF(x)> || std::floating_point<CPP2_TYPEOF(x)>) "
                                "&& !std::is_lvalue_reference_v<decltype(x)>);"));
    CHECK(contains(result.cpp1, "template<typename __cpp2_default_x = std::remove_cvref_t<decltype(1)>> "
                                "[[nodiscard]] auto m(__cpp2_default_x const& x = 1) -> int requires "
                                "(std::integral<__cpp2_default_x> && small<__cpp2_default_x>);"));
    CHECK(contains(result.cpp1, "template<typename T> requires (std::integral<T> && small<T>) "
                                "struct Box"));
    CHECK(contains(result.cpp1, "template<typename T> requires ((std::integral<T> && small<T>) && "
                                "cpp2::impl::cmp_greater_eq(sizeof(T),2)) struct Pair"));
}

TEST_CASE("constrained templates instantiate for the types that satisfy them",
          "[experiments][type-operators]") {
    const auto output = run_program(
        "#include <iostream>\n"
        "#include <concepts>\n"
        "small: <T> concept = sizeof(T) <= 4u;\n"
        "twice: <T: std::integral & small> (x: T) -> T = x * 2;\n"
        "twice: <T: std::floating_point | std::same_as<longlong>> (x: T) -> T = x + x;\n"
        "which: (x: _ is std::integral & small) -> int = 1;\n"
        "which: (x: _ is std::floating_point | std::same_as<longlong>) -> int = 2;\n"
        "Box: <T: std::integral & small> type = { public value: T = 0; }\n"
        "main: () = {\n"
        "    big: longlong = 5;\n"
        "    b: Box<i16> = ();\n"
        "    std::cout << twice(21) << \" \" << twice(1.5) << \" \" << twice(big) << \" \"\n"
        "              << which(7) << which(2.0) << which(big) << \" \" << b.value << \"\\n\";\n"
        "}\n",
        "constraint-runtime");
    CHECK(output == "42 3 10 122 0\n");
}

TEST_CASE("type operators need two operands and '&' needs a constraint position",
          "[experiments][type-operators]") {
    const auto result = translate(
        "f: (x: int | ) = {}\n"
        "g: (x: int & std::string) = {}\n"
        "h: (x: std::vector<type int & bool>) -> int = 0;\n"
        "k: <T: std::integral & > (x: T) = {}\n"
        "m: (x: _ is std::integral & std::copyable) = {}\n"
        "n: () -> int | = 0;\n");
    CHECK(has_diagnostic(result, "type-operator-single", 1, 8));
    CHECK(has_diagnostic(result, "type-operator-constraint", 2, 8));
    CHECK(has_diagnostic(result, "type-operator-constraint", 3, 25));
    CHECK(has_diagnostic(result, "type-operator-single", 4, 8));
    CHECK(has_diagnostic(result, "type-operator-single", 6, 10));
    CHECK_FALSE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) { return issue.span.begin.line == 5; }));
    const auto single = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) { return issue.code == "type-operator-single"; });
    REQUIRE(single != result.diagnostics.end());
    CHECK(single->message == "the type operator '|' needs a type on both sides");
    const auto constraint = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) { return issue.code == "type-operator-constraint"; });
    REQUIRE(constraint != result.diagnostics.end());
    CHECK(constraint->message ==
          "'&' combines constraints, not types: an intersection of types has no Cpp1 lowering, "
          "so it is allowed only in a template parameter or '_ is' constraint");
}

TEST_CASE("a template argument with '|' or '&' is an expression unless marked 'type'",
          "[experiments][type-operators][review]") {
    const auto result = translate(
        "A: int == 2;\n"
        "B: int == 4;\n"
        "Circle: @struct type = { r: int = 1; }\n"
        "main: () = {\n"
        "    a: std::array<int, A | 1> = ();\n"
        "    d: std::bitset<A | B> = ();\n"
        "    e: std::bitset<A & B> = ();\n"
        "    s: std::vector<type Circle | std::string> = ();\n"
        "    b: std::bitset<A & 6> = ();\n"
        "    c: std::array<int, true | false> = ();\n"
        "    v: std::vector<type int | std::string> = ();\n"
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result.cpp1, "std::array<int, A | 1> a {};"));
    CHECK(contains(result.cpp1, "std::bitset<A & 6> b {};"));
    CHECK(contains(result.cpp1, "std::array<int, true | false> c {};"));
    CHECK(contains(result.cpp1, "std::vector<std::variant<int, std::string>> v {};"));
    CHECK(contains(result.cpp1, "std::bitset<A | B> d {};"));
    CHECK(contains(result.cpp1, "std::bitset<A & B> e {};"));
    CHECK(contains(result.cpp1, "std::vector<std::variant<Circle, std::string>> s {};"));
}

TEST_CASE("an unmarked template argument combining a type keyword is diagnosed",
          "[experiments][type-operators][review]") {
    const auto result = translate("main: () = { v: std::vector<int | std::string> = (); }\n");
    CHECK(has_diagnostic(result, "type-operator-argument", 1, 29));
}

TEST_CASE("without the experiment type operators are diagnosed",
          "[experiments][type-operators]") {
    const auto source =
        "f: <T: std::integral & std::copyable> (x: T) -> T = x;\n"
        "main: () = {\n"
        "    x: int | std::string = \"hi\";\n"
        "}\n";
    const auto result = translate(source, false);
    CHECK(has_diagnostic(result, "type-operator-experiment", 1, 8));
    CHECK(has_diagnostic(result, "type-operator-experiment", 3, 8));
    CHECK_FALSE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) { return issue.code == "unexpected-type-token"; }));
    CHECK(contains(translate(source).cpp1, "std::variant<int, std::string> x {\"hi\"};"));
}

TEST_CASE("a type alias may name a union", "[experiments][type-operators]") {
    const auto result = translate(
        "U: type == int | std::string;\n"
        "P: <A, B> type == A | B | std::monostate;\n"
        "main: () = {\n"
        "    L: type == int | char;\n"
        "    u: U = 1;\n"
        "}\n");
    CHECK(contains(result.cpp1, "using U = std::variant<int, std::string>;"));
    CHECK(contains(result.cpp1, "template<typename A, typename B> using P = "
                                "std::variant<A, B, std::monostate>;"));
    CHECK(contains(result.cpp1, "using L = std::variant<int, char>;"));
    CHECK(has_diagnostic(translate("X: type == int & long;\n"), "type-operator-constraint", 1, 12));
}

TEST_CASE("an operator constraint on a local or a pack is checked",
          "[experiments][type-operators]") {
    const auto result = translate(
        "f: <Ts...: std::integral | std::floating_point> (xs...: Ts) -> int = 0;\n"
        "main: () = {\n"
        "    l: _ is std::integral | std::floating_point = 3;\n"
        "    std::cout << l;\n"
        "}\n");
    CHECK(contains(result.cpp1, "requires (((std::integral<Ts> || std::floating_point<Ts>) && ...))"));
    CHECK(contains(result.cpp1, "auto l {3};\n"
                                "    static_assert(std::integral<CPP2_TYPEOF(l)> || "
                                "std::floating_point<CPP2_TYPEOF(l)>);"));
    const auto not_concept = translate(
        "Plain: type = { }\n"
        "g: <T: std::integral & Plain> (x: T) = {}\n");
    CHECK(has_diagnostic(not_concept, "constraint-not-concept", 2, 24));
}

TEST_CASE("a constraint diagnostic names the template parameter it constrains",
          "[experiments][type-operators][review]") {
    const auto result = translate(
        "X: type = { }\n"
        "C1: <T> concept = true;\n"
        "f: <T: X & C1> (t: T) = { }\n"
        "g: (x: _ is X & C1) = { }\n");
    const auto message = [&](std::uint32_t line) {
        const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& issue) {
                return issue.code == "constraint-not-concept" && issue.span.begin.line == line;
            });
        return found == result.diagnostics.end() ? std::string{} : found->message;
    };
    CHECK(message(3) == "'X' is not a concept, so it cannot constrain 'T'");
    CHECK(message(4) == "'X' is not a concept, so it cannot constrain '_'");
}

TEST_CASE("without the experiment, a type operator is an error that names it",
          "[experiments][type-operators][review]") {
    for (const auto* source : {
             "f: (x: _ is std::integral | std::floating_point) -> int = x;\n",
             "g: <T: std::integral | std::floating_point> (x: T) -> int = x;\n",
             "h: (x: _ is std::integral & std::signed_integral) -> int = x;\n",
             "x: std::integral | float = 1;\n",
             "main: () = { y: int & long = 2; _ = y; }\n",
             "U: type == int | float;\n"}) {
        INFO(source);
        const auto result = translate(source, false);
        CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& issue) {
                return issue.code == "type-operator-experiment" &&
                    issue.message.ends_with("requires '-experiment=type-operators'");
            }));
    }
    CHECK_FALSE(translate("b: std::bitset<1 | 2> = ();\n", false).has_errors());
    CHECK(translate("main: () = { y: int & long = 2; _ = y; }\n").has_errors());
}

TEST_CASE("a trailing '&' outside a constraint is a Cpp1 reference",
          "[experiments][type-operators][hunt]") {
    const auto result = translate(
        "f: (copy x: const int&) -> int = x;\n"
        "g: (x: int | std::string&) = {}\n"
        "k: <T: std::integral &> (x: T) = {}\n"
        "main: () = { a := 1; r: int& = a; }\n");
    CHECK(has_diagnostic(result, "cpp1-reference-type", 1, 13));
    CHECK(has_diagnostic(result, "cpp1-reference-type", 2, 14));
    CHECK(has_diagnostic(result, "type-operator-single", 3, 8));
    CHECK(has_diagnostic(result, "cpp1-reference-type", 4, 25));
    CHECK(result.diagnostics.size() == 4);
}

TEST_CASE("a template parameter of a Cpp1 reference type is rejected",
          "[experiments][type-operators][hunt]") {
    const auto result = translate("k: <T: std::integral &> (x: T) = {}\n"
                                  "n: <N: int&> () = {}\n", false);
    CHECK(has_diagnostic(result, "cpp1-reference-type", 1, 8));
    CHECK(has_diagnostic(result, "cpp1-reference-type", 2, 8));
    CHECK(result.diagnostics.size() == 2);
}
