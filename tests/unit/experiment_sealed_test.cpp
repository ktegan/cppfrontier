#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "test_support.hpp"

namespace {

auto sealed_options() -> cppfrontier::translation_options {
    auto options = cppfrontier::translation_options{};
    REQUIRE(options.experiments.enable("sealed"));
    return options;
}

auto translate(const std::string& source) -> cppfrontier::translation_result {
    return cppfrontier::translator{}.translate_source(source, sealed_options());
}

struct compile_result final {
    bool compiled = false;
    std::string output;
};

// Compiles `cpp1`, and runs it when it compiles; `output` is the compiler's
// output otherwise.
auto compile_and_run(const std::string& cpp1, const std::string& name) -> compile_result {
    const auto scratch = test_support::scratch_directory("cppfrontier-" + name);
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (name + ".cpp");
    const auto executable_path = scratch / name;
    test_support::write_file(cpp1_path, cpp1);
    const auto compiler_result = test_support::run_process({CPPFRONTIER_GXX, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    if (compiler_result.timed_out || compiler_result.exit_status != 0) {
        return {false, compiler_result.output};
    }
    return {true, test_support::run_process({executable_path.string()}, scratch,
                                            std::chrono::seconds{10}).output};
}

auto find_diagnostic(const cppfrontier::translation_result& result, std::string_view code)
    -> const cppfrontier::diagnostic* {
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const cppfrontier::diagnostic& value) { return value.code == code; });
    return found == result.diagnostics.end() ? nullptr : &*found;
}

constexpr auto shapes_source = std::string_view{
    "Shape: @sealed @polymorphic_base type = {\n"
    "    area: (virtual this) -> double;\n"
    "}\n"
    "Circle: type = {\n"
    "    this: Shape = ();\n"
    "    r: double;\n"
    "    operator=: (out this, r_: double) = { r = r_; }\n"
    "    area: (override this) -> double = 3.0 * r * r;\n"
    "}\n"
    "Square: type = {\n"
    "    this: Shape = ();\n"
    "    s: double;\n"
    "    operator=: (out this, s_: double) = { s = s_; }\n"
    "    area: (override this) -> double = s * s;\n"
    "}\n"};

constexpr auto describe_source = std::string_view{
    "name: (s: Shape) -> std::string = inspect s -> std::string {\n"
    "    is Circle = \"circle\";\n"
    "    is Square = \"square\";\n"
    "};\n"
    "name_ptr: (s: *Shape) -> std::string = inspect s* -> std::string {\n"
    "    is Square = \"square\";\n"
    "    is Circle = \"circle\";\n"
    "};\n"};

constexpr auto main_source = std::string_view{
    "main: () = {\n"
    "    c: Circle = (1.0);\n"
    "    q: Square = (2.0);\n"
    "    std::cout << name(c) << \" \" << name(q) << \" \" << name_ptr(q&) << \" \""
    " << q.area() << \"\\n\";\n"
    "}\n"};

}

