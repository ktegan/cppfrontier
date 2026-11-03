#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

namespace {

auto translate(const std::string& source, bool enabled = true,
               std::initializer_list<std::string_view> also = {})
    -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("linear"));
    for (const auto other : also) REQUIRE(options.experiments.enable(other));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto diagnostic_at(const cppfrontier::translation_result& result, std::string_view code,
                   std::uint32_t line) -> const cppfrontier::diagnostic* {
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) { return issue.code == code && issue.span.begin.line == line; });
    return found == result.diagnostics.end() ? nullptr : &*found;
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line) -> bool {
    return diagnostic_at(result, code, line) != nullptr;
}

auto count_of(const cppfrontier::translation_result& result, std::string_view prefix)
    -> std::size_t {
    return static_cast<std::size_t>(std::count_if(result.diagnostics.begin(),
        result.diagnostics.end(),
        [&](const auto& issue) { return issue.code.starts_with(prefix); }));
}

auto run_program(const std::string& source, std::string_view name) -> std::string {
    const auto translation = translate(source);
    INFO(translation.cpp1);
    for (const auto& issue : translation.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(translation.has_errors());
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-linear");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (std::string{name} + ".cpp");
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
    REQUIRE(ran.exit_status == 0);
    return ran.output;
}

// A linear token, which `release` consumes, and a function that consumes one.
const auto prelude = std::string{
    "#include <iostream>\n"                                      // 1
    "#include <vector>\n"                                        // 2
    "Token: @linear type = {\n"                                  // 3
    "    public id: int; release: (move this) = { }\n"            // 4
    "    operator=: (out this, id_: int) = { id = id_; }\n"      // 5
    "}\n"                                                        // 6
    "consume: (move t: Token) = { std::cout << \"consumed \" << t.id << \"\\n\"; t.release(); }\n"  // 7
};

}

TEST_CASE("@linear deletes the copy functions and keeps memberwise moves",
          "[experiments][linear]") {
    const auto result = translate(prelude);
    INFO(result.cpp1);
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("Token(Token const&) = delete;") != std::string::npos);
    CHECK(result.cpp1.find("auto operator=(Token const&) -> void = delete;") != std::string::npos);
    CHECK(result.cpp1.find("Token([[maybe_unused]] Token&& that) noexcept") != std::string::npos);
    CHECK(result.cpp1.find("auto operator=([[maybe_unused]] Token&& that) noexcept -> Token&") != std::string::npos);
    CHECK(result.cpp1.find("id{cpp2::move(that).id}") != std::string::npos);

    const auto copying = translate(
        "Token: @linear type = {\n"
        "    id: int;\n"
        "    operator=: (out this, that) = { }\n"
        "}\n");
    const auto* copy = diagnostic_at(copying, "linear-copy", 3);
    REQUIRE(copy != nullptr);
    CHECK(copy->message ==
          "a @linear type cannot be copied: operator= must take 'move that', not 'that'");
}

TEST_CASE("a linear value consumed exactly once on every path is accepted and runs",
          "[experiments][linear]") {
    const auto output = run_program(prelude +
        "make: (id: int) -> Token = { t := Token(id); return t; }\n"
        "pass: (move t: Token) -> Token = { return t; }\n"
        "store: (inout v: std::vector<Token>, move t: Token) = { v.push_back(move t); }\n"
        "main: () = {\n"
        "    a := Token(1);\n"
        "    consume(move a);\n"
        "    b: Token = make(2);\n"
        "    consume(pass(move b));\n"
        "    v: std::vector<Token> = ();\n"
        "    c := make(3);\n"
        "    store(v, move c);\n"
        "    d := make(4);\n"
        "    if !v.empty() { consume(move d); } else { store(v, move d); }\n"
        "    e := make(5);\n"
        "    consume(e);\n"
        "    while !v.empty() { f := make(6); consume(f); v.pop_back(); }\n"
        "    g := make(7);\n"
        "    try { consume(move g); } catch (x: std::exception) { throw; }\n"
        "    early(v.empty());\n"
        "}\n"
        "early: (c: bool) = {\n"
        "    h := make(8);\n"
        "    if c { consume(move h); return; }\n"
        "    consume(move h);\n"
        "}\n",
        "linear-ok");
    CHECK(output == "consumed 1\nconsumed 2\nconsumed 4\nconsumed 5\nconsumed 6\n"
                    "consumed 7\nconsumed 8\n");
}

