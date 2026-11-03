#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "test_support.hpp"

namespace {

// The tests' sources use `@adt`, so both enable experiment `adt`.
auto adt_options() -> cppfrontier::translation_options {
    auto options = cppfrontier::translation_options{};
    REQUIRE(options.experiments.enable("adt"));
    return options;
}

auto patterns_options() -> cppfrontier::translation_options {
    auto options = adt_options();
    REQUIRE(options.experiments.enable("patterns"));
    return options;
}

// Compiles and runs `cpp1`, returning the program's output, or "compile failed: ...".
auto compile_and_run(const std::string& cpp1, const std::string& name) -> std::string {
    const auto scratch = test_support::scratch_directory("cppfrontier-" + name);
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (name + ".cpp");
    const auto executable_path = scratch / name;
    test_support::write_file(cpp1_path, cpp1);
    const auto compiler_result = test_support::run_process({CPPFRONTIER_GXX, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    if (compiler_result.timed_out || compiler_result.exit_status != 0) {
        return "compile failed: " + compiler_result.output;
    }
    return test_support::run_process({executable_path.string()}, scratch,
                                     std::chrono::seconds{10}).output;
}

auto find_diagnostic(const cppfrontier::translation_result& result, std::string_view code)
    -> const cppfrontier::diagnostic* {
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const cppfrontier::diagnostic& value) { return value.code == code; });
    return found == result.diagnostics.end() ? nullptr : &*found;
}

constexpr auto types_source = std::string_view{
    "Shape: @adt type = {\n"
    "    Circle: (r: double);\n"
    "    Rect:   (w: double, h: double);\n"
    "    Empty:  ();\n"
    "}\n"
    "Color: @enum type = { red; green; }\n"};

constexpr auto describe_source = std::string_view{
    "describe: (s: Shape) -> std::string = inspect s -> std::string {\n"
    "    is Shape::Circle(r) if r > 1.0 = \"big circle \" + std::to_string(r);\n"
    "    is Shape::Circle(r) = \"circle \" + std::to_string(r);\n"
    "    is Shape::Rect(w, _) as rect = \"rect \" + std::to_string(w) + \" \""
    " + std::to_string(rect.h);\n"
    "    is Shape::Empty = \"empty\";\n"
    "};\n"};

}

TEST_CASE("patterns lower bindings, guards and alternatives into the inspect chain",
          "[experiments][patterns]") {
    const auto result = cppfrontier::translator{}.translate_source(
        std::string{types_source} + std::string{describe_source} +
        "kind: (s: Shape) -> int = inspect s -> int {\n"
        "    is Shape::Circle | Shape::Rect as x = 1;\n"
        "    is _ = 0;\n"
        "};\n"
        "sum: (p: std::pair<int, int>) -> int = inspect p -> int {\n"
        "    is (a, b) if a > b = a - b;\n"
        "    is (a, b) = a + b;\n"
        "};\n",
        patterns_options());
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto has = [&](std::string_view fragment) {
        return result.cpp1.find(fragment) != std::string::npos;
    };
    // A guard: bindings, then the guard, falling through to the next alternative.
    // The bindings are made where the subject converts, which a template may not.
    CHECK(has("if (cpp2::impl::is<Shape::Circle>(__cpp2_expr)) { if constexpr( requires{"
              "cpp2::impl::as<Shape::Circle>(__cpp2_expr);} ) if constexpr( !std::is_same_v<"
              "CPP2_TYPEOF(cpp2::impl::as<Shape::Circle>(__cpp2_expr)), cpp2::nonesuch_> ) { "
              "[[maybe_unused]] auto&& [r] = "
              "cpp2::impl::as<Shape::Circle>(__cpp2_expr); if (cpp2::impl::cmp_greater(r,1.0)) { "
              "if constexpr("));
    CHECK(has("\n    if (cpp2::impl::is<Shape::Circle>(__cpp2_expr)) { if constexpr( requires{"
              "cpp2::impl::as<Shape::Circle>(__cpp2_expr);} ) if constexpr( !std::is_same_v<"
              "CPP2_TYPEOF(cpp2::impl::as<Shape::Circle>(__cpp2_expr)), cpp2::nonesuch_> ) { "
              "[[maybe_unused]] auto&& [r] = "
              "cpp2::impl::as<Shape::Circle>(__cpp2_expr); if constexpr("));
    // A wildcard field and a binding of the matched case.
    CHECK(has("[[maybe_unused]] auto&& [w, __cpp2_unused_0] = cpp2::impl::as<Shape::Rect>"
              "(__cpp2_expr); [[maybe_unused]] auto&& rect = cpp2::impl::as<Shape::Rect>"
              "(__cpp2_expr); "));
    // Exhaustive over the cases: no `is _`, so the chain ends in `std::terminate()`.
    CHECK(has("return \"empty\"; else return std::string{}; else return std::string{}; } "
              "std::terminate();"));
    // Alternatives bind the subject itself.
    CHECK(has("if (cpp2::impl::is<Shape::Circle>(__cpp2_expr) || "
              "cpp2::impl::is<Shape::Rect>(__cpp2_expr)) { [[maybe_unused]] auto&& x = "
              "__cpp2_expr; "));
    // A tuple pattern destructures the subject and matches unconditionally.
    CHECK(has("{ [[maybe_unused]] auto&& [a, b] = __cpp2_expr; if (cpp2::impl::cmp_greater(a,b))"));
    CHECK(has("{ [[maybe_unused]] auto&& [a, b] = __cpp2_expr; if constexpr( requires{a + b;} )"));
    CHECK_FALSE(has("else if (cpp2::impl::is"));
}

