#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

#include "test_support.hpp"

namespace {

auto count_occurrences(const std::string& text, std::string_view needle) -> std::size_t {
    auto count = std::size_t{0};
    for (auto position = text.find(needle); position != std::string::npos;
         position = text.find(needle, position + needle.size())) {
        ++count;
    }
    return count;
}

auto compile_check(const std::string& cpp1) -> bool {
#if defined(__unix__) || defined(__APPLE__)
    const auto scratch = test_support::scratch_directory("cppfrontier-value-reflection");
    std::filesystem::create_directories(scratch);
    const auto source_path = scratch / "value-reflection.cpp";
    test_support::write_file(source_path, cpp1);
    const auto result = test_support::run_process(
        {CPPFRONTIER_GXX, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
         source_path.string()}, scratch, std::chrono::seconds{60});
    return !result.timed_out && result.exit_status == 0;
#else
    (void)cpp1;
    return true;
#endif
}

}

TEST_CASE("@value inserts a valid assignment member and symbol", "[reflection]") {
    auto result = cppfrontier::translator{}.translate_source("Point: @value type = { x: int = 0; }\n");
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find("private: int x {0};") != std::string::npos);
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] Point const& that) -> Point&") != std::string::npos);
    CHECK(result.cpp1.find("};") != std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("@value copy assignment copies member values", "[reflection][runtime]") {
    const auto source = std::string{
        "Point: @value type = { x: int = 3; get: (this) -> int = x; }\n"
        "main: () -> int = {\n"
        "    first := Point();\n"
        "    other := Point();\n"
        "    other = first;\n"
        "    if other.get() != 3 { return 3; }\n"
        "    second := Point();\n"
        "    if second.get() != 3 { return 1; }\n"
        "    return second.get() - 3;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto scratch = test_support::scratch_directory("cppfrontier-value-runtime");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "value-runtime.cpp";
    const auto executable_path = scratch / "value-runtime";
    test_support::write_file(cpp1_path, translation.cpp1);
    const auto compiler_result = test_support::run_process({CPPFRONTIER_GXX, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO(compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);
    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO(run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
}

TEST_CASE("nested @value types receive generated assignment", "[reflection][types]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Outer: type = { Inner: @value type = { x: int = 0; } }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] Inner const& that) -> Inner&") != std::string::npos);
}

TEST_CASE("@copy_constructible generates memberwise copy and move constructors",
          "[reflection][copy-constructible]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: @copy_constructible type = {\n"
        "    value: int = 7;\n"
        "    operator=: (out this) = { }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("Widget([[maybe_unused]] Widget const& that) : value{that.value} { }") !=
          std::string::npos);
    CHECK(result.cpp1.find("Widget([[maybe_unused]] Widget&& that) noexcept : value{cpp2::move(that).value} { }") !=
          std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("user-written default construction retains its body",
          "[reflection][copy-constructible]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: @copy_constructible type = {\n"
        "    value: int = 0;\n"
        "    operator=: (out this) = { value = 9; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("explicit Widget()") != std::string::npos);
    CHECK(result.cpp1.find(": value{9}") != std::string::npos);
}

TEST_CASE("@copy_constructible initializes members in declaration order",
          "[reflection][copy-constructible][members]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Pair: @copy_constructible type = {\n"
        "    first: int = 1;\n"
        "    second: int = 2;\n"
        "    operator=: (out this) = { }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find(
        "Pair([[maybe_unused]] Pair const& that) : first{that.first}, second{that.second} { }") !=
        std::string::npos);
    CHECK(result.cpp1.find(
        "Pair([[maybe_unused]] Pair&& that) noexcept : first{cpp2::move(that).first}, "
        "second{cpp2::move(that).second} { }") != std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("@copy_constructible preserves values through copy and move",
          "[reflection][copy-constructible][runtime]") {
    const auto source = std::string{
        "Widget: @copy_constructible type = {\n"
        "    value: int = 7;\n"
        "    operator=: (out this) = { }\n"
        "    get: (this) -> int = value;\n"
        "}\n"
        "main: () -> int = {\n"
        "    first: Widget = ();\n"
        "    second: Widget = first;\n"
        "    third: Widget = (std::move(first));\n"
        "    if second.get() == 7 && third.get() == 7 { return 0; } return 1;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto scratch = test_support::scratch_directory("cppfrontier-copy-constructible");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "copy-constructible.cpp";
    const auto executable_path = scratch / "copy-constructible";
    test_support::write_file(cpp1_path, translation.cpp1);
    const auto compiler_result = test_support::run_process({CPPFRONTIER_GXX, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO(compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);
    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO(run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
}

TEST_CASE("@copy_constructible respects user-written copy construction",
          "[reflection][copy-constructible]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: @copy_constructible type = {\n"
        "    value: int = 7;\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, that) = { value = that.value + 1; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    // Declared once in the type and defined once after it.
    const auto copy_signature = std::string{"\nWidget([[maybe_unused]] Widget const& that)"};
    CHECK(result.cpp1.find("Widget::Widget([[maybe_unused]] Widget const& that)") != std::string::npos);
    const auto first_copy = result.cpp1.find(copy_signature);
    REQUIRE(first_copy != std::string::npos);
    CHECK(result.cpp1.find(copy_signature, first_copy + copy_signature.size()) ==
          std::string::npos);
    CHECK(result.cpp1.find("value{that.value}") == std::string::npos);
    CHECK(result.cpp1.find("Widget([[maybe_unused]] Widget&& that) noexcept") != std::string::npos);
}

TEST_CASE("@copy_constructible diagnoses move-only user construction",
          "[reflection][copy-constructible][diagnostics]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: @copy_constructible type = {\n"
        "    value: int = 7;\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, move that) = { value = that.value; }\n"
        "}\n");
    CHECK(result.has_errors());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "copy-constructible-partial";
        }));
}

TEST_CASE("@copy_constructible requires a type declaration",
          "[reflection][copy-constructible][diagnostics]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "value: @copy_constructible int = 0;\n");
    CHECK(result.has_errors());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "metafunction-requires-type";
        }));
}

TEST_CASE("@singleton generates a private constructor and lazy accessor",
          "[reflection][singleton]") {
    const auto source = std::string{
        "Counter: @singleton type = {\n"
        "    value: int = 0;\n"
        "    increment: (inout this) -> int = { value += 1; return value; }\n"
        "}\n"
        "main: () -> int = {\n"
        "    first := Counter::instance().increment();\n"
        "    second := Counter::instance().increment();\n"
        "    if first == 1 && second == 2 { return 0; } return 1;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    for (const auto& diagnostic : translation.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find("private: explicit Counter();") != std::string::npos);
    CHECK(translation.cpp1.find("public: [[nodiscard]] static auto instance() -> Counter&") !=
          std::string::npos);
    CHECK(translation.cpp1.find("static Counter _instance {};") != std::string::npos);
    CHECK(translation.cpp1.find("Counter::Counter() {}") != std::string::npos);
    CHECK(translation.cpp1.find("Counter(Counter const&) = delete;") != std::string::npos);
    CHECK(translation.cpp1.find(
        "auto operator=(Counter const&) -> void = delete;") != std::string::npos);

    const auto scratch = test_support::scratch_directory("cppfrontier-singleton");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "singleton.cpp";
    const auto executable_path = scratch / "singleton";
    test_support::write_file(cpp1_path, translation.cpp1);
    const auto compiler_result = test_support::run_process({CPPFRONTIER_GXX, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO(compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);
    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO(run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
}

TEST_CASE("@singleton rejects copying", "[reflection][singleton][compile-failure]") {
    const auto source = std::string{
        "Counter: @singleton type = { value: int = 0; }\n"
        "main: () -> int = {\n"
        "    copy := Counter::instance();\n"
        "    return copy.value;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto scratch = test_support::scratch_directory("cppfrontier-singleton-copy");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "singleton-copy.cpp";
    test_support::write_file(cpp1_path, translation.cpp1);
    const auto compiler_result = test_support::run_process({CPPFRONTIER_GXX, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only", cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO(compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    CHECK(compiler_result.exit_status != 0);
}

TEST_CASE("@singleton rejects user constructors and uninitialized data members",
          "[reflection][singleton][diagnostics]") {
    const auto constructor = cppfrontier::translator{}.translate_source(
        "Bad: @singleton type = {\n"
        "    operator=: (out this) = { }\n"
        "}\n");
    CHECK(constructor.has_errors());
    CHECK(std::any_of(constructor.diagnostics.begin(), constructor.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "singleton-constructor"; }));

    const auto member = cppfrontier::translator{}.translate_source(
        "Bad: @singleton type = { value: int; }\n");
    CHECK(member.has_errors());
    CHECK(std::any_of(member.diagnostics.begin(), member.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "singleton-member-initializer";
        }));
}

TEST_CASE("ordinary types receive default construction and deleted copies",
          "[reflection][special-members]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = { value: int = 1; }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("Widget() = default;") != std::string::npos);
    CHECK(result.cpp1.find("Widget(Widget const&) = delete;") != std::string::npos);
    CHECK(result.cpp1.find("auto operator=(Widget const&) -> void = delete;") !=
          std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("value-set constructor and assignment metadata is distinct",
          "[reflection][special-members][ir]") {
    const auto translation = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 1;\n"
        "    operator=: (out this, source: int) = { value = source; }\n"
        "    operator=: (inout this, source: int) = { value = source; }\n"
        "}\n");
    REQUIRE_FALSE(translation.has_errors());
    REQUIRE(translation.program.declarations.size() == 1);
    REQUIRE(translation.program.declarations[0].members.size() == 5);
    const auto& constructor = translation.program.declarations[0].members[1];
    const auto& assignment = translation.program.declarations[0].members[2];
    CHECK(constructor.special_member ==
          cppfrontier::ir_declaration::special_member_kind::converting_constructor);
    CHECK(assignment.special_member ==
          cppfrontier::ir_declaration::special_member_kind::converting_assignment);
}

TEST_CASE("conversion construction suppresses only the default constructor",
          "[reflection][special-members]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 1;\n"
        "    operator=: (out this, source: int) = { value = source; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("Widget() = default;") == std::string::npos);
    CHECK(result.cpp1.find("Widget(cpp2::impl::in<") != std::string::npos);
    CHECK(result.cpp1.find("Widget(Widget const&) = delete;") != std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("a general that constructor permits Cpp1 copy construction",
          "[reflection][special-members]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 1;\n"
        "    operator=: (out this, that) = { value = that.value; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("Widget([[maybe_unused]] Widget const& that)") != std::string::npos);
    CHECK(result.cpp1.find("Widget(Widget const&) = delete;") == std::string::npos);
    CHECK(result.cpp1.find("auto operator=(Widget const&) -> void = delete;") ==
          std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("inout that assignment lowers to copy assignment",
          "[reflection][special-members][assignment]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 1;\n"
        "    operator=: (inout this, that) = { value = that.value; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] Widget const& that) -> Widget&") !=
          std::string::npos);
    CHECK(result.cpp1.find("Widget(Widget const&) = delete;") != std::string::npos);
    CHECK(count_occurrences(result.cpp1, "return *this;") == 2);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("assignment operators return this from every body form",
          "[reflection][special-members][assignment][epilogue]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 1;\n"
        "    operator=: (inout this, that) = { value = that.value; }\n"
        "    operator=: (inout this, move that) = { value = that.value; }\n"
        "    operator=: (inout this, source: int) = value = source;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] Widget const& that) -> Widget&") !=
          std::string::npos);
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] Widget&& that) noexcept -> Widget&") !=
          std::string::npos);
    CHECK(result.cpp1.find("auto operator=(cpp2::impl::in<int> source) -> Widget&") !=
          std::string::npos);
    CHECK(count_occurrences(result.cpp1, "return *this;") == 3);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("a general that constructor generates the complete value set",
          "[reflection][special-members][value-set]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 1;\n"
        "    operator=: (out this, that) = { value = that.value + 1; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("Widget([[maybe_unused]] Widget&& that) noexcept") != std::string::npos);
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] Widget const& that) -> Widget&") !=
          std::string::npos);
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] Widget&& that) noexcept -> Widget&") !=
          std::string::npos);
    CHECK(result.cpp1.find("value = that.value + 1;") != std::string::npos);
    CHECK(result.cpp1.find("value = cpp2::move(that).value + 1;") != std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("a specific value-set member suppresses general generation",
          "[reflection][special-members][value-set]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 1;\n"
        "    operator=: (out this, that) = { value = that.value; }\n"
        "    operator=: (inout this, that) = { value = that.value + 2; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("Widget([[maybe_unused]] Widget&& that) noexcept") != std::string::npos);
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] Widget&& that) noexcept -> Widget&") !=
          std::string::npos);
    CHECK(result.cpp1.find("value = that.value + 2;") != std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("copy assignment generates move assignment without general assignment",
          "[reflection][special-members][value-set]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 1;\n"
        "    operator=: (out this, that) = { value = that.value; }\n"
        "    operator=: (out this, move that) = { value = that.value; }\n"
        "    operator=: (inout this, that) = { value = that.value + 3; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] Widget&& that) noexcept -> Widget&") !=
          std::string::npos);
    CHECK(result.cpp1.find("value = cpp2::move(that).value + 3;") != std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("explicit move operations move-render their bodies",
          "[reflection][special-members][move-that]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 1;\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, move that) = { value = that.value; }\n"
        "    operator=: (inout this, move that) = { value = that.value + 1; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find(": value{cpp2::move(that).value}") != std::string::npos);
    CHECK(result.cpp1.find("value = cpp2::move(that).value + 1;") != std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("a move function moves only at the definite last use of that",
          "[reflection][special-members][move-that]") {
    const auto translate = [](std::string_view body) {
        return cppfrontier::translator{}.translate_source(
            "Widget: type = {\n"
            "    a: std::string = ();\n"
            "    b: std::string = ();\n"
            "    id: int = 0;\n"
            "    operator=: (out this) = { }\n"
            "    operator=: (out this, move that) = " + std::string{body} + "\n"
            "    f: (s: std::string) -> std::string = s;\n"
            "    get: (move this) -> std::string = a;\n"
            "}\n");
    };
    SECTION("members read once") {
        const auto result = translate("{ a = that.a; b = that.b; }");
        INFO(result.cpp1);
        REQUIRE_FALSE(result.has_errors());
        CHECK(result.cpp1.find(": a{that.a}, b{cpp2::move(that).b}, id{cpp2::move(that).id}") !=
              std::string::npos);
        CHECK(compile_check(result.cpp1));
    }
    SECTION("a member read twice") {
        const auto result = translate("{ a = that.a; b = f(that.a); }");
        INFO(result.cpp1);
        REQUIRE_FALSE(result.has_errors());
        CHECK(result.cpp1.find(": a{that.a}, b{f(cpp2::move(that).a)}") != std::string::npos);
        CHECK(compile_check(result.cpp1));
    }
    SECTION("an assignment to that is the last use and is not moved") {
        const auto result = translate("{ a = that.a; id = that.id; that.id = 0; }");
        INFO(result.cpp1);
        REQUIRE_FALSE(result.has_errors());
        CHECK(result.cpp1.find(": a{that.a}, b{cpp2::move(that).b}, id{that.id}") !=
              std::string::npos);
        CHECK(result.cpp1.find("    that.id = 0;") != std::string::npos);
        CHECK(result.cpp1.find("cpp2::move(that).id = 0") == std::string::npos);
        CHECK(compile_check(result.cpp1));
    }
    SECTION("an explicit move") {
        const auto result = translate("{ a = (move that.a); id = that.id; that.id = 0; }");
        INFO(result.cpp1);
        REQUIRE_FALSE(result.has_errors());
        CHECK(result.cpp1.find(": a{std::move(that.a)}, b{cpp2::move(that).b}, id{that.id}") !=
              std::string::npos);
        CHECK(compile_check(result.cpp1));
    }
    SECTION("a UFCS call as the last use") {
        const auto result = translate("{ a = that.a; x := that.get(); }");
        INFO(result.cpp1);
        REQUIRE_FALSE(result.has_errors());
        // A use of `that` as a whole keeps the generated defaults copies.
        CHECK(result.cpp1.find(": a{that.a}, b{that.b}, id{that.id}") != std::string::npos);
        CHECK(result.cpp1.find("auto x {CPP2_UFCS(get)(cpp2::move(that))};") !=
              std::string::npos);
        CHECK(compile_check(result.cpp1));
    }
    SECTION("a body read of a defaulted member keeps its default a copy") {
        const auto result = translate("{ x := f(that.b); }");
        INFO(result.cpp1);
        REQUIRE_FALSE(result.has_errors());
        CHECK(result.cpp1.find(": a{cpp2::move(that).a}, b{that.b}, id{cpp2::move(that).id}") !=
              std::string::npos);
        CHECK(result.cpp1.find("auto x {f(cpp2::move(that).b)};") != std::string::npos);
    }
    SECTION("a last use followed by a generated copy of the same member") {
        const auto result = translate("{ a = f(that.b); }");
        INFO(result.cpp1);
        REQUIRE_FALSE(result.has_errors());
        CHECK(result.cpp1.find(": a{f(that.b)}, b{that.b}, id{cpp2::move(that).id}") !=
              std::string::npos);
    }
}

TEST_CASE("generated memberwise defaults of move functions move",
          "[reflection][special-members][move-that]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Base: type = {\n"
        "    public s: std::string = ();\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, that) = { }\n"
        "}\n"
        "Widget: type = {\n"
        "    this: Base = ();\n"
        "    a: std::string = ();\n"
        "    b: std::string = ();\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, that) = { }\n"
        "    operator=: (out this, move that) = { }\n"
        "    operator=: (inout this, move that) = { a = that.a; f(that.s); }\n"
        "    f: (s: std::string) = { }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("Widget::Widget([[maybe_unused]] Widget&& that) noexcept : Base{static_cast<Base&&>(that)}, "
                           "a{cpp2::move(that).a}, b{cpp2::move(that).b}") != std::string::npos);
    CHECK(result.cpp1.find("Widget::Widget([[maybe_unused]] Widget const& that) : Base{static_cast<Base const&>(that)}, "
                           "a{that.a}, b{that.b}") != std::string::npos);
    // A read of a base member keeps the base a copy.
    CHECK(result.cpp1.find("Base::operator=(static_cast<Base const&>(that)); a = that.a; "
                           "b = cpp2::move(that).b;") != std::string::npos);
    CHECK(result.cpp1.find("f(cpp2::move(that).s);") != std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("a move generated from an in that function moves at the last use",
          "[reflection][special-members][move-that]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    a: std::string = ();\n"
        "    b: std::string = ();\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, that) = { a = that.a; b = that.b; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("Widget::Widget([[maybe_unused]] Widget const& that) : a{that.a}, b{that.b}") !=
          std::string::npos);
    CHECK(result.cpp1.find("Widget::Widget([[maybe_unused]] Widget&& that) noexcept : a{that.a}, "
                           "b{cpp2::move(that).b}") != std::string::npos);
    CHECK(result.cpp1.find("    a = that.a; b = that.b;") != std::string::npos);
    CHECK(result.cpp1.find("    a = that.a; b = cpp2::move(that).b;") != std::string::npos);
    // The generated move is not the user's code, so it gets no hint.
    CHECK(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "member-copied-not-moved"; }));
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("a last use of that moves a reference member through that",
          "[reflection][special-members][move-that]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    k: std::string;\n"
        "    r: cpp1_ref<std::string>;\n"
        "    operator=: (out this, inout s: std::string) = { k = s; r = s; }\n"
        "    operator=: (out this, move that) = { k = that.k; r = that.r; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find(": k{that.k}, r{cpp2::move(that).r}") != std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("a converting constructor generates converting assignment",
          "[reflection][special-members][value-set]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 1;\n"
        "    operator=: (out this, source: int) = { value = source + 4; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto operator=(cpp2::impl::in<int> source) -> Widget&") !=
          std::string::npos);
    CHECK(result.cpp1.find("value = source + 4;") != std::string::npos);
    CHECK(result.cpp1.find("Widget([[maybe_unused]] Widget&& that) noexcept") == std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("generated value-set members are marked and not regenerated",
          "[reflection][special-members][value-set][ir]") {
    const auto translation = cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    value: int = 1;\n"
        "    operator=: (out this, that) = { value = that.value; }\n"
        "}\n");
    REQUIRE_FALSE(translation.has_errors());
    const auto& members = translation.program.declarations.front().members;
    const auto count = [&](auto kind) {
        return std::count_if(members.begin(), members.end(), [&](const auto& member) {
            return member.special_member == kind;
        });
    };
    CHECK(count(cppfrontier::ir_declaration::special_member_kind::copy_constructor) == 1);
    CHECK(count(cppfrontier::ir_declaration::special_member_kind::move_constructor) == 1);
    CHECK(count(cppfrontier::ir_declaration::special_member_kind::copy_assignment) == 1);
    CHECK(count(cppfrontier::ir_declaration::special_member_kind::move_assignment) == 1);
    CHECK(std::count_if(members.begin(), members.end(), [](const auto& member) {
              return member.is_generated;
          }) == 3);
}