TEST_CASE("a linear value that some path does not consume is diagnosed at its declaration",
          "[experiments][linear]") {
    const auto result = translate(prelude +
        "f: (c: bool) = {\n"                                     // 8
        "    a := Token(1);\n"                                   // 9
        "    b: Token = Token(2);\n"                             // 10
        "    if c { consume(move b); }\n"                        // 11
        "    d := Token(3);\n"                                   // 12
        "    if c { return; }\n"                                 // 13
        "    consume(move d);\n"                                 // 14
        "    { e := Token(4); }\n"                               // 15
        "    while c { g := Token(5); if c { break; } consume(move g); }\n"  // 16
        "    h := Token(6);\n"                                   // 17
        "    if c { throw (std::runtime_error(\"x\")); }\n"      // 18
        "    consume(move h);\n"                                 // 19
        "    i := Token(7);\n"                                   // 20
        "    if c { consume(move i); throw (std::runtime_error(\"x\")); }\n"  // 21
        "}\n"
        "k: (move t: Token, in u: Token, inout w: Token) = { }\n"  // 23
        "m: (move t: Token) = { consume(move t); }\n"            // 24
        "n: (move t: Token) = { t.id++; }\n"                     // 25
        "p: (move t: Token) -> Token = { return t; }\n");        // 26
    const auto* never = diagnostic_at(result, "linear-not-consumed", 9);
    REQUIRE(never != nullptr);
    CHECK(never->message == "Token a is not consumed on every path");
    CHECK(never->severity == cppfrontier::diagnostic_severity::error);
    CHECK(has_diagnostic(result, "linear-not-consumed", 10));
    CHECK(has_diagnostic(result, "linear-not-consumed", 12));
    CHECK(has_diagnostic(result, "linear-not-consumed", 15));
    CHECK(has_diagnostic(result, "linear-not-consumed", 16));
    CHECK_FALSE(has_diagnostic(result, "linear-not-consumed", 17));
    CHECK(has_diagnostic(result, "linear-not-consumed", 20));
    CHECK(has_diagnostic(result, "linear-not-consumed", 23));
    CHECK_FALSE(has_diagnostic(result, "linear-not-consumed", 24));
    // Reading a part of a value does not consume it, even at its last use.
    CHECK(has_diagnostic(result, "linear-not-consumed", 25));
    CHECK_FALSE(has_diagnostic(result, "linear-not-consumed", 26));
    CHECK(count_of(result, "linear-not-consumed") == 8);
    CHECK(count_of(result, "use-after") == 0);
}

TEST_CASE("a second consumption or a use after one is diagnosed without use-after-move",
          "[experiments][linear]") {
    const auto result = translate(prelude +
        "f: (c: bool) = {\n"                                     // 8
        "    a := Token(1);\n"                                   // 9
        "    consume(move a);\n"                                 // 10
        "    consume(move a);\n"                                 // 11
        "    b := Token(2);\n"                                   // 12
        "    consume(move b);\n"                                 // 13
        "    std::cout << b.id;\n"                               // 14
        "    std::cout << b.id;\n"                               // 15
        "    d := Token(3);\n"                                   // 16
        "    if c { consume(move d); }\n"                        // 17
        "    consume(move d);\n"                                 // 18
        "    e := Token(4);\n"                                   // 19
        "    while c { consume(move e); }\n"                     // 20
        "    g := Token(5);\n"                                   // 21
        "    consume(move g);\n"                                 // 22
        "    g = Token(6);\n"                                    // 23
        "    consume(move g);\n"                                 // 24
        "}\n");
    const auto* twice = diagnostic_at(result, "linear-consumed-twice", 11);
    REQUIRE(twice != nullptr);
    CHECK(twice->message == "a was consumed on line 10 and is consumed again here");
    const auto* used = diagnostic_at(result, "use-after-move", 14);
    REQUIRE(used != nullptr);
    CHECK(used->message == "b was moved on line 13 and is used here");
    // A use after a consumption is reported once per consumption.
    CHECK(count_of(result, "linear-consumed-twice") == 2);
    CHECK_FALSE(has_diagnostic(result, "use-after-move", 15));
    const auto* maybe = diagnostic_at(result, "linear-consumed-twice", 18);
    REQUIRE(maybe != nullptr);
    CHECK(maybe->message == "d may have been consumed on line 17 and is consumed again here");
    CHECK(has_diagnostic(result, "move-in-loop", 20));
    // The loop may not run, so after it `e` may have been consumed.
    CHECK(has_diagnostic(result, "linear-not-consumed", 19));
    CHECK_FALSE(has_diagnostic(result, "linear-consumed-twice", 24));
    CHECK(count_of(result, "linear-not-consumed") == 1);
}

