#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.hpp"

namespace {

auto translate(const std::string& source, bool enabled = true)
    -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("trait"));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) { return issue.code == code && issue.span.begin.line == line; });
}

// Compiles `cpp1` with g++ and, when `run`, runs it; the output is the
// compiler's, or the program's when it compiled.
auto compile(const std::string& cpp1, std::string_view name, bool run)
    -> test_support::process_result {
    const auto scratch = test_support::scratch_directory("cppfrontier-trait");
    std::filesystem::create_directories(scratch);
    const auto source_path = scratch / (std::string{name} + ".cpp");
    const auto executable_path = scratch / name;
    test_support::write_file(source_path, cpp1);
    auto arguments = std::vector<std::string>{CPPFRONTIER_GXX, "-std=c++23", "-I",
                                              CPPFRONTIER_RUNTIME_INCLUDE};
    if (run) arguments.insert(arguments.end(), {"-o", executable_path.string()});
    else arguments.push_back("-fsyntax-only");
    arguments.push_back(source_path.string());
    const auto compiled = test_support::run_process(arguments, scratch, std::chrono::seconds{90});
    if (!run || compiled.timed_out || compiled.exit_status != 0) return compiled;
    return test_support::run_process({executable_path.string()}, scratch,
                                     std::chrono::seconds{10});
}

const auto shapes_source = std::string{
    "#include <iostream>\n"
    "Shape: @trait type = {\n"
    "    area: (this) -> double;\n"
    "    name: (this) -> std::string;\n"
    "    scale: (inout this, k: double);\n"
    "}\n"
    "Circle: @impl<Shape> @value type = {\n"
    "    r: double = 0;\n"
    "    operator=: (out this, r_: double) = { r = r_; }\n"
    "    area: (this) -> double = 3.0 * r * r;\n"
    "    name: (this) -> std::string = \"circle\";\n"
    "    scale: (inout this, k: double) = { r *= k; }\n"
    "}\n"
    "Square: @value type = {\n"
    "    s: double = 0;\n"
    "    operator=: (out this, s_: double) = { s = s_; }\n"
    "    area: (this) -> double = s * s;\n"
    "    name: (this) -> std::string = \"square\";\n"
    "    scale: (inout this, k: double) = { s *= k; }\n"
    "}\n"
    "Square_is_Shape: @impl<Shape, Square> type = { }\n"
    "describe: <T: Shape> (x: T) -> std::string = x.name() + \" \" + std::to_string(x.area());\n"
    "main: () = {\n"
    "    shapes: std::vector<dyn_Shape> = (Circle(1.0), Square(2.0));\n"
    "    for shapes do (inout s) { s.scale(2.0); }\n"
    "    copy_of_first: dyn_Shape = shapes[0];\n"
    "    for shapes do (s) { std::cout << describe(s) << \"\\n\"; }\n"
    "    std::cout << copy_of_first.name() << \" \" << copy_of_first.area() << \"\\n\";\n"
    "}\n"};

}

