#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "test_support.hpp"

namespace {

auto translate(std::string_view source, bool enabled = true) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("newtype"));
    return cppfrontier::translator{}.translate_source(std::string{source}, options);
}

auto has(const std::string& text, std::string_view needle) -> bool {
    return text.find(needle) != std::string::npos;
}

auto find_diagnostic(const cppfrontier::translation_result& result, std::string_view code)
    -> const cppfrontier::diagnostic* {
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const cppfrontier::diagnostic& value) { return value.code == code; });
    return found == result.diagnostics.end() ? nullptr : &*found;
}

// Compiles `cpp1` with g++ and returns its exit status, or -1 without g++.
auto compile(std::string_view name, const std::string& cpp1, bool link) -> int {
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) return -1;
    const auto scratch = test_support::scratch_directory("cppfrontier-newtype");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (std::string{name} + ".cpp");
    test_support::write_file(cpp1_path, cpp1);
    auto arguments = std::vector<std::string>{compiler, "-std=c++23", "-I",
                                              CPPFRONTIER_RUNTIME_INCLUDE};
    if (link) {
        arguments.insert(arguments.end(), {"-o", (scratch / name).string()});
    }
    else {
        arguments.emplace_back("-fsyntax-only");
    }
    arguments.push_back(cpp1_path.string());
    const auto result = test_support::run_process(arguments, scratch, std::chrono::seconds{90});
    INFO("compiler output: " << result.output);
    REQUIRE_FALSE(result.timed_out);
    return result.exit_status;
}

constexpr auto newtype_program = std::string_view{
    "#include <iostream>\n"
    "Length: @newtype<float> type = {\n"
    "    describe: (this) -> std::string = \"length \" + std::to_string(this as float);\n"
    "}\n"
    "Area: @newtype<float> type = { }\n"
    "Count: @newtype<i32> type = { }\n"
    "operator*: (a: Length, b: Length) -> Area = Area((a as float) * (b as float));\n"
    "main: () -> int = {\n"
    "    l := 3.0f as Length;\n"
    "    m := l;\n"
    "    a := l * m;\n"
    "    n := 7 as Count;\n"
    "    std::cout << l.describe() << ' ' << (a as float) << ' ' << (l == m) << (l < m)\n"
    "              << ' ' << (n as i32) << '\\n';\n"
    "    return 0;\n"
    "}\n"};

constexpr auto quantity_program = std::string_view{
    "#include <iostream>\n"
    "Length: @quantity<float> type = { }\n"
    "Area: @quantity<float> type = { }\n"
    "operator*: (a: Length, b: Length) -> Area = Area((a as float) * (b as float));\n"
    "main: () -> int = {\n"
    "    l := 3.0f as Length;\n"
    "    m := l + l;\n"
    "    m -= 1.0f as Length;\n"
    "    n := -(2.0f * l) * 0.5f;\n"
    "    n += l / 3.0f;\n"
    "    n *= 2.0f;\n"
    "    a := l * m;\n"
    "    std::cout << (m as float) << ' ' << (n as float) << ' ' << (a as float) << ' '\n"
    "              << (m / l) << ' ' << (l <= m) << '\\n';\n"
    "    return 0;\n"
    "}\n"};

constexpr auto unit_program = std::string_view{
    "#include <iostream>\n"
    "Length: @unit<float, 1> type = { }\n"
    "Area: @unit<float, 2> type = { }\n"
    "Time: @unit<float, 0, 0, 1> type = { }\n"
    "Speed: @unit<float, 1, 0, -1> type = { }\n"
    "area: (a: Area) -> float = a as float;\n"
    "main: () -> int = {\n"
    "    l := 3.0f as Length;\n"
    "    a: Area = l * l;\n"
    "    s: Speed = l / (2.0f as Time);\n"
    "    ratio: float = l / l;\n"
    "    l += 1.0f as Length;\n"
    "    std::cout << area(a) << ' ' << (s as float) << ' ' << ratio << ' ' << (l as float)\n"
    "              << ' ' << (l == l) << '\\n';\n"
    "    return 0;\n"
    "}\n"};

}

