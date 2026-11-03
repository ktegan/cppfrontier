#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

auto translate(const std::string& source, bool enabled = true) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (enabled) REQUIRE(options.experiments.enable("interface-defaults"));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) { return issue.code == code && issue.span.begin.line == line; });
}

// Compiles `cpp1` to `name` in the scratch directory; the exit status of g++.
auto compile(const std::string& cpp1, std::string_view name, bool link = true) -> int {
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-interface-defaults");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (std::string{name} + ".cpp");
    const auto executable_path = scratch / std::string{name};
    test_support::write_file(cpp1_path, cpp1);
    auto arguments = std::vector<std::string>{std::string{CPPFRONTIER_GXX}, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE};
    if (link) arguments.insert(arguments.end(), {"-o", executable_path.string()});
    else arguments.emplace_back("-fsyntax-only");
    arguments.push_back(cpp1_path.string());
    const auto compiled = test_support::run_process(arguments, scratch, std::chrono::seconds{120});
    INFO("compiler output: " << compiled.output);
    REQUIRE_FALSE(compiled.timed_out);
    return compiled.exit_status;
}

const auto greeter = std::string{
    "#include <iostream>\n"
    "#include <string>\n"
    "Greeter: @interface type = {\n"
    "    name: (this) -> std::string;\n"
    "    greeting: (this) -> std::string = \"hello\";\n"
    "    greet: (this) -> std::string = {\n"
    "        return greeting() + \", \" + name();\n"
    "    }\n"
    "}\n"};

}

TEST_CASE("an interface function with a body is virtual with a default",
          "[experiments][interface-defaults]") {
    const auto result = translate(greeter);
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto& cpp1 = result.cpp1;
    CHECK(cpp1.find("virtual auto name() const -> std::string = 0;") != std::string::npos);
    CHECK(cpp1.find("virtual auto greeting() const -> std::string;") != std::string::npos);
    CHECK(cpp1.find("virtual auto greet() const -> std::string;") != std::string::npos);
    CHECK(cpp1.find("auto Greeter::greeting() const -> std::string { return \"hello\"; }") !=
          std::string::npos);
    CHECK(cpp1.find("auto Greeter::greet() const -> std::string  {\n") != std::string::npos);
    CHECK(cpp1.find("return greeting() + \", \" + name();") != std::string::npos);
    // One function is still pure, so the default constructor stays public.
    CHECK(cpp1.find("\nGreeter() = default;") != std::string::npos);
    CHECK(cpp1.find("protected: Greeter() = default;") == std::string::npos);
    CHECK(cpp1.find("protected: Greeter([[maybe_unused]] Greeter const& that)") != std::string::npos);
    CHECK(cpp1.find("virtual ~Greeter() noexcept { }") != std::string::npos);
}

TEST_CASE("a derived type overrides one function and inherits another's default",
          "[experiments][interface-defaults]") {
    const auto translation = translate(greeter +
        "Polite: type = {\n"
        "    this: Greeter = ();\n"
        "    name: (override this) -> std::string = \"world\";\n"
        "}\n"
        "Formal: type = {\n"
        "    this: Greeter = ();\n"
        "    name: (override this) -> std::string = \"sir\";\n"
        "    greeting: (override this) -> std::string = \"good day\";\n"
        "}\n"
        "say: (g: Greeter) = std::cout << g.greet() << \"\\n\";\n"
        "main: () = {\n"
        "    p: Polite = ();\n"
        "    f: Formal = ();\n"
        "    say(p);\n"
        "    say(f);\n"
        "}\n");
    for (const auto& issue : translation.diagnostics) INFO(issue.message);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    if (std::string{CPPFRONTIER_GXX}.empty()) SKIP("g++ is unavailable");
    REQUIRE(compile(translation.cpp1, "greeter-run") == 0);
    const auto scratch = test_support::scratch_directory("cppfrontier-experiment-interface-defaults");
    const auto ran = test_support::run_process({(scratch / "greeter-run").string()}, scratch,
                                               std::chrono::seconds{10});
    REQUIRE_FALSE(ran.timed_out);
    CHECK(ran.exit_status == 0);
    CHECK(ran.output == "hello, world\ngood day, sir\n");
}

TEST_CASE("an interface whose functions all have defaults gets a protected constructor",
          "[experiments][interface-defaults]") {
    const auto source = std::string{
        "#include <string>\n"
        "Tagged: @interface type = {\n"
        "    tag: (this) -> std::string = \"tag\";\n"
        "    label: (this) -> std::string = \"[\" + tag() + \"]\";\n"
        "}\n"
        "Item: type = {\n"
        "    this: Tagged = ();\n"
        "}\n"};
    const auto result = translate(source + "main: () -> int = { i: Item = (); return i.label().size() - 5; }\n");
    for (const auto& issue : result.diagnostics) INFO(issue.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("= 0;") == std::string::npos);
    CHECK(result.cpp1.find("protected: Tagged() = default;") != std::string::npos);
    if (std::string{CPPFRONTIER_GXX}.empty()) SKIP("g++ is unavailable");
    CHECK(compile(result.cpp1, "tagged-derived", false) == 0);

    // The interface itself cannot be created, as an abstract class cannot.
    const auto direct = translate(source + "main: () = { t: Tagged = (); }\n");
    REQUIRE_FALSE(direct.has_errors());
    CHECK(compile(direct.cpp1, "tagged-direct", false) != 0);
}

TEST_CASE("interface functions still need a this parameter and public access",
          "[experiments][interface-defaults]") {
    const auto result = translate(
        "I: @interface type = {\n"
        "    f: () -> int = 1;\n"
        "    private g: (this) -> int = 2;\n"
        "    x: int = 0;\n"
        "}\n");
    CHECK(has_diagnostic(result, "interface-function-this", 2));
    CHECK(has_diagnostic(result, "interface-function-access", 3));
    CHECK(has_diagnostic(result, "interface-data-member", 4));
    CHECK_FALSE(has_diagnostic(result, "interface-function-body", 2));
    CHECK_FALSE(has_diagnostic(result, "interface-function-body", 3));
}

TEST_CASE("interface function bodies are rejected unless the experiment is enabled",
          "[experiments][interface-defaults]") {
    const auto rejected = translate(greeter, false);
    CHECK(has_diagnostic(rejected, "interface-function-body", 5));
    CHECK(has_diagnostic(rejected, "interface-function-body", 6));
    CHECK_FALSE(has_diagnostic(rejected, "interface-function-body", 4));
    const auto body = std::find_if(rejected.diagnostics.begin(), rejected.diagnostics.end(),
        [](const auto& issue) { return issue.code == "interface-function-body"; });
    REQUIRE(body != rejected.diagnostics.end());
    CHECK(body->message ==
          "interface functions must not have a function body; remove the '=' initializer");

    const auto pure = std::string{
        "Shape: @interface type = {\n"
        "    area: (this) -> double;\n"
        "    perimeter: (this) -> double;\n"
        "}\n"};
    CHECK(translate(pure, false).cpp1 == translate(pure).cpp1);
    CHECK(translate(pure).cpp1.find("\nShape() = default;") != std::string::npos);
}

TEST_CASE("an interface constructor with a body is still rejected",
          "[experiments][interface-defaults]") {
    // A constructor is not virtual, and the generated default constructor would repeat it.
    const auto result = translate(
        "I: @interface type = {\n"
        "    operator=: (out this) = { }\n"
        "    f: (this) -> int = 1;\n"
        "}\n");
    CHECK(has_diagnostic(result, "interface-function-body", 2));
    CHECK_FALSE(has_diagnostic(result, "interface-function-body", 3));
}
