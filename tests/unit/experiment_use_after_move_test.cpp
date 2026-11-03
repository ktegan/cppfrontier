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
    if (enabled) REQUIRE(options.experiments.enable("use-after-move"));
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
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-use-after-move");
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

const auto take = std::string{"#include <string>\ntake: (move s: std::string) = {}\n"};

}

TEST_CASE("a use after an explicit move is diagnosed with the moving line",
          "[experiments][use-after-move]") {
    const auto result = translate(take +
        "f: () = {\n"                      // 3
        "    s: std::string = \"a\";\n"    // 4
        "    take(move s);\n"              // 5
        "    take(s);\n"                   // 6
        "    s = \"b\";\n"                 // 7
        "    take(s);\n"                   // 8
        "    t := std::string(\"t\");\n"   // 9
        "    take(move t);\n"              // 10
        "    return;\n"                    // 11
        "}\n"
        "g: () -> std::string = {\n"       // 13
        "    s: std::string = \"a\";\n"    // 14
        "    take(move s);\n"              // 15
        "    return s;\n"                  // 16
        "}\n");
    const auto* first = diagnostic_at(result, "use-after-move", 6);
    REQUIRE(first != nullptr);
    CHECK(first->message == "s was moved on line 5 and is used here");
    CHECK(first->severity == cppfrontier::diagnostic_severity::error);
    CHECK_FALSE(has_diagnostic(result, "use-after-move", 8));
    CHECK(has_diagnostic(result, "use-after-move", 16));
    CHECK(count_of(result, "use-after") == 2);
}

TEST_CASE("typeid is a use after a move, sizeof is not", "[experiments][use-after-move]") {
    const auto result = translate(take +
        "f: () = {\n"                            // 3
        "    s: std::string = \"a\";\n"          // 4
        "    take(move s);\n"                    // 5
        "    n := sizeof(s);\n"                  // 6
        "    t := typeid(s).name();\n"           // 7
        "}\n");
    CHECK_FALSE(has_diagnostic(result, "use-after-move", 6));
    CHECK(has_diagnostic(result, "use-after-move", 7));
}

TEST_CASE("reassignment and out arguments end the moved state and the program runs",
          "[experiments][use-after-move]") {
    const auto output = run_program(
        "#include <iostream>\n"
        "#include <string>\n"
        "take: (move s: std::string) -> std::string = { return s; }\n"
        "fill: (out s: std::string) = { s = \"three\"; }\n"
        "main: () = {\n"
        "    s: std::string = \"one\";\n"
        "    t := take(move s);\n"
        "    s = \"two\";\n"
        "    std::cout << t << \" \" << s << \"\\n\";\n"
        "    take(move s);\n"
        "    fill(out s);\n"
        "    s = take(move s) + \"!\";\n"
        "    std::cout << s << \"\\n\";\n"
        "}\n",
        "use-after-move-ok");
    CHECK(output == "one two\nthree!\n");
}

TEST_CASE("a move on one branch leaves the name maybe moved", "[experiments][use-after-move]") {
    const auto result = translate(take +
        "f: (c: bool) = {\n"                                     // 3
        "    t: std::string = \"t\";\n"                          // 4
        "    if c { take(move t); }\n"                           // 5
        "    take(t);\n"                                         // 6
        "    u: std::string = \"u\";\n"                          // 7
        "    if c { take(move u); } else { take(move u); }\n"    // 8
        "    take(u);\n"                                         // 9
        "    v: std::string = \"v\";\n"                          // 10
        "    if c { take(move v); return; }\n"                   // 11
        "    take(v);\n"                                         // 12
        "    w: std::string = \"w\";\n"                          // 13
        "    if c { take(move w); } else { w = \"x\"; }\n"       // 14
        "    take(w);\n"                                         // 15
        "}\n");
    const auto* maybe = diagnostic_at(result, "use-after-maybe-move", 6);
    REQUIRE(maybe != nullptr);
    CHECK(maybe->message == "t may have been moved on line 5 and is used here");
    CHECK(maybe->severity == cppfrontier::diagnostic_severity::error);
    const auto* both = diagnostic_at(result, "use-after-move", 9);
    REQUIRE(both != nullptr);
    CHECK(both->message == "u was moved on line 8 and is used here");
    CHECK_FALSE(has_diagnostic(result, "use-after-move", 12));
    CHECK_FALSE(has_diagnostic(result, "use-after-maybe-move", 12));
    CHECK(has_diagnostic(result, "use-after-maybe-move", 15));
}

