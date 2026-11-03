#include <catch2/catch_test_macros.hpp>

#include <cppfrontier/translator.hpp>

#include <sstream>
#include <string>

namespace {

// Emitted Cpp1 without preprocessor lines and blank lines, so tests compare
// the complete generated code rather than probing for substrings.
auto emitted_body(const std::string& source, cppfrontier::translation_options options = {})
    -> std::string {
    const auto result = cppfrontier::translator{}.translate_source(source, std::move(options));
    INFO(source);
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    auto input = std::istringstream{result.cpp1};
    auto output = std::string{};
    for (auto line = std::string{}; std::getline(input, line);) {
        if (line.empty() || line.starts_with('#')) continue;
        output += line;
        output += '\n';
    }
    return output;
}

}

TEST_CASE("else and else-if bodies are emitted", "[golden][statements]") {
    CHECK(emitted_body(
        "main: () = { c := 1; if c == 1 { c = 2; } else if c == 3 { c = 4; } else { c = 5; } }\n") ==
        "auto main() -> int;\n"
        "auto main() -> int  {\n"
        "    auto c {1};\n"
        "    if (c == 1) {\n"
        "        c = 2;\n"
        "    } else if (c == 3) {\n"
        "        c = 4;\n"
        "    } else {\n"
        "        c = 5;\n"
        "    }\n"
        "}\n");
}

TEST_CASE("declarations sharing a line are all defined", "[golden][declarations]") {
    CHECK(emitted_body("f: () -> int = 1; g: () -> int = 2;\n") ==
        "[[nodiscard]] auto f() -> int;\n"
        "[[nodiscard]] auto g() -> int;\n"
        "[[nodiscard]] auto f() -> int { return 1; }\n"
        "[[nodiscard]] auto g() -> int { return 2; }\n");
}

TEST_CASE("argument rewrites accept an expression's first token", "[golden][expressions]") {
    CHECK(emitted_body("main: () = { out := 1; z := out + g(1, 2); }\n") ==
        "auto main() -> int;\n"
        "auto main() -> int  {\n"
        "    auto out {1};\n"
        "    auto z {cpp2::move(out) + g(1, 2)};\n"
        "}\n");
    CHECK(emitted_body("main: () = { z := : int = 3; }\n") ==
        "auto main() -> int;\n"
        "auto main() -> int  {\n"
        "    auto z {int{3}};\n"
        "}\n");
}

TEST_CASE("ssize keeps its signed result", "[golden][expressions]") {
    CHECK(emitted_body("main: () = { v: std::vector<int> = (1); p := v&; n := v.ssize() + p*.ssize(); }\n") ==
        "auto main() -> int;\n"
        "auto main() -> int  {\n"
        "    std::vector<int> v {1};\n"
        "    auto p {&(v)};\n"
        "    auto n {CPP2_UFCS(ssize)(CPP2_FORWARD(v)) + CPP2_UFCS(ssize)((*cpp2::impl::assert_not_null(cpp2::move(p))))};\n"
        "}\n");
}

TEST_CASE("as applies to the whole prefix operand", "[golden][expressions]") {
    CHECK(emitted_body("main: () = { x := 1; r := (x + 1) as long; s := (x)as long; t := -x as long; u := x + x as long; }\n") ==
        "auto main() -> int;\n"
        "auto main() -> int  {\n"
        "    auto x {1};\n"
        "    auto r {cpp2::impl::as_<long>((x + 1))};\n"
        "    auto s {cpp2::impl::as_<long>((x))};\n"
        "    auto t {cpp2::impl::as_<long>(-x)};\n"
        "    auto u {x + cpp2::impl::as_<long>(x)};\n"
        "}\n");
}

TEST_CASE("nested indexed calls keep matching brackets", "[golden][expressions]") {
    CHECK(emitted_body("main: () = { a: std::vector<int> = (1); b := a[a[0]](); c := g()[1]()[2](); }\n") ==
        "auto main() -> int;\n"
        "auto main() -> int  {\n"
        "    std::vector<int> a {1};\n"
        "    auto b {CPP2_ASSERT_IN_BOUNDS(a, CPP2_ASSERT_IN_BOUNDS_LITERAL(a, 0))()};\n"
        "    auto c {CPP2_ASSERT_IN_BOUNDS_LITERAL(CPP2_ASSERT_IN_BOUNDS_LITERAL(g(), 1)(), 2)()};\n"
        "}\n");
}

TEST_CASE("deferred initializers are rendered once with the active options", "[golden][initialization]") {
    auto options = cppfrontier::translation_options{};
    options.check_division_by_zero = false;
    CHECK(emitted_body("main: () = { a := 4; x: int; x = a / 2; }\n", options) ==
        "auto main() -> int;\n"
        "auto main() -> int  {\n"
        "    auto a {4};\n"
        "    cpp2::impl::deferred_init<int> x;\n"
        "    x.construct(std::type_identity_t<int>{cpp2::move(a) / 2});\n"
        "}\n");
    CHECK(emitted_body("S: type = { v: int = 1; f: (this) -> int = { x: int; x = this.v; return x; } }\n").find(
        "x.construct((*this).v);") != std::string::npos);
}

