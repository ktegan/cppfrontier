#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

auto translate(const std::string& source, bool enabled = true) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("defer"));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) { return issue.code == code && issue.span.begin.line == line; });
}

auto run_program(const std::string& source, std::string_view name) -> std::string {
    const auto translation = translate(source);
    INFO(translation.cpp1);
    for (const auto& issue : translation.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(translation.has_errors());
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-defer");
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

}

TEST_CASE("defer lowers to a cpp2::finally scope guard", "[experiments][defer]") {
    const auto result = translate(
        "f: () = {\n"
        "    defer { cleanup(); }\n"
        "    defer close(1);\n"
        "    work();\n"
        "}\n");
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto __cpp2_defer_2_5 = cpp2::finally([&]{\n"
                           "#line 2\n"
                           "        cleanup();\n"
                           "    });\n") != std::string::npos);
    CHECK(result.cpp1.find("auto __cpp2_defer_3_5 = cpp2::finally([&]{\n"
                           "#line 3\n"
                           "        close(1);\n"
                           "    });\n") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::finally") < result.cpp1.find("work();"));
}

TEST_CASE("defer bodies run in reverse order and on exceptions", "[experiments][defer]") {
    const auto output = run_program(
        "#include <iostream>\n"
        "#include <stdexcept>\n"
        "f: () = {\n"
        "    defer { std::cout << \"first\\n\"; }\n"
        "    defer std::cout << \"second\\n\";\n"
        "    {\n"
        "        defer std::cout << \"inner\\n\";\n"
        "        std::cout << \"body\\n\";\n"
        "    }\n"
        "    std::cout << \"after inner\\n\";\n"
        "}\n"
        "g: () = {\n"
        "    defer std::cout << \"cleanup\\n\";\n"
        "    throw (std::runtime_error(\"boom\"));\n"
        "}\n"
        "main: () = {\n"
        "    f();\n"
        "    try { g(); } catch (e: std::exception) { std::cout << \"caught \" << e.what() << \"\\n\"; }\n"
        "}\n",
        "defer-order");
    CHECK(output == "body\ninner\nafter inner\nsecond\nfirst\ncleanup\ncaught boom\n");
}

TEST_CASE("a defer body cannot leave the deferred statement", "[experiments][defer]") {
    const auto result = translate(
        "f: () = {\n"
        "    defer { return; }\n"
        "    defer break;\n"
        "    defer { while true { continue; } }\n"
        "    defer { while true { continue missing; } }\n"
        "    outer: while true { defer { break outer; } break outer; }\n"
        "}\n");
    CHECK(has_diagnostic(result, "defer-control-flow", 2));
    CHECK(has_diagnostic(result, "defer-control-flow", 3));
    CHECK_FALSE(has_diagnostic(result, "defer-control-flow", 4));
    CHECK(has_diagnostic(result, "defer-control-flow", 5));
    CHECK(has_diagnostic(result, "defer-control-flow", 6));
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) { return issue.code == "defer-control-flow"; });
    REQUIRE(found != result.diagnostics.end());
    CHECK(found->message ==
          "a defer body cannot return, break or continue out of the deferred statement");
}

TEST_CASE("defer needs an enclosing function body", "[experiments][defer]") {
    const auto result = translate(
        "T: type = { defer h(); }\n"
        "k: () = defer m();\n"
        "N: namespace = { defer { f(); } }\n");
    CHECK(has_diagnostic(result, "defer-scope", 1));
    CHECK(has_diagnostic(result, "defer-scope", 2));
    CHECK(has_diagnostic(result, "defer-scope", 3));
    CHECK(result.diagnostics.front().message == "a defer statement must be inside a function body");
    // At file scope, among Cpp1 lines, too.
    const auto file_scope = translate(
        "#include <iostream>\n"
        "int deferred = 0;\n"
        "defer std::cout << 1;\n"
        "defer {\n"
        "}\n"
        "main: () = { }\n");
    CHECK(has_diagnostic(file_scope, "defer-scope", 3));
    CHECK(has_diagnostic(file_scope, "defer-scope", 4));
    CHECK(file_scope.diagnostics.size() == 2);
}

TEST_CASE("a name mentioned in a defer body is not moved in that block", "[experiments][defer]") {
    const auto result = translate(
        "#include <string>\n"
        "take: (copy s: std::string) = {}\n"
        "a: () = {\n"
        "    s: std::string = \"a\";\n"
        "    take(s);\n"
        "    defer take(s);\n"
        "}\n"
        "b: () = {\n"
        "    s: std::string = \"b\";\n"
        "    { defer take(s); take(s); }\n"
        "    take(s);\n"
        "}\n");
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("CPP2_FORWARD(s)") == result.cpp1.rfind("CPP2_FORWARD(s)"));
    CHECK(result.cpp1.find("CPP2_FORWARD(s)") != std::string::npos);
    CHECK(result.cpp1.find("take(CPP2_FORWARD(s));\n}") != std::string::npos);
}