TEST_CASE("@sealed makes the base's constructors private with its derived types as friends",
          "[experiments][sealed]") {
    const auto result = translate(std::string{shapes_source});
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE(result.diagnostics.empty());
    const auto has = [&](std::string_view fragment) {
        return result.cpp1.find(fragment) != std::string::npos;
    };
    CHECK(has("virtual ~Shape() noexcept { }\n"
              "private: Shape() = default;\n"
              "Shape(Shape const&) = delete;\n"
              "friend Circle; friend Square;\n"
              "public: auto operator=(Shape const&) -> void = delete;\n"
              "};"));
    CHECK(has("Circle::Circle(cpp2::impl::in<double> r_) : Shape{}, r{r_} {"));

    // A user-written constructor is private too, and `@interface` composes.
    const auto written = translate(
        "geo: namespace = {\n"
        "    Shape: @sealed @polymorphic_base type = {\n"
        "        id: int;\n"
        "        operator=: (out this) = { id = 7; }\n"
        "        area: (virtual this) -> double;\n"
        "    }\n"
        "    Circle: type = { this: Shape = (); area: (override this) -> double = 1.0; }\n"
        "}\n"
        "Base: @sealed @interface type = { f: (this); }\n"
        "Leaf: type = { this: Base = (); f: (override this) = { } }\n");
    for (const auto& diagnostic : written.diagnostics) INFO(diagnostic.message);
    INFO(written.cpp1);
    REQUIRE(written.diagnostics.empty());
    CHECK(written.cpp1.find("private: int id;\nexplicit Shape();\n"
                            "public: [[nodiscard]] virtual auto area() const -> double = 0;\n"
                            "virtual ~Shape() noexcept { }\n"
                            "private: Shape(Shape const&) = delete;\n"
                            "friend Circle;\n") != std::string::npos);
    CHECK(written.cpp1.find("private: Base() = default;\n"
                            "Base([[maybe_unused]] Base const& that);\n"
                            "Base([[maybe_unused]] Base&& that) noexcept;\n"
                            "friend Leaf;\n"
                            "protected: auto operator=([[maybe_unused]] Base const& that) -> Base&;\n")
          != std::string::npos);
}

TEST_CASE("@sealed hierarchies run with an exhaustive inspect and reject outside derivation",
          "[experiments][sealed]") {
    const auto result = cppfrontier::translator{}.translate_source(
        std::string{shapes_source} + std::string{describe_source} + std::string{main_source},
        sealed_options());
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE(result.diagnostics.empty());
    CHECK(result.cpp1.find("std::terminate();") != std::string::npos);
    const auto run = compile_and_run(result.cpp1, "sealed");
    INFO(run.output);
    REQUIRE(run.compiled);
    CHECK(run.output == "circle square square 4\n");

    // A derived type that is not a friend, as one in another translation unit,
    // cannot construct its base.
    const auto other = compile_and_run(result.cpp1 +
        "struct Other : Shape { auto area() const -> double override { return 0; } };\n"
        "auto make_other() -> Other { return Other{}; }\n", "sealed_other");
    INFO(other.output);
    REQUIRE_FALSE(other.compiled);
    CHECK(other.output.find("Shape::Shape()") != std::string::npos);
    CHECK(other.output.find("is private within this context") != std::string::npos);
}

TEST_CASE("@sealed inspect lists the missing derived types",
          "[experiments][sealed]") {
    const auto source = std::string{shapes_source} +
        "name: (s: Shape) -> std::string = inspect s -> std::string {\n"
        "    is Circle = \"circle\";\n"
        "};\n"
        "kind: (s: *Shape) -> int = inspect s* -> int { is Circle = 1; is Square = 2; };\n"
        "deduced: (s: *Shape) -> int = inspect s -> int { is Circle = 1; is Square = 2; };\n";
    const auto on = translate(source);
    for (const auto& diagnostic : on.diagnostics) INFO(diagnostic.message);
    REQUIRE(on.diagnostics.size() == 2);
    CHECK(on.diagnostics[0].code == "inspect-not-exhaustive");
    CHECK(on.diagnostics[0].span.begin.line == 16);
    CHECK(on.diagnostics[0].message ==
          "inspect of 'Shape' does not cover Square - add alternatives or 'is _'");
    // A pointer subject is not the sealed type; `s*` is.
    CHECK(on.diagnostics[1].code == "invalid-inspect");
    CHECK(on.diagnostics[1].span.begin.line == 20);
}

TEST_CASE("@sealed is unsupported without the experiment", "[experiments][sealed]") {
    const auto off = cppfrontier::translator{}.translate_source(std::string{shapes_source});
    REQUIRE(off.has_errors());
    CHECK(find_diagnostic(off, "unsupported-metaclass") != nullptr);
    CHECK(off.cpp1.empty());

    // With patterns alone, a @sealed type is not a sum type.
    auto options = cppfrontier::translation_options{};
    REQUIRE(options.experiments.enable("patterns"));
    const auto patterns = cppfrontier::translator{}.translate_source(
        std::string{shapes_source} + std::string{describe_source}, options);
    CHECK(find_diagnostic(patterns, "unsupported-metaclass") != nullptr);
    CHECK(find_diagnostic(patterns, "inspect-not-exhaustive") == nullptr);
    CHECK(std::count_if(patterns.diagnostics.begin(), patterns.diagnostics.end(),
        [](const cppfrontier::diagnostic& value) { return value.code == "invalid-inspect"; }) == 2);
}

