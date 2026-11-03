#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

auto translate(const std::string& source, bool enabled = true) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("unsafe"));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) { return issue.code == code && issue.span.begin.line == line; });
}

// The text of `cpp1` between the `unsafe` block's opening comment and its end.
auto unsafe_part(const std::string& cpp1) -> std::string {
    const auto begin = cpp1.find("{ // unsafe\n");
    REQUIRE(begin != std::string::npos);
    const auto end = cpp1.find("\n    }\n", begin);
    REQUIRE(end != std::string::npos);
    return cpp1.substr(begin, end - begin);
}

}

TEST_CASE("an unsafe block omits the runtime checks of its statements", "[experiments][unsafe]") {
    const auto result = translate(
        "#include <vector>\n"
        "f: (v: std::vector<int>, i: int, p: *int) -> int = {\n"
        "    a := v[i] + p* + i / 2 + (i < 3) as int;\n"
        "    unsafe {\n"
        "        b := v[i] + p* + i / 2 + (i < 3) as int;\n"
        "        return a + b;\n"
        "    }\n"
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    const auto checked = cpp1.substr(0, cpp1.find("{ // unsafe"));
    CHECK(checked.find("CPP2_ASSERT_IN_BOUNDS(v, i)") != std::string::npos);
    CHECK(checked.find("cpp2::impl::assert_not_null(p)") != std::string::npos);
    CHECK(checked.find("CPP2_ASSERT_NOT_ZERO_LITERAL(CPP2_TYPEOF(i),2)") != std::string::npos);
    CHECK(checked.find("cpp2::impl::cmp_less") != std::string::npos);
    const auto unchecked = unsafe_part(cpp1);
    CHECK(unchecked.find("v[i] + *p + i / 2") != std::string::npos);
    CHECK(unchecked.find("i < 3") != std::string::npos);
    CHECK(unchecked.find("ASSERT") == std::string::npos);
    CHECK(unchecked.find("assert_not_null") == std::string::npos);
    CHECK(unchecked.find("cmp_") == std::string::npos);
    CHECK(unchecked.find("return cpp2::move(a) + cpp2::move(b);") != std::string::npos);
}

TEST_CASE("an unsafe block turns off what the -no-*-checks flags do, for everything inside it",
          "[experiments][unsafe]") {
    const auto source = std::string{
        "#include <vector>\n"
        "f: (v: std::vector<int>, i: int, n: std::size_t) -> bool = {\n"
        "    unsafe {\n"
        "        l := :(w: std::vector<int>, j: int, m: std::size_t) -> bool = "
        "w[j] / j > 0 && j < m;\n"
        "        return i < n && l(v, i, n);\n"
        "    }\n"
        "}\n"};
    const auto result = translate(source);
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    INFO(result.cpp1);
    CHECK(result.cpp1.find("return w[j] / j > 0 && j < m; }") != std::string::npos);
    CHECK(result.cpp1.find("return i < n && cpp2::move(l)(v, i, n);") != std::string::npos);
    CHECK(result.cpp1.find("ASSERT") == std::string::npos);
    // Outside the block, both comparisons are mixed-sign errors.
    auto outside = source;
    outside.replace(outside.find("    unsafe {\n"), 13, "    {\n");
    const auto checked = translate(outside);
    CHECK(has_diagnostic(checked, "mixed-sign-comparison", 4));
    CHECK(has_diagnostic(checked, "mixed-sign-comparison", 5));
}

TEST_CASE("nested unsafe blocks and checks after the block", "[experiments][unsafe]") {
    const auto result = translate(
        "g: (i: int) -> int = {\n"
        "    unsafe { unsafe { return i / 2; } }\n"
        "    return i / 3;\n"
        "}\n");
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("{ // unsafe\n#line 2\n        { // unsafe\n#line 2\n"
                           "            return i / 2;") !=
          std::string::npos);
    CHECK(result.cpp1.find("return i / CPP2_ASSERT_NOT_ZERO_LITERAL(CPP2_TYPEOF(i),3);") !=
          std::string::npos);
}