TEST_CASE("@trait generates a concept, an archetype and a dyn type", "[experiments][trait]") {
    const auto result = translate(shapes_source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    // The concept follows the type forward declarations.
    const auto concept_at = cpp1.find("\n#line 2 \"input.cpp2\"\n"
                                      "template<typename __cpp2_T> concept Shape = "
                                      "Shape_calls<__cpp2_T>::__cpp2_satisfied();\n");
    CHECK(concept_at != std::string::npos);
    CHECK(cpp1.find("template<typename __cpp2_T> struct Shape_calls;\n") < concept_at);
    // Each function is a member or a free function that UFCS reaches.
    CHECK(cpp1.find("requires (__cpp2_T const& __cpp2_self) { "
                    "{ CPP2_UFCS_NONLOCAL(area)(__cpp2_self) } -> std::convertible_to<double>; }")
          != std::string::npos);
    CHECK(cpp1.find("requires (__cpp2_T& __cpp2_self, double const& __cpp2_arg0) { "
                    "CPP2_UFCS_NONLOCAL(scale)(__cpp2_self, __cpp2_arg0); }") != std::string::npos);
    // The requirements are defined after every other type definition and
    // function declaration.
    const auto calls = cpp1.find("template<typename __cpp2_T> struct Shape_calls {");
    REQUIRE(calls != std::string::npos);
    CHECK(cpp1.find("struct Square_is_Shape {") < calls);
    CHECK(cpp1.find("struct Shape_archetype {\n[[nodiscard]] auto area() const& -> double;")
          != std::string::npos);
    CHECK(cpp1.find("template<Shape T> struct __cpp2_model : public __cpp2_interface {")
          != std::string::npos);
    CHECK(cpp1.find("[[nodiscard]] virtual auto area() const -> double = 0;") != std::string::npos);
    CHECK(cpp1.find("auto dyn_Shape::__cpp2_model<T>::area() const -> double "
                    "{ return Shape_calls<T>::__cpp2_area(__cpp2_value); }") != std::string::npos);
    CHECK(cpp1.find("dyn_Shape::dyn_Shape([[maybe_unused]] dyn_Shape const& that) : "
                    "__cpp2_self{CPP2_UFCS(__cpp2_clone)(") != std::string::npos);
    CHECK(cpp1.find("auto dyn_Shape::area() const& -> double "
                    "{ return CPP2_UFCS(area)((*cpp2::impl::assert_not_null(__cpp2_self))); }")
          != std::string::npos);
    // The trait is not a Cpp1 type.
    CHECK(cpp1.find("struct Shape;") == std::string::npos);
    CHECK(cpp1.find("struct Shape {") == std::string::npos);
}

TEST_CASE("@impl emits a static_assert after every definition", "[experiments][trait]") {
    const auto result = translate(shapes_source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    const auto main = cpp1.find("auto main() -> int  {");
    const auto circle_check =
        cpp1.find("static_assert(::Shape<::Circle>, \"Circle does not satisfy Shape\");");
    const auto square_check =
        cpp1.find("static_assert(::Shape<Square>, \"Square does not satisfy Shape\");");
    REQUIRE(main != std::string::npos);
    REQUIRE(circle_check != std::string::npos);
    REQUIRE(square_check != std::string::npos);
    CHECK(main < circle_check);
    CHECK(circle_check < square_check);
    // The marker type of the external form is an empty struct.
    CHECK(cpp1.find("struct Square_is_Shape {\n};") != std::string::npos);
}

TEST_CASE("a trait-constrained function template is checked against the archetype",
          "[experiments][trait]") {
    const auto result = translate(shapes_source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto definition = result.cpp1.find("auto describe(T const& x) -> std::string { return");
    const auto check = result.cpp1.find(
        "static_assert((static_cast<void>(&describe<::Shape_archetype>), true), "
        "\"the body of describe is checked against Shape_archetype\");");
    REQUIRE(definition != std::string::npos);
    REQUIRE(check != std::string::npos);
    CHECK(definition < check);

    SECTION("a function with other template parameters, a requires clause, or overloads is not checked") {
        const auto other = translate(
            "Shape: @trait type = { area: (this) -> double; }\n"
            "f: <T: Shape, U> (x: T, y: U) -> double = x.area();\n"
            "g: <T: Shape> (x: T) -> double requires (sizeof(T) > 1) = x.area();\n"
            "h: <T: Shape> (x: T) -> double = x.area();\n"
            "h: <T: Shape> (x: T, y: int) -> double = x.area() + y;\n"
            "k: <T: Shape> (x: T, y) -> double = x.area();\n");
        INFO(other.cpp1);
        REQUIRE_FALSE(other.has_errors());
        CHECK(other.cpp1.find("static_assert((static_cast<void>") == std::string::npos);
    }
}

TEST_CASE("a trait inside a namespace checks types and functions there", "[experiments][trait]") {
    const auto result = translate(
        "geo: namespace = {\n"
        "    Shape: @trait type = { area: (this) -> double; }\n"
        "    Circle: @impl<Shape> @value type = { area: (this) -> double = 1.0; }\n"
        "    twice: <T: Shape> (x: T) -> double = 2 * x.area();\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find(
              "static_assert(::geo::Shape<::geo::Circle>, \"Circle does not satisfy Shape\");")
          != std::string::npos);
    CHECK(result.cpp1.find("static_cast<void>(&twice<::geo::Shape_archetype>)") !=
          std::string::npos);
    CHECK(result.cpp1.find("struct dyn_Shape {") != std::string::npos);
}

TEST_CASE("@trait and @impl diagnose malformed traits and uses", "[experiments][trait]") {
    const auto result = translate(
        "Shape: @trait type = {\n"                       // 1
        "    area: (this) -> double;\n"                  // 2
        "    bad: (this) -> double = 1.0;\n"             // 3
        "    x: int = 0;\n"                              // 4
        "    stat: () -> int;\n"                         // 5
        "    ded: (this) -> _;\n"                        // 6
        "}\n"                                            // 7
        "Empty: @trait type = { }\n"                     // 8
        "Tpl: @trait <T> type = { f: (this) -> T; }\n"   // 9
        "C: @impl type = { }\n"                          // 10
        "D: @impl<Shape, Shape, Shape> type = { }\n"     // 11
        "E: @impl<Shape, C> type = { y: int = 0; }\n"    // 12
        "F: @impl<Shape> <T> type = { }\n"               // 13
        "use: (s: Shape) -> Shape = s;\n"                // 14
        "main: () = { v: Shape = (); }\n"               // 15
        "G: @impl<std::regular> type = { }\n"           // 16
        "Outer: type = {\n"                             // 17
        "    Inner: @trait type = { f: (this); }\n"     // 18
        "}\n");                                         // 19
    CHECK(has_diagnostic(result, "trait-function-body", 3));
    CHECK(has_diagnostic(result, "trait-member-kind", 4));
    CHECK(has_diagnostic(result, "trait-function-this", 5));
    CHECK(has_diagnostic(result, "trait-deduced-return", 6));
    CHECK(has_diagnostic(result, "trait-empty", 8));
    CHECK(has_diagnostic(result, "trait-template", 9));
    CHECK(has_diagnostic(result, "impl-arguments", 10));
    CHECK(has_diagnostic(result, "impl-arguments", 11));
    CHECK(has_diagnostic(result, "impl-external-member", 12));
    CHECK(has_diagnostic(result, "impl-template-type", 13));
    CHECK(has_diagnostic(result, "trait-as-type", 14));
    CHECK(has_diagnostic(result, "trait-as-type", 15));
    CHECK(has_diagnostic(result, "impl-not-trait", 16));
    CHECK(has_diagnostic(result, "trait-scope", 18));
    const auto as_type = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) { return issue.code == "trait-as-type"; });
    REQUIRE(as_type != result.diagnostics.end());
    CHECK(as_type->message == "'Shape' is a trait, not a type; use 'dyn_Shape' for a value of "
                              "any type satisfying it, or constrain a template parameter with it");
}