TEST_CASE("a move inside a loop is diagnosed unless the name is reassigned",
          "[experiments][use-after-move]") {
    const auto result = translate(take +
        "f: (c: bool) = {\n"                                           // 3
        "    v: std::string = \"v\";\n"                                // 4
        "    while c { take(move v); }\n"                              // 5
        "    w: std::string = \"w\";\n"                                // 6
        "    while c { take(move w); w = \"\"; }\n"                    // 7
        "    x: std::string = \"x\";\n"                                // 8
        "    while c { if c { take(move x); break; } }\n"              // 9
        "    take(x);\n"                                               // 10
        "    p: std::string = \"p\";\n"                                // 11
        "    for (1, 2) do (i) { take(move p); continue; }\n"          // 12
        "    d: std::string = \"d\";\n"                                // 13
        "    do { take(move d); } while c;\n"                          // 14
        "    e: std::string = \"e\";\n"                                // 15
        "    outer: while c { while c { take(move e); break outer; } }\n"  // 16
        "    take(e);\n"                                               // 17
        "    s: std::string = \"s\";\n"                                // 18
        "    while c { t := std::string(\"t\"); take(move t); }\n"     // 19
        "}\n");
    const auto* loop = diagnostic_at(result, "move-in-loop", 5);
    REQUIRE(loop != nullptr);
    CHECK(loop->message == "v is moved inside a loop and is used moved-from on the next iteration");
    CHECK(loop->span.begin.column == 15);
    CHECK_FALSE(has_diagnostic(result, "move-in-loop", 7));
    CHECK_FALSE(has_diagnostic(result, "move-in-loop", 9));
    CHECK(has_diagnostic(result, "use-after-maybe-move", 10));
    CHECK(has_diagnostic(result, "move-in-loop", 12));
    CHECK(has_diagnostic(result, "move-in-loop", 14));
    CHECK_FALSE(has_diagnostic(result, "move-in-loop", 16));
    CHECK(has_diagnostic(result, "use-after-maybe-move", 17));
    CHECK_FALSE(has_diagnostic(result, "move-in-loop", 19));
    CHECK(count_of(result, "move-in-loop") == 3);
}

TEST_CASE("scalars and in parameters are not tracked, other parameters are",
          "[experiments][use-after-move]") {
    const auto result = translate(take +
        "f: (move q: std::string, inout r: std::string, in k: std::string, "
        "copy p: *int) = {\n"                   // 3
        "    take(move q);\n"                   // 4
        "    take(q);\n"                        // 5
        "    take(move r);\n"                   // 6
        "    r = \"\";\n"                       // 7
        "    take(r);\n"                        // 8
        "    take(move k);\n"                   // 9
        "    take(k);\n"                        // 10
        "    n := 1;\n"                         // 11
        "    m: int = 2;\n"                     // 12
        "    _ = (move n) + (move m);\n"        // 13
        "    _ = n + m;\n"                      // 14
        "    _ = (move p);\n"                   // 15
        "    _ = p;\n"                          // 16
        "    y: std::string = \"y\";\n"         // 17
        "    take(move y, y);\n"                // 18
        "}\n");
    const auto* parameter = diagnostic_at(result, "use-after-move", 5);
    REQUIRE(parameter != nullptr);
    CHECK(parameter->message == "q was moved on line 4 and is used here");
    const auto* same = diagnostic_at(result, "use-after-move", 18);
    REQUIRE(same != nullptr);
    CHECK(same->message == "y is moved and used again in the same statement");
    CHECK(count_of(result, "use-after") == 2);
    CHECK(count_of(result, "move-in-loop") == 0);
}

TEST_CASE("the analysis changes nothing unless the experiment is enabled",
          "[experiments][use-after-move]") {
    const auto faulty = take +
        "f: () = {\n"
        "    s: std::string = \"a\";\n"
        "    take(move s);\n"
        "    take(s);\n"
        "}\n";
    const auto on = translate(faulty);
    CHECK(on.has_errors());
    CHECK(on.cpp1.empty());
    const auto off = translate(faulty, false);
    CHECK_FALSE(off.has_errors());
    CHECK(off.cpp1.find("take(CPP2_FORWARD(s));") != std::string::npos);

    const auto correct = take +
        "f: () = {\n"
        "    s: std::string = \"a\";\n"
        "    take(move s);\n"
        "    s = \"b\";\n"
        "    take(s);\n"
        "}\n";
    CHECK(translate(correct, false).cpp1 == translate(correct).cpp1);
}

