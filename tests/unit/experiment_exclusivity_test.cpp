#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace {

auto translate(const std::string& source, bool enabled = true,
               std::string_view also = {}) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("exclusivity"));
    if (!also.empty()) REQUIRE(options.experiments.enable(also));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto violations(const cppfrontier::translation_result& result)
    -> std::vector<cppfrontier::diagnostic> {
    auto found = std::vector<cppfrontier::diagnostic>{};
    for (const auto& issue : result.diagnostics) {
        if (issue.code == "exclusivity-violation") found.push_back(issue);
    }
    return found;
}

auto at(const cppfrontier::translation_result& result, std::uint32_t line, std::uint32_t column)
    -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) {
            return issue.code == "exclusivity-violation" && issue.span.begin.line == line &&
                issue.span.begin.column == column;
        });
}

constexpr auto callees = std::string_view{
    "two: (inout a: int, inout b: int) = { a += b; }\n"
    "add: (inout a: int, b: int) = { a += b; }\n"
    "set: (out a: int, b: int) = { a = b; }\n"
    "size: (v: std::vector<int>) -> int = v.ssize();\n"};

}

TEST_CASE("an object passed inout cannot be used by another argument", "[experiments][exclusivity]") {
    const auto result = translate(std::string{callees} +
        "P: @struct type = { x: int = 0; y: int = 0; }\n"
        "main: () = {\n"
        "    x := 1;\n"
        "    two(x, x);\n"
        "    add(x, x);\n"
        "    add(x, x + 1);\n"
        "    p: P = ();\n"
        "    add(p.x, p.y);\n"
        "    y: int;\n"
        "    set(out y, x);\n"
        "    set(out y, y);\n"
        "    add(x, (x));\n"
        "}\n");
    const auto found = violations(result);
    REQUIRE(found.size() == 5);
    CHECK(at(result, 8, 12));
    CHECK(at(result, 9, 12));
    CHECK(at(result, 10, 12));
    // Distinct parts of an object are disjoint.
    CHECK_FALSE(at(result, 12, 14));
    CHECK(at(result, 15, 16));
    CHECK(at(result, 16, 13));
    CHECK(found.front().message ==
          "'x' is passed inout and also used by another argument of the same call");
    CHECK(found[3].message ==
          "'y' is passed out and also used by another argument of the same call");
}

TEST_CASE("the argument read while another is inout is reported", "[experiments][exclusivity]") {
    const auto result = translate(std::string{callees} +
        "main: () = {\n"
        "    x := 1;\n"
        "    add(x, x);\n"
        "    v: std::vector<int> = (1, 2);\n"
        "    add(v[0], size(v));\n"
        "    add(x,\n"
        "        x * 2);\n"
        "}\n");
    const auto found = violations(result);
    REQUIRE(found.size() == 3);
    CHECK(found[0].span.begin.line == 7);
    CHECK(found[0].span.begin.column == 12);
    CHECK(found[1].span.begin.line == 9);
    CHECK(found[1].span.begin.column == 20);
    CHECK(found[2].span.begin.line == 11);
    CHECK(found[2].span.begin.column == 9);
}

TEST_CASE("distinct objects, in parameters and unknown callees are not reported",
          "[experiments][exclusivity]") {
    const auto result = translate(std::string{callees} +
        "read: (a: int, b: int) -> int = a + b;\n"
        "P: @struct type = { x: int = 0; y: int = 0; }\n"
        "main: () = {\n"
        "    a := 1;\n"
        "    b := 2;\n"
        "    two(a, b);\n"
        "    add(a, b);\n"
        "    add(a, read(b, b));\n"
        "    _ = read(a, a);\n"
        "    unknown(a, a);\n"
        "    std::swap(a, a);\n"
        "    p: P = ();\n"
        "    q: P = ();\n"
        "    add(p.x, q.x);\n"
        "    c: int;\n"
        "    set(out c, a);\n"
        "    add(a, 1);\n"
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.code << ": " << issue.message);
    CHECK(violations(result).empty());
}