TEST_CASE("a defer body is analyzed where a path leaves its block", "[experiments][defer]") {
    const auto result = translate(
        "#include <iostream>\n"                                  // 1
        "a: () = {\n"                                            // 2
        "    x: int;\n"                                          // 3
        "    defer std::cout << x;\n"                            // 4
        "    x = 1;\n"                                           // 5
        "}\n"
        "b: () = {\n"                                            // 7
        "    y: int;\n"                                          // 8
        "    defer y = 2;\n"                                     // 9
        "    std::cout << y;\n"                                  // 10
        "}\n");
    CHECK_FALSE(has_diagnostic(result, "use-before-initialization", 4));
    CHECK(has_diagnostic(result, "use-before-initialization", 10));
}

TEST_CASE("a defer body is checked only on normal exits", "[experiments][defer]") {
    // A throw before `x = 1`, from a call or a `throw`, would run the defer
    // with `x` uninitialized; an exit by an exception is not checked.
    const auto result = translate(
        "#include <iostream>\n"
        "may_throw: () = { }\n"
        "a: (b: bool) = {\n"
        "    x: int;\n"
        "    defer std::cout << x;\n"
        "    may_throw();\n"
        "    if b { throw(1); }\n"
        "    x = 1;\n"
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("defer is an ordinary name unless the experiment is enabled", "[experiments][defer]") {
    const auto source = std::string{
        "main: () = {\n"
        "    defer := 1;\n"
        "    defer++;\n"
        "}\n"};
    const auto off = translate(source, false);
    REQUIRE_FALSE(off.has_errors());
    CHECK(off.cpp1.find("auto defer {1};") != std::string::npos);
    const auto on = translate(source);
    CHECK(has_diagnostic(on, "defer-name", 2));

    const auto statement = std::string{"main: () = {\n    defer { f(); }\n}\n"};
    CHECK(translate(statement, false).has_errors());
    CHECK_FALSE(translate(statement).has_errors());

    const auto plain = std::string{"main: () = {\n    x := 1;\n    x++;\n}\n"};
    CHECK(translate(plain, false).cpp1 == translate(plain).cpp1);
}

TEST_CASE("a named return must be initialized before the defers at the end of the body",
          "[experiments][defer][review]") {
    const auto result = translate(
        "f: () -> (r: int) = { defer { r = 1; } return; }\n"   // 1
        "g: () -> (r: int) = { defer { r = 1; } }\n"           // 2
        "h: () -> (r: int) = { r = 2; defer { r = 1; } }\n");  // 3
    CHECK(has_diagnostic(result, "use-before-initialization", 1));
    CHECK(has_diagnostic(result, "use-before-initialization", 2));
    CHECK_FALSE(has_diagnostic(result, "use-before-initialization", 3));
}

TEST_CASE("defer followed by a semicolon expects a body", "[experiment][defer][review]") {
    const auto result = translate("f: () = {\n    defer;\n    x := 1;\n}\n");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(has_diagnostic(result, "expected-body", 2));
}

TEST_CASE("a defer sees the name a return copies, and the return still consumes it",
          "[experiments][defer][hunt]") {
    auto options = cppfrontier::translation_options{};
    REQUIRE(options.experiments.enable("defer"));
    REQUIRE(options.experiments.enable("use-after-move"));
    REQUIRE(options.experiments.enable("linear"));
    const auto result = cppfrontier::translator{}.translate_source(
        "#include <iostream>\n"                                         // 1
        "#include <string>\n"                                           // 2
        "Token: @linear type = { operator=: (out this) = { } }\n"       // 3
        "f: () -> std::string = {\n"                                    // 4
        "    s: std::string = \"x\";\n"                                 // 5
        "    defer std::cout << s;\n"                                   // 6
        "    return s;\n"                                               // 7
        "}\n"
        "g: () -> Token = {\n"                                          // 9
        "    t: Token = ();\n"                                          // 10
        "    defer std::cout << \"g\";\n"                               // 11
        "    return t;\n"                                               // 12
        "}\n"
        "h: (move s: std::string) -> std::string = {\n"                 // 14
        "    defer std::cout << s;\n"                                   // 15
        "    return (move s);\n"                                        // 16
        "}\n",
        options);
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    CHECK_FALSE(has_diagnostic(result, "use-after-move", 6));
    CHECK_FALSE(has_diagnostic(result, "linear-not-consumed", 10));
    CHECK(has_diagnostic(result, "use-after-move", 15));
    CHECK(result.diagnostics.size() == 1);
}
