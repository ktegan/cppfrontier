#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace {

auto translate(const std::string& source, std::vector<std::string> experiments = {"checked-arithmetic"})
    -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    for (const auto& name : experiments) REQUIRE(options.experiments.enable(name));
    return cppfrontier::translator{}.translate_source(source, options);
}

struct run_result final {
    int exit_status = 0;
    std::string output;
};

// Compiles and runs the translation of `source` with the experiment enabled.
auto compile_and_run(const std::string& source, const std::string& name) -> run_result {
    const auto translation = translate(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-checked");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (name + ".cpp");
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
    return {ran.exit_status, ran.output};
}

}

TEST_CASE("arithmetic on known integers goes through the checked helpers",
          "[experiments][checked-arithmetic]") {
    const auto result = translate(
        "f: (a: int, b: i64, c: std::size_t, u: u32) -> int = {\n"
        "    x := 1;\n"
        "    y: u8 = 3;\n"
        "    s := a + b - c;\n"
        "    p := a * x / 2 % 3;\n"
        "    n := -a;\n"
        "    g := -(a + b);\n"
        "    v := (a + 1) as i64;\n"
        "    x++;\n"
        "    y--;\n"
        "    x += a;\n"
        "    x -= 2;\n"
        "    x *= y;\n"
        "    x = x + u;\n"
        "    return x;\n"
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    INFO(cpp1);
    CHECK(cpp1.find("#include \"cpp2util.h\"\n#include \"cpp2checked.h\"\n") != std::string::npos);
    CHECK(cpp1.find("auto s {cpp2::checked_sub(cpp2::checked_add(a, b), c)};") != std::string::npos);
    CHECK(cpp1.find("auto p {cpp2::checked_mul(a, x) / CPP2_ASSERT_NOT_ZERO_LITERAL("
                    "CPP2_TYPEOF(cpp2::checked_mul(a, x)),2) % CPP2_ASSERT_NOT_ZERO_LITERAL(") !=
          std::string::npos);
    CHECK(cpp1.find("auto n {cpp2::checked_neg(a)};") != std::string::npos);
    CHECK(cpp1.find("auto g {cpp2::checked_neg(cpp2::checked_add(a, b))};") != std::string::npos);
    CHECK(cpp1.find("cpp2::impl::as_<cpp2::i64>((cpp2::checked_add(a, 1)))") != std::string::npos);
    CHECK(cpp1.find("    cpp2::checked_add_assign(x, 1);\n") != std::string::npos);
    CHECK(cpp1.find("    cpp2::checked_sub_assign(y, 1);\n") != std::string::npos);
    CHECK(cpp1.find("    cpp2::checked_add_assign(x, a);\n") != std::string::npos);
    CHECK(cpp1.find("    cpp2::checked_sub_assign(x, 2);\n") != std::string::npos);
    CHECK(cpp1.find("    cpp2::checked_mul_assign(x, cpp2::move(y));\n") != std::string::npos);
    CHECK(cpp1.find("    x = cpp2::checked_add(x, u);\n") != std::string::npos);
}

TEST_CASE("operands that are not known integers keep the plain operators",
          "[experiments][checked-arithmetic]") {
    const auto result = translate(
        "#include <string>\n"
        "T: type = { v: int = 0; }\n"
        "g: (x: int) -> int = { return x; }\n"
        "f: (a: int, d: double, s: std::string, t: T, p: *int, w: _) -> int = {\n"
        "    e := d + 1;\n"
        "    dd := d;\n"
        "    dd += a;\n"
        "    dd++;\n"
        "    m := -d;\n"
        "    str := s + \"x\";\n"
        "    mixed := a + d;\n"
        "    called := g(a) + a;\n"
        "    member := t.v + 1;\n"
        "    pointed := p* + 1;\n"
        "    deduced := w + a;\n"
        "    trailing := a + g(a) + a;\n"
        "    constant := 2 + 3;\n"
        "    negative := -5;\n"
        "    flag := true;\n"
        "    b := flag + 1;\n"
        "    return a;\n"
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    INFO(cpp1);
    CHECK(cpp1.find("auto e {d + 1};") != std::string::npos);
    CHECK(cpp1.find("    dd += a;\n") != std::string::npos);
    CHECK(cpp1.find("    ++dd;\n") != std::string::npos);
    CHECK(cpp1.find("auto m {-d};") != std::string::npos);
    CHECK(cpp1.find("auto str {s + \"x\"};") != std::string::npos);
    CHECK(cpp1.find("auto mixed {a + d};") != std::string::npos);
    CHECK(cpp1.find("auto called {g(a) + a};") != std::string::npos);
    CHECK(cpp1.find("auto member {t.v + 1};") != std::string::npos);
    CHECK(cpp1.find("auto pointed {*cpp2::impl::assert_not_null(p) + 1};") != std::string::npos);
    CHECK(cpp1.find("auto deduced {w + a};") != std::string::npos);
    // The left fold checks `a + g(a)` only up to the unknown operand.
    CHECK(cpp1.find("auto trailing {a + g(a) + a};") != std::string::npos);
    CHECK(cpp1.find("auto constant {2 + 3};") != std::string::npos);
    CHECK(cpp1.find("auto negative {-5};") != std::string::npos);
    CHECK(cpp1.find("auto b {cpp2::move(flag) + 1};") != std::string::npos);
    CHECK(cpp1.find("cpp2checked.h") == std::string::npos);
}

// A call is never a known integer, so a checked compound assignment has no
// `out` argument, and C++17's right-before-left order of `+=` still holds for one.
TEST_CASE("a compound assignment with an out argument keeps the plain operator",
          "[experiments][checked-arithmetic]") {
    const auto result = translate(
        "f: (out x: int) -> int = { x = 1; return 2; }\n"
        "main: () = {\n"
        "    i: int;\n"
        "    n := 0;\n"
        "    n += f(out i);\n"
        "    n += i;\n"
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("n += f(cpp2::impl::out(&i));") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::checked_add_assign(n, cpp2::move(i.value()))") !=
          std::string::npos);
}

TEST_CASE("a data member declared with an integer type is a known integer",
          "[experiments][checked-arithmetic]") {
    const auto result = translate(
        "C: type = {\n"
        "    x: int = 0;\n"
        "    d: double = 0;\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, that) = { x = that.x + 1; }\n"
        "    bump: (inout this) = { x += 1; this.x -= 2; }\n"
        "    sum: (this, that) -> int = this.x * that.x;\n"
        "    scaled: (this) -> double = this.d + 1;\n"
        "}\n"
        "total: (v: std::vector<int>) -> int = {\n"
        "    s: int = 0;\n"
        "    for v do (e) { s += e; }\n"
        "    return s;\n"
        "}\n"
        "Meters: @quantity<i32> type = { }\n",
        {"checked-arithmetic", "newtype"});
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    INFO(cpp1);
    for (const auto* text : {"x = cpp2::checked_add(that.x, 1);", "cpp2::checked_add_assign(x, 1);",
                             "cpp2::checked_sub_assign((*this).x, 2);",
                             "{ return cpp2::checked_mul((*this).x, that.x); }",
                             "{ return (*this).d + 1; }", "s += e;",
                             "Meters(cpp2::checked_add(value, that.value))",
                             "Meters(cpp2::checked_sub(value, that.value))",
                             "cpp2::checked_add_assign(value, that.value);",
                             "cpp2::checked_sub_assign(value, that.value);",
                             "Meters(cpp2::checked_mul(value, k))"}) {
        CAPTURE(text);
        CHECK(cpp1.find(text) != std::string::npos);
    }
}

TEST_CASE("checked arithmetic computes in-range results as usual",
          "[experiments][checked-arithmetic]") {
    const auto ran = compile_and_run(
        "#include <iostream>\n"
        "main: () -> int = {\n"
        "    a: i32 = 2000000000;\n"
        "    b := 147483647;\n"
        "    c: i8 = 100;\n"
        "    u: u64 = 18446744073709551615u;\n"
        "    std::cout << a + b << \" \" << c * 1 << \" \" << (c as i32) - 200 << \" \"\n"
        "              << -(a as i64) << \"\\n\";\n"
        "    small: i8 = 27;\n"
        "    small += 100;\n"
        "    std::cout << (small as int) << \" \" << u - 1 << \"\\n\";\n"
        "    i := 0;\n"
        "    while i < 3 { i++; }\n"
        "    std::cout << i << \"\\n\";\n"
        "    return 0;\n"
        "}\n", "checked-in-range");
    CHECK(ran.exit_status == 0);
    CHECK(ran.output == "2147483647 100 -100 -2000000000\n127 18446744073709551614\n3\n");
}

TEST_CASE("an overflowing operation terminates the program",
          "[experiments][checked-arithmetic]") {
    SECTION("addition") {
        const auto ran = compile_and_run(
            "#include <iostream>\n"
            "main: () -> int = {\n"
            "    a: i32 = 2000000000;\n"
            "    b := 147483648;\n"
            "    std::cout << \"before\\n\";\n"
            "    x := a + b;\n"
            "    std::cout << \"after \" << x << \"\\n\";\n"
            "    return 0;\n"
            "}\n", "checked-overflow-add");
        CHECK(ran.exit_status != 0);
        CHECK(ran.output.find("before\n") != std::string::npos);
        CHECK(ran.output.find("after") == std::string::npos);
        CHECK(ran.output.find("Arithmetic safety violation: integer addition overflow") !=
              std::string::npos);
    }
    SECTION("increment of a narrow type") {
        const auto ran = compile_and_run(
            "#include <iostream>\n"
            "main: () -> int = {\n"
            "    c: i8 = 127;\n"
            "    c++;\n"
            "    std::cout << \"unreached \" << (c as int) << \"\\n\";\n"
            "    return 0;\n"
            "}\n", "checked-overflow-inc");
        CHECK(ran.exit_status != 0);
        CHECK(ran.output.find("unreached") == std::string::npos);
        CHECK(ran.output.find("Arithmetic safety violation: integer addition overflow") !=
              std::string::npos);
    }
    SECTION("multiplication and negation") {
        const auto ran = compile_and_run(
            "#include <iostream>\n"
            "main: () -> int = {\n"
            "    m: i64 = 9223372036854775807;\n"
            "    std::cout << -m << \"\\n\";\n"
            "    k: i32 = 65536;\n"
            "    k *= k;\n"
            "    std::cout << \"unreached \" << k << \"\\n\";\n"
            "    return 0;\n"
            "}\n", "checked-overflow-mul");
        CHECK(ran.exit_status != 0);
        CHECK(ran.output.find("-9223372036854775807\n") != std::string::npos);
        CHECK(ran.output.find("unreached") == std::string::npos);
        CHECK(ran.output.find("Arithmetic safety violation: integer multiplication overflow") !=
              std::string::npos);
    }
}

TEST_CASE("the builtin and portable checks agree on mixed-sign, char and bool operands",
          "[experiments][checked-arithmetic][review]") {
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-checked-paths");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "checked-paths.cpp";
    // The static_asserts hold on both paths, and the program exits 0.
    test_support::write_file(cpp1_path, R"cpp(#include "cpp2checked.h"
#include <cstdint>
#include <limits>

namespace {

template<class R, class A, class B>
constexpr auto add(A a, B b) -> std::pair<bool, R> { R r{}; const auto o = cpp2::impl::add_overflows(a, b, r); return {o, o ? R{} : r}; }
template<class R, class A, class B>
constexpr auto sub(A a, B b) -> std::pair<bool, R> { R r{}; const auto o = cpp2::impl::sub_overflows(a, b, r); return {o, o ? R{} : r}; }
template<class R, class A, class B>
constexpr auto mul(A a, B b) -> std::pair<bool, R> { R r{}; const auto o = cpp2::impl::mul_overflows(a, b, r); return {o, o ? R{} : r}; }

template<class R> constexpr auto ok(R value) { return std::pair{false, value}; }
template<class R> constexpr auto overflow() { return std::pair{true, R{}}; }

using imax = std::intmax_t;
using umax = std::uintmax_t;
constexpr auto imin = std::numeric_limits<imax>::min();
constexpr auto ihi = std::numeric_limits<imax>::max();
constexpr auto uhi = std::numeric_limits<umax>::max();
constexpr auto int_min = std::numeric_limits<int>::min();

static_assert(add<unsigned>(5u, -1) == ok(4u));
static_assert(add<unsigned>(0u, -1) == overflow<unsigned>());
static_assert(sub<unsigned>(5u, -1) == ok(6u));
static_assert(sub<unsigned>(5u, 6) == overflow<unsigned>());
static_assert(add<int>(-5, 3u) == ok(-2));
static_assert(mul<unsigned>(2u, -1) == overflow<unsigned>());
static_assert(mul<unsigned>(0u, -1) == ok(0u));
static_assert(mul<int>(-1, int_min) == overflow<int>());
static_assert(mul<int>(int_min, 1) == ok(int_min));
static_assert(mul<long long>(-3, 4) == ok(-12LL));
static_assert(add<umax>(uhi, 1) == overflow<umax>());
static_assert(add<umax>(uhi, -1) == ok(uhi - 1));
static_assert(add<imax>(imin, -1) == overflow<imax>());
static_assert(add<imax>(imin, uhi) == ok(ihi));
static_assert(sub<imax>(0, imin) == overflow<imax>());
static_assert(sub<imax>(-1, ihi) == ok(imin));
static_assert(mul<imax>(imin, -1) == overflow<imax>());
static_assert(mul<umax>(uhi, uhi) == overflow<umax>());
static_assert(add<signed char>(static_cast<signed char>(100), 27) == ok(static_cast<signed char>(127)));
static_assert(add<signed char>(static_cast<signed char>(100), 28) == overflow<signed char>());
static_assert(add<char>('a', 1) == ok('b'));
static_assert(add<int>(true, true) == ok(2));
static_assert(add<bool>(false, true) == ok(true));
static_assert(add<bool>(true, true) == overflow<bool>());

}

int main() {
    char c = 'a';
    cpp2::checked_add_assign(c, 1);
    bool b = false;
    cpp2::checked_add_assign(b, 1);
    return c == 'b' && b && cpp2::checked_add(5u, -1) == 4u ? 0 : 1;
}
)cpp");
    for (const auto* define : {"-DCPP2_CHECKED_HAS_BUILTINS=1", "-DCPP2_CHECKED_HAS_BUILTINS=0"}) {
        INFO(define);
        const auto executable_path = scratch / "checked-paths";
        const auto compiled = test_support::run_process({compiler, "-std=c++23", define, "-I",
            CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
            scratch, std::chrono::seconds{120});
        INFO("compiler output: " << compiled.output);
        REQUIRE(compiled.exit_status == 0);
        const auto ran = test_support::run_process({executable_path.string()}, scratch,
                                                   std::chrono::seconds{10});
        CHECK(ran.exit_status == 0);
    }
}

TEST_CASE("checked arithmetic changes nothing unless enabled",
          "[experiments][checked-arithmetic]") {
    const auto source = std::string{
        "f: (a: int, b: i64) -> int = {\n"
        "    x := a + b * 2;\n"
        "    x++;\n"
        "    x -= -a;\n"
        "    return x / 3;\n"
        "}\n"};
    const auto off = translate(source, {});
    REQUIRE_FALSE(off.has_errors());
    CHECK(off.cpp1 == translate(source, {"defer", "unsafe"}).cpp1);
    CHECK(off.cpp1.find("checked") == std::string::npos);
    CHECK(off.cpp1.find("auto x {a + b * 2};") != std::string::npos);
    CHECK(off.cpp1.find("    ++x;\n") != std::string::npos);
    CHECK(off.cpp1.find("    x -= -a;\n") != std::string::npos);
    CHECK(off.cpp1 != translate(source).cpp1);
}

TEST_CASE("an unsafe block uses the plain operators", "[experiments][checked-arithmetic]") {
    const auto result = translate(
        "f: (a: int) -> int = {\n"
        "    x := a + 1;\n"
        "    unsafe {\n"
        "        x = x * a;\n"
        "        x++;\n"
        "        x -= -a;\n"
        "    }\n"
        "    x += a;\n"
        "    return x;\n"
        "}\n", {"checked-arithmetic", "unsafe"});
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    INFO(cpp1);
    CHECK(cpp1.find("auto x {cpp2::checked_add(a, 1)};") != std::string::npos);
    CHECK(cpp1.find("{ // unsafe\n        x = x * a;\n        ++x;\n        x -= -a;\n    }\n") !=
          std::string::npos);
    CHECK(cpp1.find("    cpp2::checked_add_assign(x, a);\n") != std::string::npos);
}