TEST_CASE("unsupported metaclasses are diagnosed", "[reflection][metaclass][diagnostics]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: @frobnicate type = { a: int; b: float; }\n");
    INFO(result.cpp1);
    CHECK(result.has_errors());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "unsupported-metaclass"; }));
}

TEST_CASE("that members initialize unassigned members from that", "[reflection][that]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "V: type = {\n"
        "    public x: int = 0;\n"
        "    public y: int = 0;\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, that) = { y = 7; }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("V([[maybe_unused]] V const& that) : x{that.x}, y{7} {") != std::string::npos);
    CHECK(result.cpp1.find("V([[maybe_unused]] V&& that) noexcept : x{cpp2::move(that).x}, y{7} {") !=
          std::string::npos);
    CHECK(result.cpp1.find("x = that.x; y = 7; ") != std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("@basic_value types are copyable and default constructible", "[reflection][basic-value]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "V: @basic_value type = {\n"
        "    public x: int = 0;\n"
        "    operator=: (out this, v: int) = { x = v; }\n"
        "}\n"
        "main: () -> int = { a: V = (); b: V = (3); c := b; a = c; return a.x - 3; }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("V([[maybe_unused]] V const& that) : x{that.x} { }") != std::string::npos);
    CHECK(result.cpp1.find("V() = default;") != std::string::npos);
    CHECK(compile_check(result.cpp1));

    const auto partial = cppfrontier::translator{}.translate_source(
        "V: @value type = { x: int = 0; operator=: (out this, move that) = { } }\n");
    CHECK(std::any_of(partial.diagnostics.begin(), partial.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "value-partially-copyable"; }));
}

TEST_CASE("@enum types lower to classes with constexpr enumerators",
          "[reflection][enum][upstream-1316][upstream-1435][upstream-816]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "E: @enum<u8> type = { A; B := 5; C: = 7; D; }\n"
        "Skat: @enum type = { diamonds := 9; hearts; name: (this) -> std::string = to_string(); }\n"
        "main: () -> int = {\n"
        "    colors: std::array<E, 3> = (E::B);\n"
        "    static_assert(std::is_trivial_v<E>);\n"
        "    v: u8 = E::C as u8;\n"
        "    s: Skat = ();\n"
        "    if s.name() != \"diamonds\" || E::from_code(\"E::D\").to_string() != \"D\" { return 1; }\n"
        "    if v == 7 && E::A < E::B && colors[1] == E::A { return 0; } return 1;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("private: unsigned char _value;") != std::string::npos);
    CHECK(result.cpp1.find("E() = default;") != std::string::npos);
    CHECK(result.cpp1.find("inline constexpr E E::D {8};") != std::string::npos);
    CHECK(result.cpp1.find(
              "private: signed char _value {cpp2::unchecked_narrow<cpp2::i8>(9)};") !=
          std::string::npos);
    CHECK(result.cpp1.find("Skat() = default;") != std::string::npos);
    CHECK(compile_check(result.cpp1));

    const auto has = [](const std::string& source, std::string_view code) {
        const auto rejected = cppfrontier::translator{}.translate_source(source);
        return std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    CHECK(has("E: @enum type = { }\n", "enum-empty"));
    CHECK(has("E: @enum type = { A: int = 1; }\n", "enum-enumerator-type"));
    CHECK(has("E: @enum type = { A := 1 + 1; B; }\n", "enum-requires-underlying-type"));
    CHECK(has("E: @enum<i32> type = { A := 1 + 1; B; }\n", "enum-enumerator-order"));
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

TEST_CASE("@flag_enum types have power-of-two flags and bitwise operations",
          "[reflection][flag-enum]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "file_attributes: @flag_enum<u8> type = {\n"
        "    cached;\n"
        "    current;\n"
        "    obsolete;\n"
        "    cached_and_current := cached | current;\n"
        "}\n"
        "big: @flag_enum type = { a; b := 256; }\n"
        "janus: @enum type = {\n"
        "    past;\n"
        "    future;\n"
        "    flip: (inout this) == { if this == past { this = future; } else { this = past; } }\n"
        "}\n"
        "main: () = {\n"
        "    j := janus::past;\n"
        "    j.flip();\n"
        "    std::cout << j.to_string();\n"
        "    f: file_attributes = file_attributes::cached_and_current;\n"
        "    f &= file_attributes::cached | file_attributes::obsolete;\n"
        "    std::cout << f.to_string() << (f.get_raw_value() as int) << \"\\n\";\n"
        "    f.clear(f);\n"
        "    std::cout << f.to_string() << f.to_code() << \"\\n\";\n"
        "    f.set(file_attributes::current | file_attributes::obsolete);\n"
        "    std::cout << f.to_code() << f.has(file_attributes::current)"
        " << (f ^ file_attributes::obsolete).to_string() << \"\\n\";\n"
        "    g := file_attributes::from_code(\"(file_attributes::cached | file_attributes::obsolete)\");\n"
        "    std::cout << file_attributes::from_string(\"cached_and_current\").to_string()"
        " << g.to_string() << sizeof(big) << \"\\n\";\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("inline constexpr file_attributes file_attributes::obsolete {4};") !=
          std::string::npos);
    CHECK(result.cpp1.find("inline constexpr file_attributes file_attributes::none {0};") !=
          std::string::npos);
    CHECK(result.cpp1.find("private: unsigned short _value;") != std::string::npos);
    CHECK(result.cpp1.find("constexpr auto flip()& -> void") != std::string::npos);
    CHECK(compile_and_run(result.cpp1, "flag-enum") ==
          "future(cached)1\n(none)(none)\n(file_attributes::current | file_attributes::obsolete)1"
          "(current)\n(cached, current, cached_and_current)(cached, obsolete)2\n");

    const auto has = [](const std::string& source, std::string_view code) {
        const auto rejected = cppfrontier::translator{}.translate_source(source);
        return std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    CHECK(has("E: @flag_enum type = { a; none; }\n", "metafunction-reserved-name"));
    CHECK(has("E: @flag_enum type = { a; has: (this) = { } }\n", "metafunction-reserved-name"));
    CHECK(has("E: @enum type = { a; operator=: (out this) = { } }\n", "enum-reserved-name"));
    CHECK(has("E: @enum type = { a; operator=: (out this, i: int) = { } }\n",
              "enum-reserved-name"));
    CHECK(has("E: @flag_enum type = { }\n", "enum-empty"));
}

TEST_CASE("@union types hold one alternative at a time", "[reflection][union]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "name_or_number: @union type = {\n"
        "    name: std::string;\n"
        "    num : i32;\n"
        "}\n"
        "name_or_other: @union <T:type> type = {\n"
        "    name  : std::string;\n"
        "    other : T;\n"
        "    to_string: (this) -> std::string = {\n"
        "        if is_name() { return name(); }\n"
        "        else if is_other() { return other() as std::string; }\n"
        "        else { return \"invalid value\"; }\n"
        "    }\n"
        "}\n"
        "main: () = {\n"
        "    x: name_or_number = ();\n"
        "    std::cout << x.is_name() << x.is_num();\n"
        "    x.set_name(\"xyzzy\", 3u);\n"
        "    y := x;\n"
        "    x.set_num(5);\n"
        "    z: name_or_number = (std::move(y));\n"
        "    y = x;\n"
        "    std::cout << z.name() << x.num() << y.num();\n"
        "    val: name_or_other<int> = ();\n"
        "    val.set_other(42);\n"
        "    std::cout << val.to_string() << \"\\n\";\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("_discriminator {-1};") != std::string::npos);
    CHECK(compile_and_run(result.cpp1, "union") == "00xyz5542\n");

    const auto has = [](const std::string& source, std::string_view code) {
        const auto rejected = cppfrontier::translator{}.translate_source(source);
        return std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    CHECK(has("U: @union type = { private a: int; }\n", "union-alternative-access"));
    CHECK(has("U: @union type = { is_a: int; }\n", "union-alternative-name"));
    CHECK(has("U: @union type = { a: int; set_a: (inout this) = { } }\n",
              "union-alternative-name"));
    CHECK(has("U: @union type = { a: int = 1; }\n", "union-alternative-initializer"));
    CHECK(has("U: @union type = { }\n", "union-empty"));
    CHECK(has("U: @union type = { _: int; b: double; }\n", "union-anonymous-alternative"));

    // A repeated alternative is reported at its second declaration.
    const auto duplicate = cppfrontier::translator{}.translate_source(
        "U: @union type = { a: int; a: double; }\nmain: () = { }\n");
    REQUIRE(duplicate.diagnostics.size() == 1);
    CHECK(duplicate.diagnostics.front().code == "union-duplicate-alternative");
    CHECK(duplicate.diagnostics.front().message == "'a' is already an alternative of U");
    CHECK(duplicate.diagnostics.front().span.begin.column == 28);
}

TEST_CASE("@union moves move the alternative", "[reflection][union]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "U: @union type = { name: std::string; num: i32; }\n"
        "main: () = {\n"
        "    a: U = ();\n"
        "    a.set_name(\"a string longer than the small buffer\");\n"
        "    b: U = (std::move(a));\n"
        "    c: U = ();\n"
        "    c = std::move(b);\n"
        "    arr: std::array<U, 2> = ();\n"
        "    std::cout << a.name().size() << b.name().size() << c.name().size()"
        " << arr[0].is_name() << std::is_trivially_copyable_v<U>;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(compile_and_run(result.cpp1, "union-move") == "003700");
}

TEST_CASE("base types are initialized, copied, and assigned", "[reflection][bases][upstream-422]") {
    const auto source = std::string{
        "base: type = {\n"
        "    public x: i32 = -1;\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, y: i32) = x = y;\n"
        "    operator=: (out this, that) = { }\n"
        "}\n"
        "derived: type = { this: base = 42; }\n"
        "named: type = {\n"
        "    this: base = 1;\n"
        "    public tag: i32 = 5;\n"
        "    operator=: (out this, v: i32) = { base = v * 2; tag = v; }\n"
        "    operator=: (out this, that) = { }\n"
        "}\n"
        "main: () -> int = {\n"
        "    d: derived = ();\n"
        "    n: named = (3);\n"
        "    c := n;\n"
        "    c = 4;\n"
        "    if d.x == 42 && c.x == 8 && c.tag == 4 { return 0; } return 1;\n"
        "}\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("struct derived : public base {") != std::string::npos);
    CHECK(result.cpp1.find("derived() : base{42} { }") != std::string::npos);
    CHECK(result.cpp1.find("named(cpp2::impl::in<int> v) : base{v * 2}, tag{v} {") !=
          std::string::npos);
    CHECK(result.cpp1.find("named([[maybe_unused]] named const& that) : base{static_cast<base const&>(that)}, tag{that.tag} {") !=
          std::string::npos);
    CHECK(result.cpp1.find("operator=(cpp2::impl::in<int> v) -> named&") == std::string::npos);
    CHECK(result.cpp1.find("base::operator=(static_cast<base&&>(that)); ") != std::string::npos);
    CHECK(compile_check(result.cpp1));
}

TEST_CASE("data members before a base live in helper bases",
          "[reflection][bases][upstream-372][upstream-373]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "x: type = {\n"
        "    public a: int = 1;\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, that) = { }\n"
        "}\n"
        "y: type = {\n"
        "    public a: int = 0;\n"
        "    this: x = ();\n"
        "    operator=: (out this, v: int) = { a = v; x = (); }\n"
        "}\n"
        "main: () -> int = { return y(2).a - 2; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("struct y_a_as_base { int a {0}; auto operator<=>(y_a_as_base "
                           "const&) const = default; };\n"
                           "struct y : public y_a_as_base, public x {") != std::string::npos);
    CHECK(result.cpp1.find("{ using y_a_as_base::a;") != std::string::npos);
    CHECK(result.cpp1.find("y(cpp2::impl::in<int> v) : y_a_as_base{v}, x{} {") !=
          std::string::npos);
    CHECK(result.cpp1.find("operator=(cpp2::impl::in<int> v)") == std::string::npos);
    CHECK(compile_check(result.cpp1));

    const auto has = [](const std::string& source, std::string_view code) {
        const auto rejected = cppfrontier::translator{}.translate_source(source);
        return std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    CHECK(has("b: type = { }\nd: type = { private this: b; }\n", "non-public-base"));
    CHECK(has("d: type = { this: _ = 1; }\n", "deduced-base"));
    CHECK(has("d: type = { this: dur; dur: type == int; }\n", "base-names-member-type"));
    CHECK(has("d: type = { this: std::vector<dur>; dur: type == int; }\n",
              "base-names-member-type"));
    // A qualified name does not name the member type.
    CHECK_FALSE(has("n: namespace = { dur: type = { } }\n"
                    "d: type = { this: n::dur; dur: type == int; }\n",
                    "base-names-member-type"));
}

TEST_CASE("virtual functions, interfaces, and polymorphic bases",
          "[reflection][virtual][interface]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Human: @interface type = { speak: (this) -> std::string; }\n"
        "N: namespace = {\n"
        "    Machine: @polymorphic_base <I: int> type = {\n"
        "        operator=: (out this, _: std::string) = { }\n"
        "        work: (virtual this) -> int;\n"
        "        rank: (virtual this) -> int = I;\n"
        "    }\n"
        "}\n"
        "Cyborg: type = {\n"
        "    this: Human = ();\n"
        "    this: N::Machine<99>;\n"
        "    operator=: (out this, n: std::string) = { N::Machine<99> = n; }\n"
        "    speak: (override this) -> std::string = \"hi\";\n"
        "    work: (final this) -> int = 7;\n"
        "    operator=: (move this) = std::cout << \"bye\";\n"
        "}\n"
        "check: (h: Human, m: N::Machine<99>) -> bool = h.speak() == \"hi\" && m.work() == 7;\n"
        "main: () -> int = { c: Cyborg = (\"x\"); if check(c, c) { return 0; } return 1; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("virtual auto speak() const -> std::string = 0;") != std::string::npos);
    CHECK(result.cpp1.find("protected: Human([[maybe_unused]] Human const& that)") != std::string::npos);
    CHECK(result.cpp1.find("virtual ~Human() noexcept { }") != std::string::npos);
    CHECK(result.cpp1.find("virtual auto work() const -> int = 0;") != std::string::npos);
    CHECK(result.cpp1.find("virtual auto rank() const -> int;") != std::string::npos);
    CHECK(result.cpp1.find("template<int I> [[nodiscard]] auto Machine<I>::rank() const -> int { return I; }") !=
          std::string::npos);
    CHECK(result.cpp1.find("virtual ~Machine() noexcept { }") != std::string::npos);
    CHECK(result.cpp1.find("auto speak() const -> std::string override") != std::string::npos);
    CHECK(result.cpp1.find("auto work() const -> int final") != std::string::npos);
    CHECK(result.cpp1.find("~Cyborg() noexcept { std::cout << \"bye\"; }") != std::string::npos);
    CHECK(compile_check(result.cpp1));

    const auto has = [](const std::string& source, std::string_view code) {
        const auto rejected = cppfrontier::translator{}.translate_source(source);
        return std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    CHECK(has("I: @interface type = { x: int = 0; }\n", "interface-data-member"));
    CHECK(has("I: @interface type = { f: (this) = { } }\n", "interface-function-body"));
    CHECK(has("B: @polymorphic_base type = { operator=: (move this) = { } }\n",
              "polymorphic-base-destructor"));
    CHECK(has("B: @polymorphic_base type = { operator=: (out this, that) = { } }\n",
              "polymorphic-base-copy-or-move"));
    CHECK(has("B: @polymorphic_base type = { operator=: (inout this, move that) = { } }\n",
              "polymorphic-base-copy-or-move"));
    CHECK(has("I: @interface type = { operator=: (out this, that); }\n",
              "interface-copy-or-move"));
    CHECK(has("f: (virtual x: int) = { }\n", "invalid-this-specifier"));
    for (const auto* source : {
             "B: type = { f: (virtual this) -> (a: int, b: int) = { a = 1; b = 2; } }\n",
             "B: type = { f: (override this) -> (a: int) = { a = 1; } }\n",
             "B: type = { f: (final this) -> (a: int) = { a = 1; } }\n",
             "I: @interface type = { f: (this) -> (a: int, b: int); }\n"}) {
        INFO(source);
        CHECK(has(source, "virtual-named-returns"));
    }
    CHECK_FALSE(has("B: @polymorphic_base type = { f: (this) -> (a: int) = { a = 1; } }\n",
                    "virtual-named-returns"));
    for (const auto* source : {
             "B: type = { f: (virtual this, x: int) pre(x > 0); }\n",
             "I: @interface type = { f: (this, x: int) post(x > 0); }\n",
             "E: type = { operator==: (this, that) -> bool pre(true); }\n"}) {
        INFO(source);
        CHECK(has(source, "contract-without-body"));
    }
    CHECK_FALSE(has("B: type = { f: (virtual this, x: int) pre(x > 0) = { } }\n",
                    "contract-without-body"));
}

TEST_CASE("ordering metafunctions add a defaulted operator<=>", "[reflection][ordering]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "a: @ordered type = { v: int = 0; }\n"
        "b: @weakly_ordered type = { v: int = 0; }\n"
        "c: @partially_ordered type = { v: double = 0; }\n"
        "d: @weakly_ordered_value type = { v: int = 0; operator=: (out this, i: int) = { v = i; } }\n"
        "e: @partially_ordered_value type = { v: int = 0; }\n"
        "f: @value type = { v: int = 0; }\n"
        "g: @ordered type = { v: int = 0; operator<=>: (this, that) -> std::strong_ordering; }\n"
        "main: () -> int = { x: d = 1; y: d = 2; return (x < y && e() == e() && f() <= f()) as int; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto operator<=>([[maybe_unused]] a const& that) const& -> std::strong_ordering = default;") !=
          std::string::npos);
    CHECK(result.cpp1.find("-> std::weak_ordering = default;") != std::string::npos);
    CHECK(result.cpp1.find("auto operator<=>([[maybe_unused]] e const& that) const& -> std::partial_ordering = default;") !=
          std::string::npos);
    CHECK(result.cpp1.find("auto operator<=>([[maybe_unused]] f const& that) const& -> std::strong_ordering = default;") !=
          std::string::npos);
    CHECK(count_occurrences(result.cpp1, "([[maybe_unused]] g const& that) const& -> std::strong_ordering = default;") == 1);
    CHECK(result.cpp1.find("d([[maybe_unused]] d const& that)") != std::string::npos);
    CHECK(compile_check(result.cpp1));

    const auto rejected = cppfrontier::translator{}.translate_source(
        "a: @weakly_ordered type = { operator<=>: (this, that) -> std::strong_ordering; }\n");
    REQUIRE(rejected.has_errors());
    CHECK(rejected.diagnostics.front().message == "operator<=> must return std::weak_ordering");
}

TEST_CASE("@hashable combines member hashes", "[reflection][hashable]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "base: @struct @hashable type = { h: i32; }\n"
        "mystruct: @struct @hashable type = {\n"
        "    this: base = (1);\n"
        "    i: i32;\n"
        "    j: std::string;\n"
        "}\n"
        "main: () = { x: mystruct = (2, \"three\"); std::cout << x.hash() << x.h; }\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto hash() const& -> size_t") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::hash_combine(ret, base::hash());\n"
                           "    cpp2::hash_combine(ret, std::hash<cpp2::i32>()(i));\n"
                           "    cpp2::hash_combine(ret, std::hash<std::string>()(j));\n"
                           "    return ret;") != std::string::npos);
    CHECK(result.cpp1.find(": base{1}") != std::string::npos);
    CHECK(compile_check(result.cpp1));

    const auto rejected = cppfrontier::translator{}.translate_source(
        "a: @hashable type = { f: (this) = { } }\n");
    REQUIRE(rejected.has_errors());
    CHECK(rejected.diagnostics.front().message ==
          "a hashable type must have at least one data member");
}

TEST_CASE("@print pretty-prints the type", "[reflection][print]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "t: @print type = {\n    x: int = 1;\n}\nu: type = { }\n");
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.printed == "\n\nt:/* @print */ type = \n{\n    x: int = 1;\n}\n");
    CHECK(result.cpp1.find("@print") == std::string::npos);

    const auto rejected = cppfrontier::translator{}.translate_source(
        "t: @print type = { n: namespace = { } }\n");
    CHECK(rejected.printed.empty());
}

TEST_CASE("@print shows metafunction arguments apart from template parameters",
          "[reflection][print][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "s: @struct<noforward> @print type = { x: int = 0; }\n"
        "b: @print @value <T: type> type = { x: T = (); }\n");
    REQUIRE_FALSE(result.has_errors());
    INFO(result.printed);
    CHECK(result.printed.find("s:/* @struct<noforward> @print */ type = \n") != std::string::npos);
    CHECK(result.printed.find("b:/* @print @value */ <T: type, > type = \n") != std::string::npos);
}

TEST_CASE("@copyable generates the copy and move functions", "[reflection][copyable]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "C: @copyable type = {\n"
        "    public n: int = 1;\n"
        "    operator=: (out this, v: int) = { n = v; }\n"
        "}\n"
        "main: () = { a: C = 5; b := a; c: C = (move b); d: C = 0; d = c; }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("C([[maybe_unused]] C const& that);") != std::string::npos);
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] C&& that) noexcept -> C&;") != std::string::npos);
    CHECK(compile_check(result.cpp1));

    const auto partial = cppfrontier::translator{}.translate_source(
        "P: @copyable type = { n: int = 1; operator=: (out this, move that) = { } }\n");
    CHECK(partial.has_errors());
}

TEST_CASE("A metafunction reserves the names it generates",
          "[reflection][review]") {
    const auto diagnosed = [](const std::string& source) {
        auto options = cppfrontier::translation_options{};
        REQUIRE(options.experiments.enable("adt"));
        const auto result = cppfrontier::translator{}.translate_source(source, options);
        auto messages = std::vector<std::string>{};
        for (const auto& diagnostic : result.diagnostics) {
            if (diagnostic.code == "metafunction-reserved-name") {
                messages.push_back(diagnostic.message);
            }
        }
        return messages;
    };
    for (const auto& [source, reserved] : std::to_array<std::pair<const char*, const char*>>({
             {"E: @enum type = { a; to_string: (this) -> std::string = \"x\"; }\n",
              "in a '@enum' type, the name 'to_string'"},
             {"E: @enum type = { a; to_string; }\n", "in a '@enum' type, the name 'to_string'"},
             {"H: @hashable type = { x: int = 1; hash: (this, k: int) -> int = k; }\n",
              "in a '@hashable' type, the name 'hash'"},
             {"V: @adt type = { A: (x: int); is_A: (this) -> bool = true; }\n",
              "in a '@adt' type, the name 'is_A'"},
             {"V: @adt type = { A: (x: int); as_A: (y: int); }\n",
              "in a '@adt' type, the name 'as_A'"},
             {"V: @adt type = { A: (x: int); cpp2_adt_value: (y: int); }\n",
              "in a '@adt' type, the name 'cpp2_adt_value'"},
             {"X: @adt type = { X: (x: int); }\n", "in a '@adt' type, the name 'X'"},
             {"E: @enum type = { a; _value; }\n", "in a '@enum' type, the name '_value'"},
             {"U: @union type = { i: int; _storage: int == 1; }\n",
              "in a '@union' type, the name '_storage'"},
             {"U: @union type = { i: int; _discriminator: (this) = { } }\n",
              "in a '@union' type, the name '_discriminator'"}})) {
        INFO(source);
        const auto messages = diagnosed(source);
        REQUIRE(messages.size() == 1);
        CHECK(messages.front().starts_with(reserved));
    }
    CHECK(diagnosed("H: @hashable type = { x: int = 1; _: int = 2; }\n").empty());
    CHECK(diagnosed("H: @hashable type = { x: int = 1; _y: int = 2; }\n").empty());
    CHECK(diagnosed("S: @struct type = { _x: int; }\n").empty());
    CHECK(diagnosed("U: @union type = { i: int; s: std::string; }\n").empty());
    CHECK(diagnosed("V: @value type = { x: int = 0; operator=: (out this, y: int) = { x = y; } }\n")
              .empty());
}

TEST_CASE("A generated name is reserved at the member that reuses it", "[reflection][hunt]") {
    const auto reserved_at = [](const std::string& source) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& issue) { return issue.code == "metafunction-reserved-name"; });
        return found == result.diagnostics.end() ? std::optional<std::uint32_t>{}
                                                 : std::optional{found->span.begin.column};
    };
    // @singleton always generates instance().
    CHECK(reserved_at("S: @singleton type = { instance: int = 0; }\n") == 24u);
    CHECK(reserved_at("S: @singleton type = { instance: (x: int) -> int = x; }\n") == 24u);
    // A function named after its type clashes with the generated constructors.
    CHECK(reserved_at("V: @value type = { x: int = 0; V: (this) = { } }\n") == 32u);
    // Two written enumerators are a redeclaration, which Cpp1 reports.
    CHECK_FALSE(reserved_at("E: @enum type = { a; b := 1; a := 2; }\n"));
    CHECK_FALSE(reserved_at("E: @flag_enum type = { a; a; }\n"));
}

TEST_CASE("A misplaced this-specifier is reported once at the specifier", "[reflection][hunt]") {
    const auto only = [](const std::string& source) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        REQUIRE(result.diagnostics.size() == 1);
        return result.diagnostics.front();
    };
    const auto twice = only("B: @polymorphic_base type = { f: (virtual this) = { } }\n"
                            "D: type = { this: B = (); f: (final override this) = { } }\n");
    CHECK(twice.code == "invalid-this-specifier");
    CHECK(twice.span.begin.line == 2);
    CHECK(twice.span.begin.column == 37);
    CHECK(twice.message.find("one of virtual, override and final") != std::string::npos);
    const auto named = only("T: type = { f: (virtual x: int) = { } }\n");
    CHECK(named.code == "invalid-this-specifier");
    CHECK(named.span.begin.column == 17);
}

TEST_CASE("a repeated enumerator is reported at its second declaration", "[reflection][enum]") {
    for (const auto* metaclass : {"@enum", "@flag_enum"}) {
        INFO(metaclass);
        const auto result = cppfrontier::translator{}.translate_source(
            std::string{"E: "} + metaclass + " type = { a; b; a; }\nmain: () = { }\n");
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().code == "enum-duplicate-enumerator");
        CHECK(result.diagnostics.front().message == "'a' is already an enumerator of E");
        CHECK(result.diagnostics.front().span.begin.column ==
              static_cast<std::uint32_t>(std::string_view{metaclass}.size() + 20));
    }
}

TEST_CASE("@flag_enum reports a reserved name at the enumerator", "[reflection][review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "F: @flag_enum type = {\n    a;\n    b;\n    none;\n}\n");
    REQUIRE(result.has_errors());
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) { return issue.message.find("'none' is reserved") != std::string::npos; });
    REQUIRE(found != result.diagnostics.end());
    CHECK(found->span.begin.line == 4);
    CHECK(found->span.begin.column == 5);
}