TEST_CASE("@sealed counts a derived type spelling its base qualified",
          "[experiments][sealed]") {
    // `this: geo::Shape` in Shape's scope is a case and a friend.
    const auto result = translate(
        "geo: namespace = {\n"
        "    Shape: @sealed @polymorphic_base type = { area: (virtual this) -> double; }\n"
        "    Circle: type = { this: Shape = (); area: (override this) -> double = 1.0; }\n"
        "    Square: type = { this: geo::Shape = (); area: (override this) -> double = 2.0; }\n"
        "    name: (s: Shape) -> int = inspect s -> int { is Circle = 1; };\n"
        "}\n"
        "other: namespace = {\n"
        "    Shape: @polymorphic_base type = { area: (virtual this) -> double; }\n"
        "    Disc: type = { this: Shape = (); area: (override this) -> double = 1.0; }\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code == "inspect-not-exhaustive");
    CHECK(result.diagnostics[0].message ==
          "inspect of 'Shape' does not cover Square - add alternatives or 'is _'");
}

TEST_CASE("@sealed diagnoses non-polymorphic, template, childless and misplaced types",
          "[experiments][sealed]") {
    const auto result = translate(
        std::string{shapes_source} +
        "Plain: @sealed type = { x: int; }\n"
        "Lonely: @sealed @interface type = { f: (this); }\n"
        "Gen: @sealed @polymorphic_base <T> type = { f: (virtual this); }\n"
        "ns: namespace = { Odd: type = { this: Shape = (); area: (override this) -> double"
        " = 3.0; } }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE(result.diagnostics.size() == 6);
    const auto* plain = find_diagnostic(result, "sealed-not-polymorphic");
    REQUIRE(plain != nullptr);
    CHECK(plain->span.begin.line == 16);
    CHECK(plain->message == "a @sealed type must also be @polymorphic_base or @interface");
    const auto* lonely = find_diagnostic(result, "sealed-no-derived");
    REQUIRE(lonely != nullptr);
    CHECK(lonely->severity == cppfrontier::diagnostic_severity::warning);
    CHECK(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const cppfrontier::diagnostic& value) { return value.code == "sealed-no-derived"; }) == 3);
    CHECK(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const cppfrontier::diagnostic& value) {
            return value.code == "sealed-no-derived" && value.span.begin.line == 17 &&
                value.message == "nothing derives from the @sealed type 'Lonely' in this "
                                 "translation unit";
        }) == 1);
    const auto* generic = find_diagnostic(result, "sealed-template");
    REQUIRE(generic != nullptr);
    CHECK(generic->span.begin.line == 18);
    CHECK(generic->message == "a @sealed type cannot be a template");
    const auto* odd = find_diagnostic(result, "sealed-derived-scope");
    REQUIRE(odd != nullptr);
    CHECK(odd->span.begin.line == 19);
    CHECK(odd->message ==
          "'Odd' derives from the @sealed type 'Shape' but is not declared in the same scope");
}

TEST_CASE("@sealed rejects a type nested in a type", "[experiments][sealed]") {
    // `friend Circle;` in `Outer::Shape` could not name the later `Outer::Circle`.
    const auto result = translate(
        "Outer: type = {\n"
        "    Shape: @sealed @polymorphic_base type = { area: (virtual this) -> double; }\n"
        "    Circle: type = { this: Shape = (); area: (override this) -> double = 1.0; }\n"
        "}\n");
    const auto* nested = find_diagnostic(result, "sealed-scope");
    REQUIRE(nested != nullptr);
    CHECK(nested->span.begin.line == 2);
    CHECK(nested->message == "a @sealed type must be declared at namespace scope");
}
