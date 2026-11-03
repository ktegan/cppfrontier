#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

auto translate(const std::string& source, bool enabled = true) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("delegate"));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line, std::string_view message = {}) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) {
            return issue.code == code && issue.span.begin.line == line &&
                (message.empty() || issue.message == message);
        });
}

auto contains(const std::string& text, std::string_view part) -> bool {
    return text.find(part) != std::string::npos;
}

const auto logger = std::string{
    "#include <iostream>\n"
    "#include <string>\n"
    "Logger: type = {\n"
    "    log: (this, s: std::string) = { std::cout << \"[\" << level() << \"] \" << s << \"\\n\"; }\n"
    "    level: (this) -> int = level_;\n"
    "    set_level: (inout this, l: int) = { level_ = l; }\n"
    "    take: (this, move s: std::string) -> std::string = s + \"!\";\n"
    "    twice: (this, x) = x * 2;\n"
    "    bump: (this, inout n: int) = { n++; }\n"
    "    operator+: (this, n: int) -> int = level_ + n;\n"
    "    private level_: int = 1;\n"
    "    private hidden: (this) = {}\n"
    "}\n"};

const auto service = std::string{
    "Service: @delegate<inner> type = {\n"
    "    inner: Logger = ();\n"
    "    work: (this) = { inner.log(\"working\"); }\n"
    "    level: (this) -> int = 42;\n"
    "}\n"};

}

TEST_CASE("@delegate forwards the public functions of a member's type", "[experiments][delegate]") {
    const auto result = translate(logger + service);
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    // The same signature, `this` passing style and return type as the original.
    CHECK(contains(cpp1, "auto Service::log(cpp2::impl::in<std::string> s) const& -> void  {\n"
                         "    CPP2_UFCS(log)((*this).inner, s);\n}"));
    CHECK(contains(cpp1, "auto Service::set_level(cpp2::impl::in<int> l)& -> void  {\n"
                         "    CPP2_UFCS(set_level)((*this).inner, l);\n}"));
    CHECK(contains(cpp1, "[[nodiscard]] auto Service::take(std::string&& s) const& -> std::string  {\n"
                         "    return CPP2_UFCS(take)((*this).inner, CPP2_FORWARD(s));\n}"));
    CHECK(contains(cpp1, "auto Service::twice(auto const& x) const& -> decltype(auto) "
                         "{ return CPP2_UFCS(twice)((*this).inner, x); }"));
    CHECK(contains(cpp1, "auto Service::bump(int& n) const& -> void  {\n"
                         "    CPP2_UFCS(bump)((*this).inner, n);\n}"));
    // A function the type already has is kept; private functions and operators
    // are not forwarded.
    CHECK(contains(cpp1, "auto Service::level() const& -> int { return 42; }"));
    CHECK_FALSE(contains(cpp1, "Service::hidden"));
    CHECK_FALSE(contains(cpp1, "Service::operator+"));
    CHECK(contains(cpp1, "auto Service::work() const& -> void"));
}

TEST_CASE("@delegate forwarding functions compile and run", "[experiments][delegate]") {
    const auto translation = translate(logger + service +
        "main: () = {\n"
        "    s: Service = ();\n"
        "    s.log(\"hello\");\n"
        "    s.set_level(5);\n"
        "    s.work();\n"
        "    std::cout << s.level() << \" \" << s.take(\"moved\") << \" \" << s.twice(21) << \"\\n\";\n"
        "    n := 1;\n"
        "    s.bump(n);\n"
        "    std::cout << n << \"\\n\";\n"
        "}\n");
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-delegate");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "delegate-run.cpp";
    const auto executable_path = scratch / "delegate-run";
    test_support::write_file(cpp1_path, translation.cpp1);
    const auto compiled = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
        scratch, std::chrono::seconds{120});
    INFO("compiler output: " << compiled.output);
    REQUIRE(compiled.exit_status == 0);
    const auto ran = test_support::run_process({executable_path.string()}, scratch,
                                               std::chrono::seconds{10});
    REQUIRE_FALSE(ran.timed_out);
    CHECK(ran.exit_status == 0);
    CHECK(ran.output == "[1] hello\n[5] working\n42 moved! 42\n2\n");
}

TEST_CASE("@delegate forwards only the selected functions", "[experiments][delegate]") {
    const auto result = translate(logger +
        "Service: @delegate<inner, log, set_level> type = {\n"
        "    inner: Logger = ();\n"
        "}\n");
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result.cpp1, "Service::log("));
    CHECK(contains(result.cpp1, "Service::set_level("));
    CHECK_FALSE(contains(result.cpp1, "Service::level("));
    CHECK_FALSE(contains(result.cpp1, "Service::take("));
}

TEST_CASE("several @delegates forward from several members", "[experiments][delegate]") {
    const auto result = translate(
        "A: type = { f: (this) -> int = 1; g: (this) -> int = 2; }\n"
        "B: type = { h: (this) -> int = 3; }\n"
        "Both: @delegate<a> @delegate<b> type = { a: A = (); b: B = (); }\n");
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result.cpp1, "return CPP2_UFCS(f)((*this).a);"));
    CHECK(contains(result.cpp1, "return CPP2_UFCS(g)((*this).a);"));
    CHECK(contains(result.cpp1, "return CPP2_UFCS(h)((*this).b);"));
}