TEST_CASE("a path that leaves does not reach the code after its statement",
          "[experiments][use-after-move]") {
    const auto result = translate(take +
        "g: () = {}\n"                                                  // 3
        "f: (c: bool) = {\n"                                            // 4
        "    s: std::string = \"s\";\n"                                 // 5
        "    try { g(); } catch (e: std::exception) { take(move s); return; }\n"  // 6
        "    take(s);\n"                                                // 7
        "    t: std::string = \"t\";\n"                                 // 8
        "    try { take(move t); } catch (e: std::exception) { }\n"    // 9
        "    take(t);\n"                                                // 10
        "}\n");
    CHECK_FALSE(has_diagnostic(result, "use-after-maybe-move", 7));
    CHECK(has_diagnostic(result, "use-after-maybe-move", 10));
    CHECK(count_of(result, "use-after") == 1);
}

TEST_CASE("a loop is analyzed again from the state of its next iteration",
          "[experiments][use-after-move]") {
    const auto result = translate(take +
        "make: () -> std::string = \"m\";\n"                            // 3
        "f: (c: bool) = {\n"                                            // 4
        "    s: std::string = \"s\";\n"                                 // 5
        "    while c { s = make(); take(move s); }\n"                   // 6
        "    t: std::string = \"t\";\n"                                 // 7
        "    take(move t);\n"                                           // 8
        "    do { t = make(); } while c;\n"                             // 9
        "    take(t);\n"                                                // 10
        "    u: std::string = \"u\";\n"                                 // 11
        "    while c { take(move u); }\n"                               // 12
        "    take(u);\n"                                                // 13
        "    take(u);\n"                                                // 14
        "    v: std::string = \"v\";\n"                                 // 15
        "    while c { take(v); if c { take(move v); continue; } }\n"   // 16
        "}\n");
    CHECK_FALSE(has_diagnostic(result, "move-in-loop", 6));
    CHECK_FALSE(has_diagnostic(result, "use-after-maybe-move", 10));
    CHECK(has_diagnostic(result, "move-in-loop", 12));
    // A move is reported once, at the first mention after it: the next iteration's.
    CHECK_FALSE(has_diagnostic(result, "use-after-maybe-move", 13));
    CHECK(has_diagnostic(result, "move-in-loop", 16));
    CHECK(count_of(result, "move-in-loop") == 2);
    CHECK(count_of(result, "use-after") == 0);
}

TEST_CASE("a moved-from name is reported once per move", "[experiments][use-after-move]") {
    const auto result = translate(take +
        "f: (c: bool) = {\n"                  // 3
        "    s: std::string = \"s\";\n"       // 4
        "    take(move s);\n"                 // 5
        "    take(s);\n"                      // 6
        "    take(s);\n"                      // 7
        "    s.clear();\n"                    // 8
        "    s = \"t\";\n"                    // 9
        "    take(move s);\n"                 // 10
        "    s.clear();\n"                    // 11
        "}\n");
    CHECK(has_diagnostic(result, "use-after-move", 6));
    CHECK(has_diagnostic(result, "use-after-move", 11));
    CHECK(count_of(result, "use-after") == 2);
}

TEST_CASE("a defer body runs where a path leaves its block", "[experiments][use-after-move]") {
    const auto result = translate(take +
        "#include <iostream>\n"                                         // 3
        "f: (c: bool) = {\n"                                            // 4
        "    s: std::string = \"s\";\n"                                 // 5
        "    defer take(move s);\n"                                     // 6
        "    std::cout << s;\n"                                         // 7
        "    if c { return; }\n"                                        // 8
        "    t: std::string = \"t\";\n"                                 // 9
        "    defer std::cout << t;\n"                                   // 10
        "    while c { u: std::string = \"u\"; defer std::cout << u; take(move u); break; }\n"  // 11
        "    take(move t);\n"                                           // 12
        "}\n", true, {"defer"});
    CHECK_FALSE(has_diagnostic(result, "use-after-move", 7));
    const auto* deferred = diagnostic_at(result, "use-after-move", 10);
    REQUIRE(deferred != nullptr);
    CHECK(deferred->message == "t was moved on line 12 and is used here");
    CHECK(has_diagnostic(result, "use-after-move", 11));
    CHECK(count_of(result, "use-after") == 2);
}