TEST_CASE("an inout receiver is the first argument of its call", "[experiments][exclusivity]") {
    const auto result = translate(std::string{callees} +
        "T: type = {\n"
        "    m: int = 0;\n"
        "    n: int = 0;\n"
        "    bump: (inout this, k: int) = { m += k; }\n"
        "    peek: (this, k: int) -> int = m + k;\n"
        "    run: (inout this) = {\n"
        "        bump(m);\n"
        "        bump(this.n);\n"
        "        this.bump(peek(1));\n"
        "        add(m, n);\n"
        "        add(m, this.m);\n"
        "        two(m, m);\n"
        "        _ = peek(m);\n"
        "    }\n"
        "}\n"
        "main: () = {\n"
        "    t: T = ();\n"
        "    t.bump(t.m);\n"
        "    t.bump(t.peek(0));\n"
        "    u: T = ();\n"
        "    t.bump(u.m);\n"
        "    x: int = 1;\n"
        "    x.add(x);\n"
        "    x.add(1);\n"
        "    y := 1;\n"
        "    y.add(y);\n"
        "    _ = t.peek(t.m);\n"
        "    v: std::vector<int> = (1);\n"
        "    v.push_back(v[0]);\n"
        "}\n");
    const auto found = violations(result);
    for (const auto& issue : found) INFO(issue.span.begin.line << ":" << issue.span.begin.column);
    REQUIRE(found.size() == 9);
    CHECK(at(result, 11, 14));
    CHECK(at(result, 12, 14));
    CHECK(at(result, 13, 19));
    CHECK(at(result, 15, 16));
    CHECK(at(result, 16, 16));
    CHECK(at(result, 22, 12));
    CHECK(at(result, 23, 12));
    CHECK(at(result, 27, 11));
    // `y` deduces `int` from its literal.
    CHECK(at(result, 30, 11));
    CHECK(found.front().message ==
          "'this' is passed inout and also used by another argument of the same call");
    CHECK(found[2].message ==
          "'this' is passed inout and also used by another argument of the same call");
    CHECK(found[3].message ==
          "'m' is passed inout and also used by another argument of the same call");
    CHECK(found[5].message ==
          "'t' is passed inout and also used by another argument of the same call");
}

TEST_CASE("overloads must agree on an inout parameter", "[experiments][exclusivity]") {
    const auto result = translate(
        "f: (inout a: int, b: int) = { a += b; }\n"
        "f: (a: double, b: int) = { }\n"
        "g: (inout a: int, b: int) = { a += b; }\n"
        "g: (inout a: double, b: int) = { }\n"
        "main: () = {\n"
        "    x := 1;\n"
        "    f(x, x);\n"
        "    g(x, x);\n"
        "}\n");
    const auto found = violations(result);
    REQUIRE(found.size() == 1);
    CHECK(found.front().span.begin.line == 8);
}

TEST_CASE("the check runs only when the experiment is enabled", "[experiments][exclusivity]") {
    const auto source = std::string{callees} +
        "main: () = {\n"
        "    x := 1;\n"
        "    two(x, x);\n"
        "}\n";
    const auto off = translate(source, false);
    REQUIRE_FALSE(off.has_errors());
    CHECK(violations(off).empty());
    CHECK(off.cpp1.find("two(x, x);") != std::string::npos);
    const auto on = translate(source);
    CHECK(on.has_errors());
    CHECK(on.cpp1.empty());
}

TEST_CASE("a use is a mention outside a compile-time context", "[experiments][exclusivity]") {
    const auto result = translate(std::string{callees} +
        "f: () = {\n"                                                 // 5
        "    n := 1;\n"                                               // 6
        "    add(n, sizeof(n));\n"                                    // 7
        "    add(n, :() -> int = n$;());\n"                           // 8
        "    add(n, std::ssize(\"(n)$\"));\n"                         // 9
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    CHECK(at(result, 8, 12));
    CHECK(at(result, 9, 23));
    CHECK(violations(result).size() == 2);
}

TEST_CASE("parts of an object are disjoint, and callees may be qualified or receivers grouped",
          "[experiments][exclusivity]") {
    const auto result = translate(std::string{callees} +
        "Point: type = { public a: int = 0; public b: int = 0; }\n"  // 5
        "bump: (inout p: Point, n: int) = { p.a += n; }\n"           // 6
        "N: namespace = { g: (inout x: int, y: int) = { x = y; } }\n"  // 7
        "holder: type = {\n"                                          // 8
        "    m: int = 0;\n"                                           // 9
        "    peek: (this, inout x: int) = { x = m; }\n"              // 10
        "    f: (inout this) = {\n"                                   // 11
        "        peek(m);\n"                                          // 12
        "        this.peek(m);\n"                                     // 13
        "    }\n"
        "}\n"
        "f: () = {\n"                                                 // 16
        "    p: Point = ();\n"                                        // 17
        "    add(p.a, p.b);\n"                                        // 18
        "    add(p.a, p.a);\n"                                        // 19
        "    bump(p, p.a);\n"                                         // 20
        "    n := 1;\n"                                               // 21
        "    N::g(n, n);\n"                                           // 22
        "    (p).bump(p.b);\n"                                        // 23
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    CHECK(at(result, 12, 14));
    CHECK(at(result, 13, 19));
    CHECK_FALSE(at(result, 18, 14));
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) { return issue.span.begin.line == 19; });
    REQUIRE(found != result.diagnostics.end());
    CHECK(found->message == "'p.a' is passed inout and also used by another argument of the same call");
    CHECK(at(result, 20, 13));
    CHECK(at(result, 22, 13));
    CHECK(at(result, 23, 14));
    CHECK(violations(result).size() == 6);
}