TEST_CASE("a function two @delegates would forward is ambiguous", "[experiments][delegate]") {
    const auto result = translate(
        "A: type = { f: (this) -> int = 1; g: (this) -> int = 2; }\n"
        "B: type = { f: (this) -> int = 3; h: (this) -> int = 4; }\n"
        "Both: @delegate<a> @delegate<b> type = { a: A = (); b: B = (); }\n"
        "Fine: @delegate<a> @delegate<b, h> type = { a: A = (); b: B = (); }\n"
        "Own: @delegate<a> @delegate<b> type = { a: A = (); b: B = (); f: (this) -> int = 0; }\n");
    CHECK(has_diagnostic(result, "delegate-ambiguous", 3,
                         "'f' is forwarded by more than one @delegate of 'Both'"));
    CHECK(result.diagnostics.size() == 1);
}

TEST_CASE("@delegate finds nested, namespaced and later types", "[experiments][delegate]") {
    const auto result = translate(
        "N: namespace = { Later: type = { f: (this) -> int = 1; } }\n"
        "Outer: @delegate<n> @delegate<l> @delegate<a, g> type = {\n"
        "    Nested: type = { nested: (this) -> int = 2; }\n"
        "    n: Nested = ();\n"
        "    l: N::Later = ();\n"
        "    a: After = ();\n"
        "}\n"
        "After: type = { f: (this) -> int = 3; g: (this) -> int = 4; }\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result.cpp1, "return CPP2_UFCS(nested)((*this).n);"));
    CHECK(contains(result.cpp1, "return CPP2_UFCS(f)((*this).l);"));
    CHECK_FALSE(contains(result.cpp1, "return CPP2_UFCS(f)((*this).a);"));
}

TEST_CASE("@delegate diagnoses what it cannot forward", "[experiments][delegate]") {
    const auto result = translate(
        "#include <vector>\n"
        "A: type = { f: (this) = {} }\n"
        "Sel: @delegate<a, f, nope> type = { a: A = (); }\n"
        "NoMember: @delegate<zz> type = { a: A = (); }\n"
        "NoType: @delegate<v> type = { v: std::vector<int> = (); }\n"
        "Tmpl: <T> type = { f: (this) = {} }\n"
        "Templated: @delegate<t> type = { t: Tmpl<int> = (); }\n"
        "Bare: @delegate type = { a: A = (); }\n"
        "Base: @delegate<this> type = { this: A = (); }\n"
        "B: type = { make: () -> int = 1; private secret: (this) -> int = 2; }\n"
        "Hidden: @delegate<b, make, secret> type = { b: B = (); }\n");
    CHECK(has_diagnostic(result, "delegate-unknown-function", 3,
                         "'A' has no function named 'nope'"));
    CHECK(has_diagnostic(result, "delegate-unknown-member", 4,
                         "'NoMember' has no data member named 'zz'"));
    CHECK(has_diagnostic(result, "delegate-unknown-type", 5,
        "the type 'std::vector<int>' of 'v' is not declared in this translation unit"));
    CHECK(has_diagnostic(result, "delegate-template-type", 7,
                         "cannot delegate to 't' because its type 'Tmpl' is a template"));
    CHECK(has_diagnostic(result, "delegate-unknown-member", 8,
                         "@delegate needs the name of a data member, as in @delegate<m>"));
    CHECK(has_diagnostic(result, "delegate-unknown-member", 9));
    for (const auto* name : {"make", "secret"}) {
        CHECK(has_diagnostic(result, "delegate-not-forwardable", 11,
                             "'B::" + std::string{name} + "' is not a public function taking an "
                             "'in' or 'inout' 'this', so @delegate cannot forward it"));
    }
    CHECK(result.diagnostics.size() == 8);
}

TEST_CASE("@delegate is unsupported unless the experiment is enabled", "[experiments][delegate]") {
    const auto result = translate(logger + service, false);
    CHECK(has_diagnostic(result, "unsupported-metaclass", 14, "'@delegate' requires '-experiment=delegate'"));
    CHECK_FALSE(contains(result.cpp1, "Service::log"));
    const auto twice = translate(
        "A: type = { f: (this) -> int = 1; }\n"
        "Both: @delegate<a> @delegate<a, f> type = { a: A = (); }\n", false);
    CHECK(twice.diagnostics.size() == 1);
}

TEST_CASE("@delegate forwards through this, with the source's types and template arguments",
          "[experiments][delegate]") {
    const auto result = translate(
        "lib: namespace = {\n"
        "    Point: type = { }\n"
        "    Counter: type = {\n"
        "        Kind: type == i32;\n"
        "        add: (inout this, counter: Kind) = { }\n"
        "        at: (this, p: Point) -> Point = p;\n"
        "        pair: (this) -> (a: i32, b: i32) = { a = 1; b = 2; }\n"
        "        conv: <T> (this) -> T = T();\n"
        "    }\n"
        "}\n"
        "Service: @delegate<counter> type = { counter: lib::Counter = (); }\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    CHECK(contains(cpp1, "auto Service::add(cpp2::impl::in<::lib::Counter::Kind> counter)& "
                         "-> void  {\n    CPP2_UFCS(add)((*this).counter, counter);\n}"));
    CHECK(contains(cpp1, "auto Service::at(cpp2::impl::in<::lib::Point> p) const& -> ::lib::Point"));
    CHECK(contains(cpp1, "auto Service::pair() const& -> ::lib::Counter::pair_ret"));
    CHECK(contains(cpp1, "return CPP2_UFCS_TEMPLATE(conv<T>)((*this).counter);"));
}