TEST_CASE("each move of a name in one statement is counted", "[experiments][use-after-move]") {
    const auto result = translate(take +
        "size: (move s: std::string) -> int = s.ssize();\n"                 // 3
        "f: () = {\n"                                                       // 4
        "    s: std::string = \"s\";\n"                                     // 5
        "    n := size(move s) + size(move s);\n"                           // 6
        "    t: std::string = \"t\";\n"                                     // 7
        "    (copy a := size(move t), copy b := size(move t)) { _ = a + b; }\n"  // 8
        "}\n");
    const auto* twice = diagnostic_at(result, "use-after-move", 6);
    REQUIRE(twice != nullptr);
    CHECK(twice->message == "s is moved and used again in the same statement");
    const auto* parameters = diagnostic_at(result, "use-after-move", 8);
    REQUIRE(parameters != nullptr);
    CHECK(parameters->message == "t was moved on line 8 and is used here");
    CHECK(count_of(result, "use-after") == 2);
}

TEST_CASE("every binding form is followed", "[experiments][use-after-move]") {
    const auto result = translate(take +
        "#include <vector>\n"                                               // 3
        "f: (v: std::vector<std::string>) = {\n"                            // 4
        "    (copy s: std::string = \"a\") { take(move s); take(s); }\n"    // 5
        "    for v do (copy s) { take(move s); take(s); }\n"                // 6
        "    (a, b) := :std::pair<std::string, std::string> = (\"x\", \"y\");\n"  // 7
        "    take(move a);\n"                                               // 8
        "    take(a);\n"                                                    // 9
        "    take(b);\n"                                                    // 10
        "    try { } catch (e: std::string) { take(move e); take(e); }\n"   // 11
        "}\n", true, {"destructuring"});
    CHECK(has_diagnostic(result, "use-after-move", 5));
    CHECK(has_diagnostic(result, "use-after-move", 6));
    CHECK(has_diagnostic(result, "use-after-move", 9));
    CHECK(count_of(result, "use-after") == 3);
}

