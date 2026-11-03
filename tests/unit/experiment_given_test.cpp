#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.hpp"

namespace {

auto translate(const std::string& source, std::string_view experiment = "given")
    -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (!experiment.empty()) REQUIRE(options.experiments.enable(experiment));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) { return issue.code == code && issue.span.begin.line == line; });
}

// Compiles `cpp1` with g++ and runs it; the output is the compiler's when it
// did not compile.
auto compile_and_run(const std::string& cpp1, std::string_view name) -> test_support::process_result {
    const auto scratch = test_support::scratch_directory("cppfrontier-given");
    std::filesystem::create_directories(scratch);
    const auto source_path = scratch / (std::string{name} + ".cpp");
    const auto executable_path = scratch / name;
    test_support::write_file(source_path, cpp1);
    const auto compiled = test_support::run_process(
        {CPPFRONTIER_GXX, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-o",
         executable_path.string(), source_path.string()}, scratch, std::chrono::seconds{90});
    if (compiled.timed_out || compiled.exit_status != 0) return compiled;
    return test_support::run_process({executable_path.string()}, scratch,
                                     std::chrono::seconds{10});
}

const auto show_source = std::string{
    "#include <iostream>\n"
    "Show: @trait type = {\n"
    "    show: (this) -> std::string;\n"
    "    bump: (inout this, k: int);\n"
    "}\n"
    "Show_int: @given<Show, int> type = {\n"
    "    show: (x: int) -> std::string = \"int \" + std::to_string(x);\n"
    "    bump: (inout x: int, k: int) = { x += k; }\n"
    "}\n"
    "Circle: @impl<Show> @value type = {\n"
    "    r: int = 1;\n"
    "    show: (this) -> std::string = \"circle \" + std::to_string(r);\n"
    "    bump: (inout this, k: int) = { r += k; }\n"
    "}\n"
    "Int_is_Show: @impl<Show, int> type = { }\n"
    "print: <T: Show> (copy x: T) = { x.bump(1); std::cout << x.show() << \"\\n\"; }\n"
    "main: () = {\n"
    "    n := 42;\n"
    "    std::cout << n.show() << \" \" << Circle().show() << \"\\n\";\n"
    "    print(n);\n"
    "    print(Circle());\n"
    "    items: std::vector<dyn_Show> = (7, Circle());\n"
    "    for items do (inout s) { s.bump(10); }\n"
    "    for items do (s) { std::cout << s.show() << \"\\n\"; }\n"
    "}\n"};

}

TEST_CASE("@given moves its functions out as free functions", "[experiments][given]") {
    const auto result = translate(show_source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    CHECK(cpp1.find("struct Show_int {\n};") != std::string::npos);
    CHECK(cpp1.find("[[nodiscard]] auto show(cpp2::impl::in<int> x) -> std::string;\n"
                    "#line 8 \"input.cpp2\"\n"
                    "auto bump(int& x, cpp2::impl::in<int> k) -> void;\n") != std::string::npos);
    // The given has no generated value set.
    CHECK(cpp1.find("Show_int(Show_int const&") == std::string::npos);
}

TEST_CASE("a trait admits a given through UFCS", "[experiments][given]") {
    const auto result = translate(show_source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    CHECK(cpp1.find("{ CPP2_UFCS_NONLOCAL(show)(__cpp2_self) } -> std::convertible_to<std::string>;")
          != std::string::npos);
    // `dyn_Show` calls through `Show_calls`, defined after the given's functions
    // are declared.
    const auto declared = cpp1.find("auto bump(int& x, cpp2::impl::in<int> k) -> void;");
    const auto calls = cpp1.find("template<typename __cpp2_T> struct Show_calls {");
    REQUIRE(declared != std::string::npos);
    REQUIRE(calls != std::string::npos);
    CHECK(declared < calls);
    CHECK(cpp1.find("{ return Show_calls<T>::__cpp2_show(__cpp2_value); }") != std::string::npos);
    CHECK(cpp1.find("static_assert(::Show<int>, \"int does not satisfy Show\");") != std::string::npos);
    CHECK(cpp1.find("Show_instance") == std::string::npos);
}

TEST_CASE("@given diagnoses malformed instances", "[experiments][given]") {
    const auto result = translate(
        "Show: @trait type = { show: (this) -> std::string; }\n"  // 1
        "A: @given<Show> type = { }\n"                            // 2
        "B: @given<Show, int> <T> type = { }\n"                   // 3
        "C: @given<Show, int> type = {\n"                         // 4
        "    show: (this) -> std::string = \"\";\n"               // 5
        "    x: int = 0;\n"                                       // 6
        "    other: (y: double) -> int = 0;\n"                    // 7
        "}\n"                                                     // 8
        "D: @given<Show, int> type = { }\n"                       // 9
        "E: @given<int, int> type = { }\n"                        // 10
        "n: namespace = {\n"                                      // 11
        "    F: @given<Show, int> type = {\n"                     // 12
        "        show: (x: int) -> std::string = \"\";\n"          // 13
        "    }\n"                                                 // 14
        "}\n");                                                   // 15
    CHECK(has_diagnostic(result, "given-arguments", 2));
    CHECK(has_diagnostic(result, "given-template-type", 3));
    CHECK(has_diagnostic(result, "given-this", 5));
    CHECK(has_diagnostic(result, "given-member-kind", 6));
    CHECK(has_diagnostic(result, "given-instance-parameter", 7));
    CHECK(has_diagnostic(result, "given-missing-function", 9));
    CHECK(has_diagnostic(result, "given-not-trait", 10));
    CHECK(has_diagnostic(result, "given-scope", 12));
    const auto missing = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) { return issue.code == "given-missing-function"; });
    REQUIRE(missing != result.diagnostics.end());
    CHECK(missing->message == "the given of 'Show' for 'int' does not define 'show'");
}

TEST_CASE("a given for int and a member for a type both satisfy the trait at run time",
          "[experiments][given][runtime]") {
    const auto result = translate(show_source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    if (std::string{CPPFRONTIER_GXX}.empty()) SKIP("g++ is unavailable");
    const auto run = compile_and_run(result.cpp1, "given");
    INFO(run.output);
    REQUIRE_FALSE(run.timed_out);
    REQUIRE(run.exit_status == 0);
    CHECK(run.output == "int 42 circle 1\nint 43\ncircle 2\nint 17\ncircle 11\n");
}

TEST_CASE("@given needs its experiment, which enables trait", "[experiments][given]") {
    auto experiments = cppfrontier::experiment_set{};
    REQUIRE(experiments.enable("given"));
    CHECK(experiments.enabled("trait"));

    const auto off = translate(show_source, "");
    CHECK(has_diagnostic(off, "unsupported-metaclass", 2));
    CHECK(has_diagnostic(off, "unsupported-metaclass", 6));
    const auto trait_only = translate(show_source, "trait");
    CHECK_FALSE(has_diagnostic(trait_only, "unsupported-metaclass", 2));
    CHECK(has_diagnostic(trait_only, "unsupported-metaclass", 6));
    CHECK_FALSE(translate(show_source).has_errors());
}