TEST_CASE("@newtype generates a distinct value type over the underlying type",
          "[experiments][newtype]") {
    const auto result = translate("Length: @newtype<float> type = { }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(has(result.cpp1, "private: float value;"));
    CHECK(has(result.cpp1, "public: using cpp2_newtype_rep = float;"));
    CHECK(has(result.cpp1, "explicit Length(cpp2::impl::in<float> v);"));
    CHECK_FALSE(has(result.cpp1, "operator=(cpp2::impl::in<float> v)"));
    CHECK(has(result.cpp1, "Length([[maybe_unused]] Length const& that);"));
    CHECK(has(result.cpp1, "operator<=>([[maybe_unused]] Length const& that) const& -> auto = default;"));
    CHECK(has(result.cpp1, "constexpr explicit operator float() const { return value; }"));
    CHECK(has(result.cpp1, "friend constexpr auto as(Length const& x) -> float { return x.value; }"));
    CHECK_FALSE(has(result.cpp1, "operator+"));
    CHECK_FALSE(has(result.cpp1, "operator*"));
    CHECK_FALSE(has(result.cpp1, "cpp2units.h"));
}

TEST_CASE("@quantity adds the arithmetic of one quantity", "[experiments][newtype]") {
    const auto result = translate("Length: @quantity<float> type = { }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(has(result.cpp1, "auto operator+([[maybe_unused]] Length const& that) const& -> Length;"));
    CHECK(has(result.cpp1, "auto operator-([[maybe_unused]] Length const& that) const& -> Length;"));
    CHECK(has(result.cpp1, "auto operator-() const& -> Length;"));
    CHECK(has(result.cpp1, "auto operator+=([[maybe_unused]] Length const& that)& -> void;"));
    CHECK(has(result.cpp1, "auto operator-=([[maybe_unused]] Length const& that)& -> void;"));
    CHECK(has(result.cpp1, "auto operator*(cpp2::impl::in<float> k) const& -> Length;"));
    CHECK(has(result.cpp1, "auto operator/(cpp2::impl::in<float> k) const& -> Length;"));
    CHECK(has(result.cpp1, "auto operator/([[maybe_unused]] Length const& that) const& -> float;"));
    CHECK(has(result.cpp1, "[[nodiscard]] auto operator*(cpp2::impl::in<Length::cpp2_newtype_rep> "
                           "k, cpp2::impl::in<Length> q) -> Length;"));
    CHECK_FALSE(has(result.cpp1, "friend constexpr auto operator*"));
}

TEST_CASE("@quantity's k * q of a template type deduces from q only", "[experiments][newtype]") {
    const auto result = translate("Q: @quantity<T> <T: type, N: int> type = { }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(has(result.cpp1, "template<typename T, int N> [[nodiscard]] auto operator*("
                           "typename Q<T, N>::cpp2_newtype_rep const& k, Q<T, N> const& q) -> "
                           "Q<T, N>;"));
}

TEST_CASE("@quantity's k * q of a nested type is declared after the outermost type",
          "[experiments][newtype]") {
    const auto result = translate(
        "N: namespace = { A: type = { B: type = { Q: @quantity<float> type = { } } } }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(has(result.cpp1, "[[nodiscard]] auto operator*(cpp2::impl::in<A::B::Q::cpp2_newtype_rep> "
                           "k, cpp2::impl::in<A::B::Q> q) -> A::B::Q;"));
}

TEST_CASE("@newtype, @quantity and @unit reject what clashes with what they generate",
          "[experiments][newtype]") {
    struct rejected final {
        std::string_view source;
        std::string_view code;
        std::string message;
    };
    const auto generates = std::string_view{
        "' cannot be combined with '@newtype', which generates the type's members; only "
        "@hashable, @delegate and @print can"};
    for (const auto& [source, code, message] : {
             rejected{"C: @value @newtype<i32> type = { }\n", "newtype-composition",
                      "'@value" + std::string{generates}},
             rejected{"C: @newtype<i32> @value type = { }\n", "newtype-composition",
                      "'@value" + std::string{generates}},
             rejected{"C: @enum @newtype<i32> type = { a; b; }\n", "newtype-composition",
                      "'@enum" + std::string{generates}},
             rejected{"C: @unit<float, 1> @struct type = { }\n", "newtype-composition",
                      "'@struct' cannot be combined with '@unit', which makes the type an "
                      "alias; only @print can"},
             rejected{"C: @newtype<i32> type = { value: i32 = 0; }\n", "newtype-member-name",
                      "'@newtype' generates the member 'value' holding the underlying value, so "
                      "the type cannot declare its own"},
             rejected{"H: type = { protected L: @quantity<float> type = { } }\n", "quantity-scope",
                      "the 'k * q' operator of a @quantity type is declared at namespace scope, "
                      "which cannot name 'L': 'L' is a protected member of 'H'"},
             rejected{"Metres: @unit<float, 1> type = { }\nFeet: @unit<float, 01, 0> type = { }\n",
                      "unit-duplicate",
                      "'Feet' has the underlying type and exponents of 'Metres', so both would "
                      "name one type; a @unit type is distinguished by its dimensions only"}}) {
        const auto result = translate(source);
        INFO(source);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().code == code);
        CHECK(result.diagnostics.front().message == message);
    }
    for (const auto* source : {"C: @newtype<i32> @hashable type = { }\n",
                               "M: @unit<float, 1> type = { }\nS: @unit<double, 1> type = { }\n"}) {
        INFO(source);
        CHECK_FALSE(translate(source).has_errors());
    }
}

TEST_CASE("metaclass arguments nest template arguments and may not be empty",
          "[experiments][newtype]") {
    const auto result = translate(
        "V: @newtype<std::vector<i32>> type = { }\n"
        "M: @newtype<std::map<i32, std::vector<i32> > > type = { }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(has(result.cpp1, "private: std::vector<int> value;"));
    CHECK(has(result.cpp1, "private: std::map<int, std::vector<int>> value;"));

    const auto empty = translate(
        "A: @unit<double, 1,> type = { }\n"
        "B: @unit<double,,2> type = { }\n"
        "C: @newtype<,i32> type = { }\n");
    auto columns = std::vector<std::uint32_t>{};
    for (const auto& diagnostic : empty.diagnostics) {
        CHECK(diagnostic.code == "empty-metaclass-argument");
        columns.push_back(diagnostic.span.begin.column);
    }
    CHECK(columns == std::vector<std::uint32_t>{17, 13});
    CHECK(empty.diagnostics.front().message == "empty argument in the argument list of '@unit'");
}

TEST_CASE("@unit lowers to an alias of cpp2::quantity with padded exponents",
          "[experiments][newtype]") {
    const auto result = translate(
        "Length: @unit<float, 1> type = { }\n"
        "Speed: @unit<double, 1, 0, -1> type = { }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(has(result.cpp1, "#include \"cpp2units.h\""));
    CHECK(has(result.cpp1, "using Length = cpp2::quantity<float, cpp2::dims<1, 0, 0, 0, 0, 0, 0>>;"));
    CHECK(has(result.cpp1, "using Speed = cpp2::quantity<double, cpp2::dims<1, 0, -1, 0, 0, 0, 0>>;"));
    CHECK_FALSE(has(result.cpp1, "class Length"));
}

TEST_CASE("@newtype, @quantity and @unit need their underlying type", "[experiments][newtype]") {
    for (const auto* metaclass : {"newtype", "quantity", "unit"}) {
        const auto result = translate("Length: @" + std::string{metaclass} + " type = { }\n");
        const auto* found = find_diagnostic(result, "newtype-without-underlying-type");
        REQUIRE(found != nullptr);
        CHECK(found->span.begin.line == 1);
        CHECK(found->message == "'@" + std::string{metaclass} +
              "' needs the underlying type as its argument, as in '@" + metaclass + "<i64>'");
    }
}

TEST_CASE("@unit rejects members and non-integer exponents", "[experiments][newtype]") {
    const auto members = translate("Length: @unit<float, 1> type = { f: (this) = { } }\n");
    const auto* found = find_diagnostic(members, "unit-with-members");
    REQUIRE(found != nullptr);
    CHECK(found->span.begin.line == 1);
    CHECK(found->message == "a @unit type is an alias of cpp2::quantity and cannot declare "
                            "members; use @quantity for a type with members of its own");

    for (const auto* arguments : {"float, x", "float, 1, 2, 3, 4, 5, 6, 7, 8", "float, 1-2"}) {
        const auto result = translate("Length: @unit<" + std::string{arguments} + "> type = { }\n");
        INFO(arguments);
        const auto* dimension = find_diagnostic(result, "unit-dimension");
        REQUIRE(dimension != nullptr);
        CHECK(dimension->message == "'@unit' takes the underlying type and up to 7 integer "
                                    "exponents, as in '@unit<double, 1, 0, -1>'");
    }
}

TEST_CASE("as between two distinct types is rejected", "[experiments][newtype]") {
    const auto result = translate(
        "Length: @newtype<float> type = { }\n"
        "Area: @quantity<float> type = { }\n"
        "Metres: @unit<float, 1> type = { }\n"
        "Square: @unit<float, 2> type = { }\n"
        "f: (a: Length, m: Metres) = {\n"
        "    x := a as Area;\n"
        "    y := m as Square;\n"
        "    l: Length = 1.0f as Length;\n"
        "    g(l as Area);\n"
        "    same := a as Length;\n"
        "    deduced := 1.0f as Length;\n"
        "    made := Length(1.0f);\n"
        "    _ = deduced as Area;\n"
        "    _ = made as Area;\n"
        "    through := (a as float) as Area;\n"
        "    scalar := 2.0f as Area;\n"
        "}\n"
        "g: (_: Area) = { }\n");
    auto lines = std::vector<std::uint32_t>{};
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
        CHECK(diagnostic.code == "newtype-conversion");
        lines.push_back(diagnostic.span.begin.line);
    }
    CHECK(lines == std::vector<std::uint32_t>{6, 7, 9, 13, 14});
    REQUIRE(result.diagnostics.size() == 5);
    CHECK(result.diagnostics[0].message ==
          "no conversion between distinct types Length and Area - convert through the "
          "underlying type, as in '(a as float) as Area'");
    CHECK(result.diagnostics[1].message ==
          "no conversion between distinct types Metres and Square - convert through the "
          "underlying type, as in '(m as float) as Square'");
    CHECK(result.diagnostics[2].message ==
          "no conversion between distinct types Length and Area - convert through the "
          "underlying type, as in '(l as float) as Area'");
}

TEST_CASE("newtype, quantity and unit programs compile and run",
          "[experiments][newtype][runtime]") {
    struct program final {
        const char* name;
        std::string_view source;
        std::string_view expected;
    };
    for (const auto& [name, source, expected] : {
             program{"newtype", newtype_program, "length 3.000000 9 10 7\n"},
             program{"quantity", quantity_program, "5 -4 15 1.66667 1\n"},
             program{"unit", unit_program, "9 1.5 1 4 1\n"}}) {
        CAPTURE(name);
        const auto result = translate(source);
        INFO(result.cpp1);
        REQUIRE_FALSE(result.has_errors());
        const auto status = compile(name, result.cpp1, true);
        if (status < 0) SKIP("g++ is unavailable");
        REQUIRE(status == 0);
        const auto scratch = test_support::scratch_directory("cppfrontier-newtype");
        const auto run = test_support::run_process({(scratch / name).string()}, scratch,
                                                   std::chrono::seconds{10});
        INFO("program output: " << run.output);
        REQUIRE_FALSE(run.timed_out);
        CHECK(run.exit_status == 0);
        CHECK(run.output == expected);
    }
}

TEST_CASE("a distinct type does not convert implicitly", "[experiments][newtype][runtime]") {
    struct program final {
        const char* name;
        std::string_view source;
    };
    for (const auto& [name, source] : {
             program{"newtype-argument",
                     "Length: @newtype<float> type = { }\n"
                     "Area: @newtype<float> type = { }\n"
                     "f: (_: Area) = { }\n"
                     "main: () = { l := 1.0f as Length; f(l); }\n"},
             program{"newtype-underlying",
                     "Length: @newtype<float> type = { }\n"
                     "f: (_: float) = { }\n"
                     "main: () = { l := 1.0f as Length; f(l); g(2.0f); }\n"
                     "g: (_: Length) = { }\n"},
             program{"newtype-assignment",
                     "Length: @newtype<float> type = { }\n"
                     "main: () = { l := 1.0f as Length; l = 2.0f; }\n"},
             program{"newtype-arithmetic",
                     "Length: @newtype<float> type = { }\n"
                     "main: () = { l := 1.0f as Length; m := l + l; }\n"},
             program{"quantity-mixed",
                     "Length: @quantity<float> type = { }\n"
                     "Area: @quantity<float> type = { }\n"
                     "main: () = { l := 1.0f as Length; a := 1.0f as Area; _ = l + a; }\n"},
             program{"unit-argument",
                     "Length: @unit<float, 1> type = { }\n"
                     "Area: @unit<float, 2> type = { }\n"
                     "f: (_: Area) = { }\n"
                     "main: () = { l := 1.0f as Length; f(l); f(l + l); }\n"}}) {
        CAPTURE(name);
        const auto result = translate(source);
        INFO(result.cpp1);
        REQUIRE_FALSE(result.has_errors());
        const auto status = compile(name, result.cpp1, false);
        if (status < 0) SKIP("g++ is unavailable");
        CHECK(status != 0);
    }
}

TEST_CASE("@newtype, @quantity and @unit are unsupported unless the experiment is enabled",
          "[experiments][newtype]") {
    const auto result = translate(
        "Length: @newtype<float> type = { }\n"
        "Area: @quantity<float> type = { }\n"
        "Metres: @unit<float, 1> type = { }\n"
        "f: (a: Length) -> Area = a as Area;\n", false);
    auto unsupported = std::vector<std::uint32_t>{};
    for (const auto& diagnostic : result.diagnostics) {
        CHECK(diagnostic.code != "newtype-conversion");
        if (diagnostic.code == "unsupported-metaclass") {
            unsupported.push_back(diagnostic.span.begin.line);
        }
    }
    CHECK(unsupported == std::vector<std::uint32_t>{1, 2, 3});
    CHECK_FALSE(has(result.cpp1, "cpp2units.h"));
}
