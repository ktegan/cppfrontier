#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "test_support.hpp"

namespace {

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

auto translate(std::string_view source, bool enabled = true) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("adt"));
    return cppfrontier::translator{}.translate_source(std::string{source}, options);
}

auto find_diagnostic(const cppfrontier::translation_result& result, std::string_view code)
    -> const cppfrontier::diagnostic* {
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const cppfrontier::diagnostic& value) { return value.code == code; });
    return found == result.diagnostics.end() ? nullptr : &*found;
}

constexpr auto shape_source = std::string_view{
    "Shape: @adt type = {\n"
    "    Circle: (r: double);\n"
    "    Rect:   (w: double, h: double);\n"
    "    Empty:  ();\n"
    "    area: (this) -> double = {\n"
    "        if is_Circle() { return 3.0 * as_Circle().r * as_Circle().r; }\n"
    "        if is_Rect() { return as_Rect().w * as_Rect().h; }\n"
    "        return 0.0;\n"
    "    }\n"
    "}\n"};

}

TEST_CASE("@adt lowers cases to nested structs held in a variant", "[experiments][adt]") {
    const auto result = translate(shape_source);
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto has = [&](std::string_view fragment) {
        return result.cpp1.find(fragment) != std::string::npos;
    };
    CHECK(has("struct Circle { double r;"));
    CHECK(has("struct Rect { double w; double h;"));
    CHECK(has("struct Empty {"));
    CHECK(has("std::variant<Circle, Rect, Empty> _value;"));
    CHECK(has("Shape(Circle _case);"));
    CHECK(has("Shape(Empty _case);"));
    CHECK(has("[[nodiscard]] auto is_Circle() const& -> bool;"));
    CHECK(has("[[nodiscard]] auto as_Circle() const& -> Circle const&;"));
    CHECK(has("[[nodiscard]] auto as_Circle()& -> Circle&;"));
    CHECK(has("[[nodiscard]] auto operator==([[maybe_unused]] Shape const& that) const& -> bool = default;"));
    CHECK(has("[[nodiscard]] auto operator==([[maybe_unused]] Circle const& that) const& -> bool = default;"));
    CHECK(has("auto cpp2_adt_value() const& -> std::variant<Circle, Rect, Empty> const&;"));
    CHECK(has("Shape::Shape(Rect _case) : _value{cpp2::move(_case)} {"));
}

TEST_CASE("@adt values are built, copied, compared and inspected", "[experiments][adt]") {
    const auto result = translate(
        std::string{shape_source} +
        "describe: (s: Shape) -> std::string = inspect s -> std::string {\n"
        "    is Shape::Circle = \"circle \" + std::to_string((s as Shape::Circle).r);\n"
        "    is Shape::Rect = \"rect\";\n"
        "    is _ = \"other\";\n"
        "};\n"
        "main: () = {\n"
        "    shapes: std::vector<Shape> = (Shape::Circle(1.0), Shape::Rect(2.0, 3.0),"
        " Shape::Empty());\n"
        "    for shapes do (s) { std::cout << describe(s) << \" \" << s.area() << \"\\n\"; }\n"
        "    c: Shape = Shape::Circle(2.0);\n"
        "    copy := c;\n"
        "    c = Shape::Empty();\n"
        "    c.as_Empty();\n"
        "    std::cout << shapes[0].is_Circle() << shapes[0].as_Circle().r\n"
        "        << (shapes[0] == shapes[1]) << (shapes[0] == Shape::Circle(1.0))\n"
        "        << (copy == Shape::Circle(2.0)) << c.is_Empty() << (c is Shape::Empty)\n"
        "        << (c is Shape) << (c is Shape::Circle) << \"\\n\";\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(compile_and_run(result.cpp1, "adt") ==
          "circle 1.000000 3\nrect 6\nother 0\n110111110\n");
}

TEST_CASE("@adt diagnoses malformed, duplicate and missing cases", "[experiments][adt]") {
    SECTION("a data member is not a case") {
        const auto result = translate("S: @adt type = {\n    x: int;\n}\n");
        const auto* found = find_diagnostic(result, "adt-case-form");
        REQUIRE(found != nullptr);
        CHECK(found->span.begin.line == 2);
        CHECK(found->message == "an @adt case is a list of typed fields: 'x: (name: type, ...);'");
        CHECK(find_diagnostic(result, "adt-empty") == nullptr);
    }
    SECTION("a case has typed in fields") {
        CHECK(find_diagnostic(translate("S: @adt type = { A: (inout x: int); }\n"),
                              "adt-case-form") != nullptr);
        CHECK(find_diagnostic(translate("S: @adt type = { A: (x); }\n"),
                              "adt-case-form") != nullptr);
        CHECK(find_diagnostic(translate("S: @adt type = { A: (x: int) -> int; }\n"),
                              "adt-case-form") != nullptr);
        CHECK(find_diagnostic(translate("S: @adt type = { A: <T> (x: T); }\n"),
                              "adt-case-form") != nullptr);
    }
    SECTION("a duplicate case") {
        const auto result = translate("S: @adt type = {\n    A: (x: int);\n    A: ();\n}\n");
        const auto* found = find_diagnostic(result, "adt-duplicate-case");
        REQUIRE(found != nullptr);
        CHECK(found->span.begin.line == 3);
        CHECK(found->message == "'A' is already a case of S");
    }
    SECTION("no cases") {
        const auto result = translate("S: @adt type = { f: (this) = { } }\n");
        REQUIRE(find_diagnostic(result, "adt-empty") != nullptr);
    }
}

TEST_CASE("@adt is unsupported unless the experiment is enabled", "[experiments][adt]") {
    const auto result = translate(shape_source, false);
    const auto* unsupported = find_diagnostic(result, "unsupported-metaclass");
    REQUIRE(unsupported != nullptr);
    CHECK(unsupported->message == "'@adt' requires '-experiment=adt'");
}

TEST_CASE("the cases of an unsupported @adt are not reported as missing bodies",
          "[experiments][adt][hunt]") {
    const auto result = translate("S: @adt type = {\n    A: (x: int);\n    B: ();\n}\n"
                                  "T: @unknown type = { f: (this) -> int; }\n", false);
    REQUIRE(result.diagnostics.size() == 2);
    CHECK(result.diagnostics[0].message == "'@adt' requires '-experiment=adt'");
    CHECK(result.diagnostics[1].message == "'@unknown' is not supported");
}
