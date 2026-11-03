#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

auto translate(const std::string& source, bool enabled = true) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("lazy"));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) { return issue.code == code && issue.span.begin.line == line; });
}

auto message_of(const cppfrontier::translation_result& result, std::string_view code)
    -> std::string {
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) { return issue.code == code; });
    return found == result.diagnostics.end() ? std::string{} : found->message;
}

auto run_program(const std::string& source, std::string_view name) -> std::string {
    const auto translation = translate(source);
    INFO(translation.cpp1);
    for (const auto& issue : translation.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(translation.has_errors());
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-lazy");
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

TEST_CASE("a lazy namespace-scope object lowers to a cpp2::lazy holder", "[experiments][lazy]") {
    const auto result = translate(
        "#include <map>\n"
        "#include <string>\n"
        "load: () -> std::map<std::string, int> = { return (); }\n"
        "config: lazy std::map<std::string, int> = load();\n"
        "N: namespace = {\n"
        "    count: lazy int = 42;\n"
        "}\n"
        "main: () = {\n"
        "    config[\"x\"] = 1;\n"
        "    _ = config.size();\n"
        "    _ = N::count;\n"
        "}\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("#include \"cpp2lazy.h\"") != std::string::npos);
    CHECK(result.cpp1.find("extern cpp2::lazy<std::map<std::string, int>> config;") !=
          std::string::npos);
    CHECK(result.cpp1.find("cpp2::lazy<std::map<std::string, int>> config{[]{ return "
                           "std::map<std::string, int>{load()}; }};") != std::string::npos);
    CHECK(result.cpp1.find("extern cpp2::lazy<int> count;") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::lazy<int> count{[]{ return int{42}; }};") != std::string::npos);
    // Uses read the object through `.value()`.
    CHECK(result.cpp1.find("CPP2_ASSERT_IN_BOUNDS(config.value(), \"x\") = 1;") != std::string::npos);
    CHECK(result.cpp1.find("CPP2_UFCS(size)(config.value())") != std::string::npos);
    CHECK(result.cpp1.find("N::count.value()") != std::string::npos);
}

TEST_CASE("a lazy type-scope object is a static member defined after its type",
          "[experiments][lazy]") {
    const auto result = translate(
        "#include <string>\n"
        "T: type = {\n"
        "    public table: lazy std::string = \"tbl\";\n"
        "    show: () -> int = table.size();\n"
        "}\n"
        "main: () = { _ = T::table.size(); }\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("struct T {\nstatic cpp2::lazy<std::string> table;\n") !=
          std::string::npos);
    CHECK(result.cpp1.find("inline cpp2::lazy<std::string> T::table{[]{ return "
                           "std::string{\"tbl\"}; }};") != std::string::npos);
    CHECK(result.cpp1.find("CPP2_UFCS(size)(table.value())") != std::string::npos);
    CHECK(result.cpp1.find("CPP2_UFCS(size)(T::table.value())") != std::string::npos);
}

TEST_CASE("a local that hides a lazy object is not rewritten", "[experiments][lazy]") {
    const auto result = translate(
        "n: lazy int = 1;\n"
        "f: () -> int = { n := 2; return n; }\n"
        "g: () -> int = n;\n");
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("auto n {2};") != std::string::npos);
    CHECK(result.cpp1.find("return n;\n") != std::string::npos);
    CHECK(result.cpp1.find("return n.value(); }") != std::string::npos);
}

TEST_CASE("a lazy object is initialized on first use, once", "[experiments][lazy]") {
    const auto output = run_program(
        "#include <iostream>\n"
        "#include <string>\n"
        "#include <vector>\n"
        "load: () -> std::vector<std::string> = {\n"
        "    std::cout << \"loading\\n\";\n"
        "    return (\"a\", \"b\");\n"
        "}\n"
        "words: lazy std::vector<std::string> = load();\n"
        "T: type = {\n"
        "    public greeting: lazy std::string = make();\n"
        "    make: () -> std::string = { std::cout << \"greeting\\n\"; return \"hi\"; }\n"
        "}\n"
        "main: () = {\n"
        "    std::cout << \"start\\n\";\n"
        "    std::cout << words.size() << \"\\n\";\n"
        "    words.push_back(\"c\");\n"
        "    std::cout << words.size() << \" \" << words[2] << \"\\n\";\n"
        "    std::cout << T::greeting << \"\\n\";\n"
        "    std::cout << T::greeting.size() << \"\\n\";\n"
        "}\n",
        "lazy-once");
    CHECK(output == "start\nloading\n2\n3 c\ngreeting\nhi\n2\n");
}

TEST_CASE("a lazy object needs a declared type and an initializer", "[experiments][lazy]") {
    const auto result = translate(
        "a: lazy int;\n"
        "b: lazy int == 3;\n"
        "c: lazy _ = 3;\n"
        "T: type = {\n"
        "    d: lazy int;\n"
        "}\n");
    CHECK(has_diagnostic(result, "lazy-initializer", 1));
    CHECK(has_diagnostic(result, "lazy-initializer", 2));
    CHECK(has_diagnostic(result, "lazy-type", 3));
    CHECK(has_diagnostic(result, "lazy-initializer", 5));
    CHECK(message_of(result, "lazy-initializer") == "a lazy object must have an '=' initializer");
    CHECK(message_of(result, "lazy-type") == "a lazy object must have a declared type");
}

TEST_CASE("a lazy object initialized with a block is one error", "[experiments][lazy]") {
    const auto result = translate("x: lazy int = { return 1; }\n");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "object-initializer-not-expression");
}

TEST_CASE("a lazy object cannot be a local", "[experiments][lazy]") {
    const auto result = translate(
        "f: () = {\n"
        "    x: lazy int = 4;\n"
        "    y := 5;\n"
        "}\n");
    CHECK(has_diagnostic(result, "lazy-scope", 2));
    CHECK(message_of(result, "lazy-scope") ==
          "a lazy object must be declared at namespace or type scope");
    CHECK(result.has_errors());
    CHECK(std::all_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto& issue) { return issue.code == "lazy-scope"; }));
}

TEST_CASE("lazy is an ordinary type name unless the experiment is enabled", "[experiments][lazy]") {
    const auto source = std::string{
        "lazy: type = { public v: int = 1; }\n"
        "x: lazy = ();\n"
        "main: () = { y: lazy = (); _ = y.v; _ = x.v; }\n"};
    const auto off = translate(source, false);
    REQUIRE_FALSE(off.has_errors());
    CHECK(off.cpp1.find("lazy x {};") != std::string::npos);
    CHECK(off.cpp1.find("cpp2lazy.h") == std::string::npos);
    // `lazy` followed by anything but a type is still the type name.
    CHECK(translate(source).cpp1 == off.cpp1);

    const auto declaration = std::string{"n: lazy int = 1;\nmain: () = { _ = n; }\n"};
    CHECK(translate(declaration, false).cpp1.find("cpp2::lazy") == std::string::npos);
    CHECK(translate(declaration).cpp1.find("cpp2::lazy<int> n{[]{ return int{1}; }};") !=
          std::string::npos);
}