TEST_CASE("a member call on a deduced object is checked", "[experiments][exclusivity][review]") {
    const auto result = translate(
        "P: type = { public b: int = 0; bump: (inout this, v: int) = { b += v; } }\n"  // 1
        "main: () = {\n"                                                           // 2
        "    q := P();\n"                                                          // 3
        "    q.bump(q.b);\n"                                                       // 4
        "    r: P = ();\n"                                                         // 5
        "    (copy s := r) { s.bump(s.b); }\n"                                     // 6
        "}\n");
    CHECK(at(result, 4, 12));
    CHECK(at(result, 6, 28));
    CHECK(violations(result).size() == 2);
}

TEST_CASE("destructuring and named return initializers are checked once",
          "[experiments][exclusivity][review]") {
    const auto result = translate(std::string{callees} +
        "addr: (inout a: int, b: int) -> int = { a += b; return a; }\n"           // 5
        "f: (inout n: int) -> (r: int = addr(n, n)) = { }\n"                      // 6
        "g: (inout n: int) -> int = {\n"                                          // 7
        "    (a, b) := std::pair(addr(n, n), 1);\n"                               // 8
        "    return a + b;\n"                                                     // 9
        "}\n", true, "destructuring");
    CHECK(at(result, 6, 40));
    CHECK(at(result, 8, 33));
    CHECK(violations(result).size() == 2);
}

TEST_CASE("an argument's parts follow its member paths", "[experiments][exclusivity][review]") {
    const auto result = translate(std::string{callees} +
        "addb: (inout a: int, b: bool) = { if b { a += 1; } }\n"   // 5
        "Q: type = { public x: int = 0; public y: int = 0; }\n"    // 6
        "P: type = { public a: Q = (); public b: int = 0; }\n"     // 7
        "main: () = {\n"                                           // 8
        "    p: P = ();\n"                                         // 9
        "    addb(p.b, p.a.x < 3);\n"                              // 10
        "    add(p.a.x, p.a.y);\n"                                 // 11
        "    add(p.a.x, p.a.x);\n"                                 // 12
        "    add(p.a.x, p.a);\n"                                   // 13
        "}\n");
    CHECK(at(result, 12, 16));
    CHECK(at(result, 13, 16));
    CHECK(violations(result).size() == 2);
    const auto found = violations(result);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().message ==
          "'p.a.x' is passed inout and also used by another argument of the same call");
}

TEST_CASE("a call that cannot be resolved is not checked", "[experiments][exclusivity][review]") {
    const auto result = translate(
        "#include <vector>\n"                     // 1
        "main: () = {\n"                          // 2
        "    x: std::vector<int> = ();\n"         // 3
        "    lib(out x, x.size());\n"             // 4
        "}\n");
    CHECK(violations(result).empty());
}

TEST_CASE("a violation is located in its own text even when the name is used earlier",
          "[experiments][exclusivity][hunt]") {
    const auto result = translate(std::string{callees} +
        "f: (copy x: int) = {\n"                                      // 5
        "    x := add(x, x);\n"                                       // 6
        "    while x > 100 next add(x, x) { }\n"                      // 7
        "    while size(x) > 0 next add(x, x) { }\n"                  // 8
        "    g := :() = { w := 1; v := w + 1; add(w, w); _ = v; };\n" // 9
        "}\n");
    CHECK(at(result, 6, 17));
    CHECK(at(result, 7, 31));
    CHECK(at(result, 8, 35));
    CHECK(at(result, 9, 45));
    CHECK(violations(result).size() == 4);
}