TEST_CASE("patterns match, bind and guard at runtime", "[experiments][patterns]") {
    const auto result = cppfrontier::translator{}.translate_source(
        std::string{types_source} + std::string{describe_source} +
        "kind: (s: Shape) -> int = inspect s -> int {\n"
        "    is Shape::Circle | Shape::Rect as x = 1;\n"
        "    is Shape::Empty = 0;\n"
        "};\n"
        "name: (c: Color) -> std::string = inspect c -> std::string {\n"
        "    is (Color::red) = \"red\";\n"
        "    is (Color::green) = \"green\";\n"
        "};\n"
        "sum: (p: std::pair<int, int>) -> int = inspect p -> int {\n"
        "    is (a, b) if a > b = a - b;\n"
        "    is (a, b) = a + b;\n"
        "};\n"
        "main: () = {\n"
        "    shapes: std::vector<Shape> = (Shape::Circle(2.0), Shape::Circle(0.5),"
        " Shape::Rect(2.0, 3.0), Shape::Empty());\n"
        "    for shapes do (s) { std::cout << describe(s) << \" \" << kind(s) << \"\\n\"; }\n"
        "    std::cout << name(Color::red) << name(Color::green) << sum(std::pair(3, 1))"
        " << sum(std::pair(1, 3)) << \"\\n\";\n"
        "}\n",
        patterns_options());
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(compile_and_run(result.cpp1, "patterns") ==
          "big circle 2.000000 1\ncircle 0.500000 1\nrect 2.000000 3.000000 1\nempty 0\n"
          "redgreen24\n");
}

TEST_CASE("patterns require every case or a wildcard", "[experiments][patterns]") {
    const auto result = cppfrontier::translator{}.translate_source(
        std::string{types_source} +
        "f: (s: Shape) -> int = inspect s -> int {\n"
        "    is Shape::Circle(r) if r > 1.0 = 2;\n"
        "    is Shape::Circle = 1;\n"
        "};\n"
        "g: (c: Color) -> int = inspect c -> int { is (Color::red) = 1; };\n"
        "h: (x: int) -> int = inspect x -> int { is 1 = 1; };\n"
        "main: () = {\n"
        "    s := Shape::Empty();\n"
        "    _ = inspect s -> int { is Shape::Empty = 0; };\n"
        "    t: Shape = Shape::Empty();\n"
        "    _ = inspect t -> int { is Shape::Empty = 0; is Shape::Circle | Shape::Rect = 1; };\n"
        "}\n",
        patterns_options());
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE(result.diagnostics.size() == 4);
    const auto* shape = find_diagnostic(result, "inspect-not-exhaustive");
    REQUIRE(shape != nullptr);
    CHECK(shape->span.begin.line == 7);
    CHECK(shape->message ==
          "inspect of 'Shape' does not cover Shape::Rect, Shape::Empty - add alternatives or 'is _'");
    CHECK(result.diagnostics[1].code == "inspect-not-exhaustive");
    CHECK(result.diagnostics[1].span.begin.line == 11);
    CHECK(result.diagnostics[1].message ==
          "inspect of 'Color' does not cover Color::green - add alternatives or 'is _'");
    // A subject whose type is not a sum type, or is deduced, needs `is _` as before.
    CHECK(result.diagnostics[2].code == "invalid-inspect");
    CHECK(result.diagnostics[2].span.begin.line == 12);
    CHECK(result.diagnostics[3].code == "invalid-inspect");
    CHECK(result.diagnostics[3].span.begin.line == 15);
    CHECK(result.diagnostics[3].message ==
          "an inspect expression must have an `is _` match-anything wildcard alternative");
}