TEST_CASE("a loop nest is analyzed in time linear in its depth",
          "[experiments][use-after-move][review]") {
    auto source = take + "f: (n: int) = {\n    s: std::string = \"a\";\n";
    for (auto depth = 0; depth < 40; ++depth) source += "while n > 0 {\n";
    source += "take(move s);\ns = \"b\";\n";
    for (auto depth = 0; depth < 40; ++depth) source += "}\n";
    source += "}\n";
    const auto start = std::chrono::steady_clock::now();
    const auto result = translate(source);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("only a name of a known type that is not a scalar is followed",
          "[experiments][use-after-move][review]") {
    const auto result = translate(take +
        "#include <tuple>\n"                                                  // 3
        "ti: (move v: int) = { }\n"                                           // 4
        "main: (args) = {\n"                                                  // 5
        "    n := args.ssize();\n"                                            // 6
        "    b := n + 1;\n"                                                   // 7
        "    ti(move b); _ = b;\n"                                            // 8
        "    (copy d := n) { ti(move d); _ = d; }\n"                          // 9
        "    (p, q) := :std::tuple<int, std::string> = (n, \"x\");\n"         // 10
        "    ti(move p); _ = p;\n"                                            // 11
        "    take(move q); _ = q;\n"                                          // 12
        "    s := :std::string = \"s\";\n"                                    // 13
        "    t := s;\n"                                                       // 14
        "    take(move t); _ = t;\n"                                          // 15
        "}\n", true, {"destructuring"});
    CHECK(has_diagnostic(result, "use-after-move", 12));
    CHECK(has_diagnostic(result, "use-after-move", 15));
    CHECK(count_of(result, "use-after") == 2);
}

TEST_CASE("inspect alternatives are exclusive paths", "[experiments][use-after-move][review]") {
    const auto result = translate(take +
        "f: (n: int) -> int = {\n"                                                   // 3
        "    s: std::string = \"a\";\n"                                              // 4
        "    return inspect n -> int { is 1 = take2(move s); is _ = s.ssize(); };\n" // 5
        "}\n"
        "g: (n: int) -> int = {\n"                                                   // 7
        "    s: std::string = \"a\";\n"                                              // 8
        "    _ = inspect n -> int { is 1 = take2(move s); is _ = 0; };\n"            // 9
        "    return s.ssize();\n"                                                    // 10
        "}\n"
        "take2: (move s: std::string) -> int = 0;\n");
    CHECK_FALSE(has_diagnostic(result, "use-after-move", 5));
    const auto* maybe = diagnostic_at(result, "use-after-maybe-move", 10);
    REQUIRE(maybe != nullptr);
    CHECK(maybe->message == "s may have been moved on line 9 and is used here");
    CHECK(count_of(result, "use-after") == 1);
}

TEST_CASE("a move in a loop is reported once, at its next iteration",
          "[experiments][use-after-move][review]") {
    const auto result = translate(take +
        "#include <iostream>\n"                                    // 3
        "f: (n: int) = {\n"                                        // 4
        "    s: std::string = \"a\";\n"                            // 5
        "    i := 0;\n"                                            // 6
        "    while i < n next std::cout << s {\n"                  // 7
        "        take(move s);\n"                                  // 8
        "        i++;\n"                                           // 9
        "    }\n"
        "}\n"
        "g: (v: std::vector<int>) = {\n"                           // 12
        "    s: std::string = \"a\";\n"                            // 13
        "    for v do (x) { if x > 0 { take(move s); } }\n"        // 14
        "    std::cout << s;\n"                                    // 15
        "}\n");
    CHECK(count_of(result, "use-after") + count_of(result, "move-in-loop") == 2);
    CHECK((has_diagnostic(result, "use-after-move", 7) ||
           has_diagnostic(result, "move-in-loop", 7)));
    CHECK(has_diagnostic(result, "move-in-loop", 14));
}

TEST_CASE("returning reads the named return values", "[experiments][use-after-move][review]") {
    const auto result = translate(take +
        "f: () -> (r: std::string) = {\n"           // 3
        "    r = \"a\";\n"                          // 4
        "    take(move r);\n"                       // 5
        "    return;\n"                             // 6
        "}\n"
        "g: () -> (r: std::string) = {\n"           // 8
        "    r = \"a\";\n"                          // 9
        "    take(move r);\n"                       // 10
        "}\n"                                       // 11
        "h: () -> (r: std::string) = {\n"           // 12
        "    r = \"a\";\n"                          // 13
        "    take(move r);\n"                       // 14
        "    r = \"b\";\n"                          // 15
        "}\n");
    const auto* bare = diagnostic_at(result, "use-after-move", 6);
    REQUIRE(bare != nullptr);
    CHECK(bare->message == "r was moved on line 5 and is used here");
    CHECK(has_diagnostic(result, "use-after-move", 11));
    CHECK(count_of(result, "use-after") == 2);
}

TEST_CASE("the early return of a hoisted try operator is a path",
          "[experiments][use-after-move][try-operator][review]") {
    const auto result = translate(take +
        "#include <iostream>\n"                                          // 3
        "#include <optional>\n"                                          // 4
        "f: (o: std::optional<int>) -> std::optional<int> = {\n"         // 5
        "    s: std::string = \"a\";\n"                                  // 6
        "    defer std::cout << s;\n"                                    // 7
        "    take(move s);\n"                                            // 8
        "    y := o?;\n"                                                 // 9
        "    s = \"b\";\n"                                               // 10
        "    return y;\n"                                                // 11
        "}\n"
        "g: (o: std::optional<int>) -> std::optional<int> = {\n"         // 13
        "    s: std::string = \"a\";\n"                                  // 14
        "    defer std::cout << s;\n"                                    // 15
        "    y := take2(move s, o?);\n"                                  // 16
        "    s = \"b\";\n"                                               // 17
        "    return y;\n"                                                // 18
        "}\n"
        "take2: (move s: std::string, v: int) -> int = v;\n",
        true, {"try-operator", "defer"});
    // The defer runs on the early return of `o?`, after the move.
    CHECK(has_diagnostic(result, "use-after-move", 7));
    // `o?` is evaluated before the call that moves `s`.
    CHECK_FALSE(has_diagnostic(result, "use-after-move", 15));
    CHECK(count_of(result, "use-after") == 1);
}

TEST_CASE("a handler starts from every state its try block may throw from",
          "[experiments][use-after-move][review]") {
    const auto result = translate(take +
        "#include <iostream>\n"                                    // 3
        "f: () = {\n"                                              // 4
        "    s: std::string = \"a\";\n"                            // 5
        "    try {\n"                                              // 6
        "        take(move s);\n"                                  // 7
        "        s = \"b\";\n"                                     // 8
        "    } catch (e: std::string) {\n"                         // 9
        "        std::cout << s;\n"                                // 10
        "    }\n"
        "}\n"
        "g: () = {\n"                                              // 13
        "    s: std::string = \"a\";\n"                            // 14
        "    try {\n"                                              // 15
        "        take(move s);\n"                                  // 16
        "    } catch (e: std::string) {\n"                         // 17
        "        s = \"c\";\n"                                     // 18
        "    }\n"
        "    std::cout << s;\n"                                    // 20
        "}\n");
    CHECK(has_diagnostic(result, "use-after-maybe-move", 10));
    CHECK(has_diagnostic(result, "use-after-maybe-move", 20));
    CHECK(count_of(result, "use-after") == 2);
}