TEST_CASE("a malformed trait's functions are not reported as missing bodies",
          "[experiments][trait][hunt]") {
    const auto result = translate(
        "Sink: @trait type = {\n"                       // 1
        "    put: (inout this, s: int) -> int;\n"       // 2
        "    get: <T> (this) -> T;\n"                   // 3
        "    peek: (this, n: int = 1) -> int;\n"        // 4
        "}\n"
        "main: () = { }\n");
    CHECK(has_diagnostic(result, "trait-function-form", 3));
    CHECK(has_diagnostic(result, "trait-default-argument", 4));
    CHECK(result.diagnostics.size() == 2);
}

TEST_CASE("a program with a trait, dyn values and a generic function compiles and runs",
          "[experiments][trait][runtime]") {
    const auto result = translate(shapes_source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    if (std::string{CPPFRONTIER_GXX}.empty()) SKIP("g++ is unavailable");
    const auto run = compile(result.cpp1, "shapes", true);
    INFO(run.output);
    REQUIRE_FALSE(run.timed_out);
    REQUIRE(run.exit_status == 0);
    CHECK(run.output == "circle 12.000000\nsquare 16.000000\ncircle 12\n");
}

TEST_CASE("a type claiming @impl without the trait's functions fails to compile",
          "[experiments][trait][runtime]") {
    const auto result = translate(
        "Shape: @trait type = { area: (this) -> double; }\n"
        "Blob: @impl<Shape> @value type = { size: int = 0; }\n"
        "main: () = { }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    if (std::string{CPPFRONTIER_GXX}.empty()) SKIP("g++ is unavailable");
    const auto compiled = compile(result.cpp1, "bad-impl", false);
    INFO(compiled.output);
    REQUIRE_FALSE(compiled.timed_out);
    CHECK(compiled.exit_status != 0);
    CHECK(compiled.output.find("Blob does not satisfy Shape") != std::string::npos);
}

TEST_CASE("a generic body using more than its trait fails to compile", "[experiments][trait][runtime]") {
    const auto result = translate(
        "Shape: @trait type = { area: (this) -> double; }\n"
        "twice: <T: Shape> (x: T) -> double = x.area() + x.perimeter();\n"
        "main: () = { }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    if (std::string{CPPFRONTIER_GXX}.empty()) SKIP("g++ is unavailable");
    const auto compiled = compile(result.cpp1, "bad-body", false);
    INFO(compiled.output);
    REQUIRE_FALSE(compiled.timed_out);
    CHECK(compiled.exit_status != 0);
    CHECK(compiled.output.find("Shape_archetype") != std::string::npos);
    CHECK(compiled.output.find("perimeter") != std::string::npos);
}

TEST_CASE("a trait is resolved by name lookup", "[experiments][trait]") {
    const auto result = translate(
        "a: namespace = { Shape: @trait type = { area: (this) -> double; } }\n"
        "b: namespace = {\n"
        "    Shape: @value type = { area: (this) -> double = 1.0; }\n"
        "    f: (s: Shape) -> double = s.area();\n"
        "    Unit: @impl<a::Shape> @value type = { area: (this) -> double = 1.0; }\n"
        "}\n");
    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find("static_assert(::a::Shape<::b::Unit>, \"Unit does not satisfy Shape\");")
          != std::string::npos);
}

TEST_CASE("@trait and @impl are unsupported unless the experiment is enabled",
          "[experiments][trait]") {
    const auto result = translate(shapes_source, false);
    CHECK(has_diagnostic(result, "unsupported-metaclass", 2));
    CHECK(has_diagnostic(result, "unsupported-metaclass", 7));
    CHECK_FALSE(has_diagnostic(result, "trait-as-type", 1));
    CHECK(result.cpp1.find("concept Shape") == std::string::npos);
}