TEST_CASE("an unsafe block compiles and runs", "[experiments][unsafe]") {
    const auto translation = translate(
        "#include <iostream>\n"
        "#include <vector>\n"
        "main: () = {\n"
        "    v: std::vector<int> = (10, 20, 30);\n"
        "    i := 2;\n"
        "    x := 0;\n"
        "    p := x&;\n"
        "    unsafe {\n"
        "        p* = v[i] / 3;\n"
        "    }\n"
        "    std::cout << x << \" \" << v[0] / 5 << \"\\n\";\n"
        "}\n");
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-unsafe");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "unsafe-run.cpp";
    const auto executable_path = scratch / "unsafe-run";
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
    CHECK(ran.output == "10 2\n");
}

TEST_CASE("an unsafe block needs an enclosing function body", "[experiments][unsafe]") {
    const auto result = translate(
        "T: type = { unsafe { f(); } }\n"
        "k: () = unsafe { m(); };\n"
        "N: namespace = { unsafe { g(); } }\n");
    CHECK(has_diagnostic(result, "unsafe-scope", 1));
    CHECK(has_diagnostic(result, "unsafe-scope", 2));
    CHECK(has_diagnostic(result, "unsafe-scope", 3));
    CHECK(result.diagnostics.front().message == "an unsafe block must be inside a function body");
    // At file scope, among Cpp1 lines, too.
    const auto file_scope = translate(
        "int unsafe_count = 0;\n"
        "unsafe { }\n"
        "unsafe\n"
        "{ }\n"
        "main: () = { }\n");
    CHECK(has_diagnostic(file_scope, "unsafe-scope", 2));
    CHECK(has_diagnostic(file_scope, "unsafe-scope", 3));
    CHECK(file_scope.diagnostics.size() == 2);
}

TEST_CASE("unsafe is an ordinary name unless the experiment is enabled", "[experiments][unsafe]") {
    const auto named = std::string{"main: () = {\n    unsafe := 1;\n    unsafe++;\n}\n"};
    CHECK(translate(named, false).cpp1 == translate(named).cpp1);
    CHECK(translate(named).cpp1.find("auto unsafe {1};") != std::string::npos);

    const auto block = std::string{"main: () = {\n    unsafe { f(); }\n}\n"};
    CHECK(translate(block, false).has_errors());
    CHECK_FALSE(translate(block).has_errors());

    const auto plain = std::string{"f: (i: int) -> int = {\n    return i / 2;\n}\n"};
    CHECK(translate(plain, false).cpp1 == translate(plain).cpp1);
}

TEST_CASE("an unsafe block in one function leaves the checks of the others on",
          "[experiments][unsafe][review]") {
    const auto result = translate(
        "#include <vector>\n"
        "f: () = {\n"
        "    h := :(v: std::vector<int>, n: int) -> int = v[n];\n"
        "    _ = h(:std::vector<int> = (1, 2), 1);\n"
        "}\n"
        "g: (i: int, u: u32) -> bool = { return i < u; }\n"
        "k: () = {\n"
        "    unsafe { x := 1; }\n"
        "}\n");
    CHECK(has_diagnostic(result, "mixed-sign-comparison", 6));
    const auto ok = translate(
        "#include <vector>\n"
        "f: () = {\n"
        "    h := :(v: std::vector<int>, n: int) -> int = v[n];\n"
        "    _ = h(:std::vector<int> = (1, 2), 1);\n"
        "}\n"
        "k: () = {\n"
        "    unsafe { u := :(v: std::vector<int>, n: int) -> int = v[n]; _ = u; }\n"
        "}\n");
    REQUIRE_FALSE(ok.has_errors());
    CHECK(ok.cpp1.find("return CPP2_ASSERT_IN_BOUNDS(v, n);") != std::string::npos);
    CHECK(ok.cpp1.find("return v[n];") != std::string::npos);
}