TEST_CASE("only values declared with a linear type of this translation unit are checked",
          "[experiments][linear]") {
    const auto result = translate(prelude +
        "Plain: type = { public id: int = 0; }\n"                // 8
        "f: () = {\n"                                            // 9
        "    p := Plain();\n"                                    // 10
        "    q: Plain = ();\n"                                   // 11
        "    n := 1;\n"                                          // 12
        "    v: std::vector<Token> = ();\n"                      // 13
        "    v.push_back(Token(1));\n"                           // 14
        "    _ = n;\n"                                           // 15
        "}\n");
    INFO(result.cpp1);
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    CHECK_FALSE(result.has_errors());
    CHECK(count_of(result, "linear") == 0);
}

TEST_CASE("@linear is unsupported and unchecked unless the experiment is enabled",
          "[experiments][linear]") {
    const auto result = translate(prelude +
        "f: () = {\n"                                           // 8
        "    a := Token(1);\n"                                  // 9
        "    consume(move a);\n"                                // 10
        "    consume(move a);\n"                                // 11
        "    b := Token(2);\n"                                  // 12
        "}\n", false);
    CHECK(has_diagnostic(result, "unsupported-metaclass", 3));
    CHECK(count_of(result, "linear") == 0);
    CHECK(count_of(result, "use-after") == 0);
}

TEST_CASE("a defer body consumes a linear value where a path leaves its block",
          "[experiments][linear]") {
    const auto result = translate(prelude +
        "f: (c: bool) = {\n"                                     // 8
        "    a := Token(1);\n"                                   // 9
        "    defer consume(move a);\n"                           // 10
        "    std::cout << a.id;\n"                               // 11
        "    if c { return; }\n"                                 // 12
        "    b := Token(2);\n"                                   // 13
        "    defer std::cout << b.id;\n"                         // 14
        "    consume(move b);\n"                                 // 15
        "}\n", true, {"defer"});
    CHECK(count_of(result, "linear") == 0);
    CHECK(has_diagnostic(result, "use-after-move", 14));
    CHECK(count_of(result, "use-after") == 1);
}

TEST_CASE("a linear value is consumed only by being passed on whole",
          "[experiments][linear]") {
    const auto result = translate(prelude +
        "open: (id: int) -> Token = { t := Token(id); return t; }\n"     // 8
        "look: (t: Token) = { std::cout << t.id; }\n"                    // 9
        "check: (move t: Token) -> bool = { t.release(); return true; }\n"  // 10
        "f: (v: std::vector<Token>) = {\n"                               // 11
        "    a := open(1);\n"                                            // 12
        "    b := open(2);\n"                                            // 13
        "    c := b;\n"                                                  // 14
        "    d := (move c);\n"                                           // 15
        "    consume(d);\n"                                              // 16
        "    e := open(3);\n"                                            // 17
        "    look(e);\n"                                                 // 18
        "    assert(check(move e));\n"                                   // 19
        "    g := open(4);\n"                                            // 20
        "    l := :() = g$.id;\n"                                        // 21
        "    h := open(5);\n"                                            // 22
        "    h = open(6);\n"                                             // 23
        "    h.release();\n"                                             // 24
        "    (copy q := open(7)) { }\n"                                  // 25
        "    for v do (copy t: Token) { }\n"                             // 26
        "}\n"
        "k: (inout t: Token) = { consume(move t); }\n"                   // 28
        "m: (inout t: Token) = { consume(move t); t = open(8); }\n"      // 29
        "n: (out t: Token) = { t = open(9); }\n");                       // 30
    for (const auto line : {12, 17, 25, 26}) {
        INFO(line);
        CHECK(has_diagnostic(result, "linear-not-consumed", line));
    }
    const auto* assigned = diagnostic_at(result, "linear-not-consumed", 23);
    REQUIRE(assigned != nullptr);
    CHECK(assigned->message == "Token h is assigned before it is consumed");
    const auto* borrowed = diagnostic_at(result, "linear-not-consumed", 28);
    REQUIRE(borrowed != nullptr);
    CHECK(borrowed->message == "Token t is moved from and not reassigned on every path");
    CHECK(count_of(result, "linear-not-consumed") == 6);
    CHECK(count_of(result, "linear-consumed-twice") == 0);
    CHECK(count_of(result, "use-after") == 0);
}