TEST_CASE("an enumerator written as a type-id is pointed to its value form",
          "[experiments][patterns][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        std::string{types_source} +
        "g: (c: Color) -> int = inspect c -> int { is Color::red = 1; is (Color::green) = 2; };\n",
        patterns_options());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].message ==
          "inspect of 'Color' does not cover Color::red - add alternatives or 'is _'; "
          "'is Color::red' tests a type, so write 'is (Color::red)' to match the value");
}

TEST_CASE("patterns are rejected and inspect is unchanged without the experiment",
          "[experiments][patterns]") {
    const auto plain = std::string{types_source} +
        "f: (s: Shape) -> int = inspect s -> int {\n"
        "    is Shape::Circle = 1;\n"
        "    is _ = 0;\n"
        "};\n";
    const auto off = cppfrontier::translator{}.translate_source(plain, adt_options());
    const auto on = cppfrontier::translator{}.translate_source(plain, patterns_options());
    REQUIRE_FALSE(off.has_errors());
    REQUIRE_FALSE(on.has_errors());
    CHECK(off.cpp1.find("else return 0;") != std::string::npos);
    CHECK(off.cpp1.find("std::terminate") == std::string::npos);
    CHECK(on.cpp1.find("else return 0;") == std::string::npos);
    CHECK(on.cpp1.find("\n    return 0;") != std::string::npos);

    for (const auto* alternative : {"is Shape::Circle(r) = r;", "is Shape::Circle | Shape::Empty = 1.0;",
                                    "is Shape::Circle as c = c.r;", "is Shape::Circle if true = 1.0;",
                                    "is (a, b) if a > b = 1.0;"}) {
        CAPTURE(alternative);
        const auto result = cppfrontier::translator{}.translate_source(
            std::string{types_source} + "f: (s: Shape) -> double = inspect s -> double {\n    " +
            alternative + "\n    is _ = 0.0;\n};\n", adt_options());
        const auto* found = find_diagnostic(result, "invalid-inspect");
        REQUIRE(found != nullptr);
        CHECK(found->span.begin.line == 8);
        CHECK(found->message == "destructuring, alternatives, bindings and guards in an inspect "
                                "alternative require '-experiment=patterns'");
    }
    const auto missing = cppfrontier::translator{}.translate_source(
        std::string{types_source} +
        "f: (s: Shape) -> int = inspect s -> int { is Shape::Circle = 1; };\n", adt_options());
    REQUIRE(find_diagnostic(missing, "invalid-inspect") != nullptr);
    CHECK(find_diagnostic(missing, "invalid-inspect")->message ==
          "an inspect expression must have an `is _` match-anything wildcard alternative");
}