TEST_CASE("@print shows the access and virtual that metafunctions give members",
          "[reflection][review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "shape: @interface @print type = {\n"
        "    draw: (this);\n"
        "    move: (inout this, dx: int);\n"
        "    public size: (virtual this) -> int;\n"
        "}\n");
    INFO(result.printed);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.printed.find("    public draw:(virtual in this, );\n") != std::string::npos);
    CHECK(result.printed.find("        virtual inout this, \n") != std::string::npos);
    CHECK(result.printed.find("    public size:(virtual in this, )") != std::string::npos);
}

TEST_CASE("@print shows the public access @polymorphic_base gives functions",
          "[reflection][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "base: @polymorphic_base @print type = {\n"
        "    g: (virtual this) -> int = 1;\n"
        "    private h: (this) = { }\n"
        "    x: int = 0;\n"
        "}\n");
    INFO(result.printed);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.printed.find("    public g:(virtual in this, ) -> move int = 1;\n") !=
          std::string::npos);
    CHECK(result.printed.find("    private h:(in this, )") != std::string::npos);
    CHECK(result.printed.find("    x: int = 0;\n") != std::string::npos);
}

TEST_CASE("a constructor checks its preconditions before its member initializers",
          "[reflection][contracts]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "X: type = {\n"
        "    name: std::string = ();\n"
        "    v: std::vector<int> = ();\n"
        "    operator=: (out this, move n: std::string) pre(!n.empty()) = { name = n; v = (1, 2); }\n"
        "    operator=: (out this, a: int, out r: int) pre(a > 0) = { name = (); r = a; v = (a, 2); }\n"
        "    operator=: (out this, a: int) pre(a > 1) = { }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("name{([&]{ if (cpp2::cpp2_default.is_active() && "
                           "!(!CPP2_UFCS(empty)(n)) ) { cpp2::cpp2_default.report_violation(\"\"); "
                           "} }(), CPP2_FORWARD(n))}, v{1, 2}") != std::string::npos);
    CHECK(result.cpp1.find("name{}, v{([&]{ if (cpp2::cpp2_default.is_active() && "
                           "!(cpp2::impl::cmp_greater(a,0)) ) { cpp2::cpp2_default."
                           "report_violation(\"\"); } }(), r.construct(a), a), 2}") !=
          std::string::npos);
    // With no member initializer, the body checks them first.
    const auto bare = result.cpp1.find("X::X(cpp2::impl::in<int> a)  {\n");
    REQUIRE(bare != std::string::npos);
    CHECK(result.cpp1.find("cmp_greater(a,1)", bare) < result.cpp1.find("\n}", bare));
    // The generated assignments check them before they assign the members.
    for (const auto* signature : {"auto X::operator=(std::string&& n) -> X&",
                                  "auto X::operator=(cpp2::impl::in<int> a) -> X&"}) {
        INFO(signature);
        const auto assignment = result.cpp1.find(signature);
        REQUIRE(assignment != std::string::npos);
        CHECK(result.cpp1.find("report_violation", assignment) <
              result.cpp1.find("name = ", assignment));
    }
    CHECK(compile_check(result.cpp1));
}