TEST_CASE("a loop nest is checked for linear values in time linear in its depth",
          "[experiments][linear][review]") {
    auto source = prelude + "f: (n: int) = {\n";
    for (auto depth = 0; depth < 40; ++depth) source += "while n > 0 {\n";
    source += "t := Token(1);\nconsume(move t);\n";
    for (auto depth = 0; depth < 40; ++depth) source += "}\n";
    source += "}\n";
    const auto start = std::chrono::steady_clock::now();
    const auto result = translate(source);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("a linear name is tracked by its known type", "[experiments][linear][review]") {
    const auto result = translate(prelude +
        "Pool: type = {\n"                                                        // 8
        "    public v: std::vector<Token> = ();\n"                                // 9
        "    give: (inout this, move t: Token) = { v.push_back(move t); }\n"      // 10
        "}\n"
        "release: (inout pool: std::vector<Token>, move t: Token) = { pool.push_back(move t); }\n"  // 12
        "f: () = {\n"                                                             // 13
        "    pool: std::vector<Token> = ();\n"                                    // 14
        "    a := Token(1);\n"                                                    // 15
        "    pool.release(a);\n"                                                  // 16
        "    p: Pool = ();\n"                                                     // 17
        "    b := Token(2);\n"                                                    // 18
        "    p.give(b);\n"                                                        // 19
        "    for pool do (copy t) { }\n"                                          // 20
        "    for pool do (move t) { consume(t); }\n"                              // 21
        "}\n"
        "g: (forward t: Token) = { std::cout << t.id; }\n"                        // 23
        "h: (forward t: Token) = { consume(move t); }\n");                        // 24
    CHECK(has_diagnostic(result, "linear-not-consumed", 20));
    CHECK(has_diagnostic(result, "linear-not-consumed", 23));
    CHECK(count_of(result, "linear") == 2);
}

TEST_CASE("a linear value may be consumed in each inspect alternative",
          "[experiments][linear][review]") {
    const auto result = translate(prelude +
        "take: (move t: Token) -> int = { t.release(); return 1; }\n"             // 8
        "f: (n: int) -> int = {\n"                                                // 9
        "    t := Token(1);\n"                                                    // 10
        "    return inspect n -> int { is 1 = take(move t); is _ = take(move t); };\n"  // 11
        "}\n");
    CHECK(count_of(result, "linear") == 0);
}

TEST_CASE("the early return of a hoisted try operator must consume linear values",
          "[experiments][linear][try-operator][review]") {
    const auto result = translate(prelude +
        "#include <optional>\n"                                                   // 8
        "f: (o: std::optional<int>) -> std::optional<int> = {\n"                  // 9
        "    t: Token = 1;\n"                                                     // 10
        "    x := o?;\n"                                                          // 11
        "    t.release();\n"                                                      // 12
        "    return x;\n"                                                         // 13
        "}\n"
        "g: (o: std::optional<int>) -> std::optional<int> = {\n"                  // 15
        "    t: Token = 1;\n"                                                     // 16
        "    t.release();\n"                                                      // 17
        "    x := o?;\n"                                                          // 18
        "    return x;\n"                                                         // 19
        "}\n", true, {"try-operator"});
    const auto* early = diagnostic_at(result, "linear-not-consumed", 10);
    REQUIRE(early != nullptr);
    CHECK(early->message == "Token t is not consumed on every path");
    CHECK(count_of(result, "linear") == 1);
}