TEST_CASE("patterns need a wildcard wherever an inspect may match nothing",
          "[experiments][patterns]") {
    const auto result = cppfrontier::translator{}.translate_source(
        std::string{types_source} +
        "Flags: @flag_enum<u8> type = { a; b; }\n"
        "f: (x: int) pre(inspect x -> bool { is (1) = true; }) = { }\n"
        "g: (x: int, y: int = inspect x -> int { is (1) = 1; }) -> int = y;\n"
        "h: (x: int) -> int = inspect x -> int { is _ if x > 5 = 1; };\n"
        "i: (p: std::pair<int, int>) -> int = inspect p -> int { is (a, b) if a > b = 1; };\n"
        "j: (x: Flags) -> int = inspect x -> int {\n"
        "    is (Flags::a) = 1; is (Flags::b) = 2; is (Flags::none) = 0;\n"
        "};\n"
        "k: (s: Shape) -> int = inspect s -> int {\n"
        "    is Shape::Circle | Shape::Rect = 1; is Shape::Empty if true = 0;\n"
        "};\n",
        patterns_options());
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE(result.diagnostics.size() == 6);
    for (auto index = std::size_t{0}; index < 5; ++index) {
        CAPTURE(index);
        CHECK(result.diagnostics[index].code == "invalid-inspect");
        CHECK(result.diagnostics[index].span.begin.line == 8 + index);
    }
    // A guarded case is not covered.
    CHECK(result.diagnostics[5].code == "inspect-not-exhaustive");
    CHECK(result.diagnostics[5].span.begin.line == 15);
    CHECK(result.diagnostics[5].message ==
          "inspect of 'Shape' does not cover Shape::Empty - add alternatives or 'is _'");
}

TEST_CASE("patterns prove an inspect exhaustive wherever it appears", "[experiments][patterns]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "E: @enum type = { a; b; }\n"
        "N: namespace = {\n"
        "    f: (x: E) -> int = inspect x -> int { is (E::a) = 1; is (E::b) = 2; };\n"
        "}\n"
        "g: (x: E) pre(inspect x -> bool { is (E::a) | (E::b) = true; }) -> int = {\n"
        "    l := :(y: E) -> int = inspect y -> int { is (E::a) | (E::b) = 3; };\n"
        "    return l(x);\n"
        "}\n"
        "h: (x: int) -> int = inspect x -> int { is _ as v = v; };\n"
        "i: (p: std::pair<int, int>) -> int = inspect p -> int { is (a, b) as q = a + b; };\n",
        patterns_options());
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    CHECK(result.diagnostics.empty());
}

TEST_CASE("patterns bindings hide outer names", "[experiments][patterns]") {
    auto options = patterns_options();
    REQUIRE(options.experiments.enable("use-after-move"));
    REQUIRE(options.experiments.enable("checked-arithmetic"));
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () = {\n"
        "    p: std::pair<std::string, int> = (\"hello\", 1);\n"
        "    a: std::string;\n"
        "    b: std::string = \"x\";\n"
        "    c := (move b);\n"
        "    r := inspect p -> std::string { is (a, b) = a; };\n"
        "    a = r;\n"
        "    i: int = 1;\n"
        "    d: std::pair<double, double> = (0.5, 0.25);\n"
        "    s := inspect d -> double { is (i, _) if i - 1.0 < 0.0 = i - 1.0; is _ as i = 0.0; };\n"
        "}\n",
        options);
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("checked_sub") == std::string::npos);
}

TEST_CASE("@print prints beside an exhaustive inspect without a wildcard",
          "[experiment][patterns][hunt]") {
    const auto source = std::string{
        "V: @print type = { x: int = 0; }\n"
        "E: @enum type = { a; b; }\n"
        "f: (e: E) -> int = inspect e -> int { is (E::a) = 1; is (E::b) = 2; };\n"};
    const auto result = cppfrontier::translator{}.translate_source(source, patterns_options());
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.printed.find("V:/* @print */ type =") != std::string::npos);
}

TEST_CASE("patterns prove an inspect of this exhaustive in a type with constructors",
          "[experiment][patterns][hunt]") {
    const auto source = std::string{
        "ev: @adt @value type = {\n"
        "    a: (v: int);\n"
        "    b: ();\n"
        "    get: (this) -> int = inspect this -> int { is ev::a(v) = v; is ev::b = 0; };\n"
        "}\n"
        "light: @enum type = {\n"
        "    red; green;\n"
        "    go: (this) -> bool = inspect this -> bool { is (light::red) = false; is (light::green) = true; };\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source, patterns_options());
    CHECK(result.diagnostics.empty());
}