TEST_CASE("inner declarations shadow deferred locals", "[golden][initialization]") {
    CHECK(emitted_body("main: () = { x: int; x = 1; { x := 5; y := x; } z := x; }\n") ==
        "auto main() -> int;\n"
        "auto main() -> int  {\n"
        "    cpp2::impl::deferred_init<int> x;\n"
        "    x.construct(std::type_identity_t<int>{1});\n"
        "    {\n"
        "        auto x {5};\n"
        "        auto y {cpp2::move(x)};\n"
        "    }\n"
        "    auto z {cpp2::move(x.value())};\n"
        "}\n");
    CHECK(emitted_body("main: () = { x: int; { x: int; x = 1; } x = 2; }\n") ==
        "auto main() -> int;\n"
        "auto main() -> int  {\n"
        "    cpp2::impl::deferred_init<int> x;\n"
        "    {\n"
        "        cpp2::impl::deferred_init<int> x;\n"
        "        x.construct(std::type_identity_t<int>{1});\n"
        "    }\n"
        "    x.construct(std::type_identity_t<int>{2});\n"
        "}\n");
}

TEST_CASE("declarations follow the upstream emission phases", "[golden][declarations]") {
    CHECK(emitted_body("S: type = { v: int = 1; }\nf: (s: S) -> int = g();\ng: () -> int = 2;\n") ==
        "struct S;\n"
        "[[nodiscard]] auto g() -> int;\n"
        "struct S { private: int v {1};\n"
        "public: S() = default;\n"
        "S(S const&) = delete;\n"
        "auto operator=(S const&) -> void = delete;\n"
        "};\n"
        "[[nodiscard]] auto f(cpp2::impl::in<S> s) -> int;\n"
        "[[nodiscard]] auto f(cpp2::impl::in<S> s) -> int { return g(); }\n"
        "[[nodiscard]] auto g() -> int { return 2; }\n");
}

TEST_CASE("namespace members are forward declared", "[golden][declarations][namespaces]") {
    CHECK(emitted_body("N: namespace = { f: () -> int = g(); g: () -> int = 2; }\n") ==
        "namespace N {\n"
        "[[nodiscard]] auto f() -> int;\n"
        "[[nodiscard]] auto g() -> int;\n"
        "}\n"
        "namespace N {\n"
        "[[nodiscard]] auto f() -> int { return g(); }\n"
        "[[nodiscard]] auto g() -> int { return 2; }\n"
        "}\n");
}

TEST_CASE("dependent parameter types match whole identifiers", "[golden][templates]") {
    CHECK(emitted_body("f: <T> (v: std::vector<Type>, w: std::pair<int, T>) = { }\n") ==
        "template<typename T> auto f(cpp2::impl::in<std::vector<Type>> v, std::pair<int, T> const& w) -> void;\n"
        "template<typename T> auto f(cpp2::impl::in<std::vector<Type>> v, std::pair<int, T> const& w) -> void  {\n"
        "}\n");
}

TEST_CASE("digit separators stay inside numeric literals", "[golden][lexer]") {
    CHECK(emitted_body("main: () = { x := 10; y := 1'000 / x; e := 1.5e+3 / x; c := 'a'; d := 0x1'F; }\n") ==
        "auto main() -> int;\n"
        "auto main() -> int  {\n"
        "    auto x {10};\n"
        "    auto y {1'000 / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(1'000),x)};\n"
        "    auto e {1.5e+3 / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(1.5e+3),cpp2::move(x))};\n"
        "    auto c {'a'};\n"
        "    auto d {0x1'F};\n"
        "}\n");
}

TEST_CASE("a function returning a value is nodiscard", "[golden][declarations][ports]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "t: type = {\n"
        "    v: int = 0;\n"
        "    get: (this) -> int = v;\n"
        "    operator+=: (inout this, x: int) -> forward t = { v += x; return this; }\n"
        "    operator++: (inout this) -> forward t = { v++; return this; }\n"
        "}\n"
        "g: () -> _ = 1;\n"
        "h: () = 1;\n"
        "s: () = { }\n"
        "operator<<: (inout o: std::ostream, x: t) -> forward std::ostream = o << x.get();\n"
        "main: () = { f := :() -> int = 1; _ = f(); }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("[[nodiscard]] auto get() const& -> int") != std::string::npos);
    CHECK(result.cpp1.find("auto operator+=") != std::string::npos);
    for (const auto* name : {"operator+=(", "operator++(", "h()", "s()", "operator<<(", "main()"}) {
        INFO(name);
        CHECK(result.cpp1.find(std::string{"[[nodiscard]] auto "} + name) == std::string::npos);
    }
    CHECK(result.cpp1.find("[[nodiscard]] auto g()") != std::string::npos);
}
