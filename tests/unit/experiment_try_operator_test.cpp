#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

auto try_options() -> cppfrontier::translation_options {
    auto options = cppfrontier::translation_options{};
    REQUIRE(options.experiments.enable("try-operator"));
    return options;
}

auto translate(const std::string& source) -> cppfrontier::translation_result {
    return cppfrontier::translator{}.translate_source(source, try_options());
}

auto codes_of(const cppfrontier::translation_result& result) -> std::vector<std::string> {
    auto codes = std::vector<std::string>{};
    for (const auto& issue : result.diagnostics) codes.push_back(issue.code);
    return codes;
}

auto diagnostic_named(const cppfrontier::translation_result& result, std::string_view code)
    -> const cppfrontier::diagnostic* {
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& issue) { return issue.code == code; });
    return found == result.diagnostics.end() ? nullptr : &*found;
}

constexpr auto prelude =
    "#include <expected>\n"
    "#include <optional>\n"
    "#include <string>\n"
    "parse: (s: std::string) -> std::expected<int, std::string> = {\n"
    "    if s.empty() { return std::unexpected(\"empty\"); }\n"
    "    return cpp2::unchecked_narrow<int>(s.size());\n"
    "}\n"
    "half: (n: int) -> std::optional<int> = {\n"
    "    if n % 2 == 0 { return n / 2; }\n"
    "    return std::nullopt;\n"
    "}\n";

}

TEST_CASE("try operator hoists each operand before its statement", "[experiments][try-operator]") {
    const auto result = translate(std::string{prelude} +
        "sum: (a: std::string, b: std::string) -> std::expected<int, std::string> = {\n"
        "    x := parse(a)?;\n"
        "    return x + parse(b)?;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("#include \"cpp2try.h\"") != std::string::npos);
    CHECK(result.cpp1.find(
        "    auto&& __cpp2_try_0 = cpp2::try_hold(parse(a)); if (!__cpp2_try_0) "
        "return cpp2::try_failure(CPP2_FORWARD(__cpp2_try_0)); "
        "auto x {(*CPP2_FORWARD(__cpp2_try_0))};\n") != std::string::npos);
    CHECK(result.cpp1.find(
        "    auto&& __cpp2_try_1 = cpp2::try_hold(parse(b)); if (!__cpp2_try_1) "
        "return cpp2::try_failure(CPP2_FORWARD(__cpp2_try_1)); "
        "return cpp2::move(x) + (*CPP2_FORWARD(__cpp2_try_1));\n") != std::string::npos);
}

TEST_CASE("try operator includes its runtime when lowering hoists an operand",
          "[experiments][try-operator]") {
    const auto interpolated = translate(std::string{prelude} +
        "f: (n: int) -> std::optional<int> = {\n"
        "    s := \"half (half(n)?)$\";\n"
        "    return s.ssize();\n"
        "}\n");
    INFO(interpolated.cpp1);
    REQUIRE_FALSE(interpolated.has_errors());
    CHECK(interpolated.cpp1.find("#include \"cpp2util.h\"\n#include \"cpp2try.h\"\n") !=
          std::string::npos);
    const auto unused = translate(std::string{prelude} + "s: std::string = \"?\";\n");
    REQUIRE_FALSE(unused.has_errors());
    CHECK(unused.cpp1.find("cpp2try.h") == std::string::npos);
}

TEST_CASE("try operator keeps statements on their source lines", "[experiments][try-operator]") {
    const auto result = translate(std::string{prelude} +
        "f: (a: std::string) -> std::expected<int, std::string> = {\n"
        "    x := parse(a)?;\n"
        "    y := parse(a)?;\n"
        "    return x + y;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    // The prelude is 11 lines; `f` begins on line 12 and its statements are
    // on lines 13 to 15, each with its hoisted statements on its own line.
    // The definition follows the forward declaration.
    const auto directive = result.cpp1.rfind("#line 12 \"input.cpp2\"\n[[nodiscard]] auto f(");
    REQUIRE(directive != std::string::npos);
    const auto from_directive = result.cpp1.substr(directive);
    const auto line_of = [&](std::string_view text) {
        const auto at = from_directive.find(text);
        REQUIRE(at != std::string::npos);
        return 11 + std::count(from_directive.begin(),
                               from_directive.begin() + static_cast<std::ptrdiff_t>(at), '\n');
    };
    CHECK(line_of("auto&& __cpp2_try_0") == 13);
    CHECK(line_of("auto x {") == 13);
    CHECK(line_of("auto&& __cpp2_try_1") == 14);
    CHECK(line_of("return cpp2::move(x) + cpp2::move(y);") == 15);
}

TEST_CASE("try operator hoists in evaluation order, inner first, and once per operand",
          "[experiments][try-operator]") {
    const auto result = translate(std::string{prelude} +
        "f: (n: int) -> std::optional<int> = {\n"
        "    x := half(half(n)?)? * 2;\n"
        "    r := half(n);\n"
        "    return r?.value_or(0);\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find(
        "auto&& __cpp2_try_0 = cpp2::try_hold(half(n)); if (!__cpp2_try_0) "
        "return cpp2::try_failure(CPP2_FORWARD(__cpp2_try_0)); "
        "auto&& __cpp2_try_1 = cpp2::try_hold(half((*CPP2_FORWARD(__cpp2_try_0)))); "
        "if (!__cpp2_try_1) "
        "return cpp2::try_failure(CPP2_FORWARD(__cpp2_try_1)); "
        "auto x {(*CPP2_FORWARD(__cpp2_try_1)) * 2};") != std::string::npos);
    // A UFCS receiver is rendered more than once but hoisted once.
    CHECK(result.cpp1.find("__cpp2_try_3") == std::string::npos);
    CHECK(result.cpp1.find(
        "auto&& __cpp2_try_2 = cpp2::try_hold(cpp2::move(r)); if (!__cpp2_try_2) "
        "return cpp2::try_failure(CPP2_FORWARD(__cpp2_try_2)); "
        "return CPP2_UFCS(value_or)((*CPP2_FORWARD(__cpp2_try_2)), 0);") != std::string::npos);
}

TEST_CASE("try operator applies to expression bodies and function expressions",
          "[experiments][try-operator]") {
    const auto result = translate(std::string{prelude} +
        "f: (a: std::string) -> std::expected<int, std::string> = parse(a)? + 1;\n"
        "g: (n: int) -> int = {\n"
        "    h := :(m: int) -> std::optional<int> = half(m)?;\n"
        "    return h(n).value_or(-1);\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("{ auto&& __cpp2_try_0 = cpp2::try_hold(parse(a)); "
                           "if (!__cpp2_try_0) return cpp2::try_failure(CPP2_FORWARD(__cpp2_try_0)); "
                           "return (*CPP2_FORWARD(__cpp2_try_0)) + 1; }") != std::string::npos);
    CHECK(result.cpp1.find("return cpp2::try_failure(CPP2_FORWARD(__cpp2_try_1)); "
                           "return (*CPP2_FORWARD(__cpp2_try_1)); }") != std::string::npos);
}

TEST_CASE("try operator is rejected outside a statement that can return early",
          "[experiments][try-operator]") {
    const auto expect_context = [](const std::string& body, int line, std::string_view where,
                                   std::string_view also = {}) {
        auto options = try_options();
        if (!also.empty()) REQUIRE(options.experiments.enable(also));
        const auto result =
            cppfrontier::translator{}.translate_source(std::string{prelude} + body, options);
        INFO(body);
        const auto* issue = diagnostic_named(result, "try-operator-context");
        REQUIRE(issue != nullptr);
        CHECK(issue->span.begin.line == static_cast<std::uint32_t>(line));
        CHECK(issue->message.find(where) != std::string::npos);
    };
    expect_context("f: (n: int) -> std::optional<int> = {\n"
                   "    if half(n)? == 1 { return 1; }\n"
                   "    return 2;\n}\n", 13, "not in a condition");
    expect_context("f: (n: int) -> std::optional<int> = {\n"
                   "    while half(n)? == 1 { break; }\n"
                   "    return 2;\n}\n", 13, "not in a loop condition");
    expect_context("f: (n: int) -> std::optional<int> = {\n"
                   "    assert(half(n)? == 1);\n"
                   "    return 2;\n}\n", 13, "not in a contract");
    expect_context("f: (b: bool, n: int) -> std::optional<int> = {\n"
                   "    return inspect b -> int { is true = half(n)?; is _ = 0; };\n}\n",
                   13, "not in an inspect alternative");
    expect_context("f: (n: int) -> std::optional<int> = {\n"
                   "    return ? n;\n}\n", 13, "must follow the expression");
    expect_context("f: (n: int) -> std::optional<int> = {\n"
                   "    defer { m := half(n)?; }\n"
                   "    return 2;\n}\n", 13, "not in a defer body", "defer");
    expect_context("f: (n: int) -> std::optional<int> = {\n"
                   "    defer if n > 0 { _ = half(n)?; }\n"
                   "    return 2;\n}\n", 13, "not in a defer body", "defer");
    expect_context("f: (n: int) -> std::optional<int> pre(half(n)? == 1) = {\n"
                   "    return 2;\n}\n", 12, "not in a contract");
    expect_context("f: (n: int = half(2)?) -> std::optional<int> = n;\n", 12,
                   "not in a parameter default");
    expect_context("f: <N: int = half(2)?> () -> std::optional<int> = N;\n", 12,
                   "not in a parameter default");
    expect_context("f: <T> () -> std::optional<int> requires (half(2)? == 1) = 1;\n", 12,
                   "not in a requires clause");
    expect_context("f: () -> (x: int = half(2)?) = { }\n", 12, "not in a named return value");
    expect_context("x: int = half(2)?;\n", 12, "outside a function");
    expect_context("t: type = {\n    m: int = half(2)?;\n}\n", 13, "outside a function");
}

TEST_CASE("try operator is rejected where it would run once or in a lambda",
          "[experiments][try-operator][review]") {
    const auto expect_context = [](const std::string& body, std::string_view where,
                                   std::string_view also = {}) {
        auto options = try_options();
        if (!also.empty()) REQUIRE(options.experiments.enable(also));
        const auto result =
            cppfrontier::translator{}.translate_source(std::string{prelude} + body, options);
        INFO(body);
        const auto* issue = diagnostic_named(result, "try-operator-context");
        REQUIRE(issue != nullptr);
        CHECK(issue->span.begin.line == 13);
        CHECK(issue->message.find(where) != std::string::npos);
    };
    expect_context("f: (n: int) -> std::optional<int> = {\n"
                   "    static x := half(n)?;\n"
                   "    return x;\n}\n", "not in a static local's initializer");
    expect_context("f: (copy i: int) -> std::optional<int> = {\n"
                   "    while i < 10 next i = half(i)? { }\n"
                   "    return i;\n}\n", "not in a loop's next clause");
    expect_context("f: (v: std::vector<std::optional<int>>) -> std::optional<int> = {\n"
                   "    w := for v do (x) yield x? * 10;\n"
                   "    return 1;\n}\n", "not in a comprehension's guard or value",
                   "comprehension");
    expect_context("f: (v: std::vector<int>, o: std::optional<int>) -> std::optional<int> = {\n"
                   "    w := for v do (x) if x > o? yield x;\n"
                   "    return 1;\n}\n", "not in a comprehension's guard or value",
                   "comprehension");
    expect_context("f: (n: int) -> std::optional<int> = {\n"
                   "    (copy m := half(n)?) { return m; }\n"
                   "}\n", "not in a statement parameter");
    expect_context("f: () -> std::optional<int> = {\n"
                   "    T: type == decltype(half(2)?);\n"
                   "    return T();\n}\n", "not in a local declaration");

    // The range is evaluated before the comprehension, in the statement.
    auto options = try_options();
    REQUIRE(options.experiments.enable("comprehension"));
    const auto range = cppfrontier::translator{}.translate_source(std::string{prelude} +
        "f: (o: std::optional<std::vector<int>>) -> std::optional<int> = {\n"
        "    w := for o? do (x) yield x;\n"
        "    return 1;\n}\n", options);
    CHECK(diagnostic_named(range, "try-operator-context") == nullptr);
}

TEST_CASE("try operator diagnostics in a function expression point at its own `?`",
          "[experiments][try-operator][review]") {
    const auto result = translate(std::string{prelude} +
        "f: (n: int) -> std::optional<int> = {\n"
        "    g := :(m: int) -> int = half(m)? * 2;\n"
        "    return g(n);\n"
        "}\n"
        "h: (n: int) -> std::optional<int> = half(n)? + 1;\n");
    const auto* issue = diagnostic_named(result, "try-operator-return-type");
    REQUIRE(issue != nullptr);
    CHECK(issue->span.begin.line == 13);
    CHECK(issue->span.begin.column == 36);
}

TEST_CASE("try operator requires an expected or optional return type",
          "[experiments][try-operator]") {
    const auto expect_return_type = [](const std::string& body, std::string_view detail) {
        const auto result = translate(std::string{prelude} + body);
        INFO(body);
        const auto* issue = diagnostic_named(result, "try-operator-return-type");
        REQUIRE(issue != nullptr);
        CHECK(issue->span.begin.line == 13);
        CHECK(issue->span.begin.column == 17);
        CHECK(issue->message ==
              "`?` returns early from a function whose declared return type is std::expected "
              "or std::optional, " + std::string{detail});
    };
    expect_return_type("f: (n: int) -> int = {\n    x := half(n)?;\n    return x;\n}\n",
                       "not `int`");
    expect_return_type("f: (n: int) -> _ = {\n    x := half(n)?;\n    return x;\n}\n",
                       "not a deduced one");
    expect_return_type("f: (n: int) = {\n    x := half(n)?;\n}\n", "not a deduced one");
    expect_return_type("f: (n: int) -> (r: int) = {\n    x := half(n)?;\n    r = x;\n}\n",
                       "not `(...)`");
}

TEST_CASE("try operator is rejected on the right of a short-circuit operator",
          "[experiments][try-operator]") {
    const auto result = translate(std::string{prelude} +
        "f: (b: bool, n: int) -> std::optional<int> = {\n"
        "    x := b && half(n)? == 1;\n"
        "    y := half(n)? == 1 || b;\n"
        "    return 0;\n"
        "}\n");
    const auto* issue = diagnostic_named(result, "try-operator-short-circuit");
    REQUIRE(issue != nullptr);
    CHECK(issue->span.begin.line == 13);
    CHECK(issue->span.begin.column == 22);
    CHECK(issue->message == "`?` on the right of `&&` or `||` would be evaluated unconditionally");
    const auto codes = codes_of(result);
    CHECK(std::count(codes.begin(), codes.end(), "try-operator-short-circuit") == 1);
}

TEST_CASE("try operator is unchanged Cpp2 when the experiment is off", "[experiments][try-operator]") {
    const auto source = std::string{prelude} +
        "f: (n: int) -> std::optional<int> = {\n"
        "    x := half(n)?;\n"
        "    return x;\n"
        "}\n";
    const auto off = cppfrontier::translator{}.translate_source(source);
    REQUIRE(off.has_errors());
    CHECK(codes_of(off) == std::vector<std::string>{"cpp1-conditional-operator"});
    CHECK(off.diagnostics.front().span.begin.line == 13);
    CHECK(off.cpp1.empty());
    const auto on = translate(source);
    CHECK_FALSE(on.has_errors());
    // A ternary is still an error, now a parse error, with the experiment on.
    const auto ternary = translate("f: (b: bool) -> int = { return b ? 1 : 2; }\n");
    CHECK(ternary.has_errors());
    CHECK(ternary.cpp1.empty());
    const auto plain = std::string{prelude} + "main: () = { }\n";
    CHECK(cppfrontier::translator{}.translate_source(plain).cpp1 == translate(plain).cpp1);
}

TEST_CASE("try operator programs compile and run", "[experiments][try-operator][runtime]") {
    const auto source = std::string{prelude} +
        "#include <iostream>\n"
        "sum: (a: std::string, b: std::string) -> std::expected<int, std::string> = {\n"
        "    x := parse(a)?;\n"
        "    return x + parse(b)?;\n"
        "}\n"
        "quarter: (n: int) -> std::optional<int> = {\n"
        "    h := half(n)?;\n"
        "    return half(h)?;\n"
        "}\n"
        "twice: (s: std::string) -> std::expected<std::string, std::string> = {\n"
        "    r: std::expected<std::string, std::string> = s;\n"
        "    if s.empty() { r = std::unexpected(\"none\"); }\n"
        "    first := r?;\n"
        "    return first + r?;\n"
        "}\n"
        "main: () = {\n"
        "    show := :(r: std::expected<int, std::string>) = {\n"
        "        if r { std::cout << \"ok \" << r.value() << \"\\n\"; }\n"
        "        else { std::cout << \"error \" << r.error() << \"\\n\"; }\n"
        "    };\n"
        "    show(sum(\"ab\", \"c\"));\n"
        "    show(sum(\"\", \"c\"));\n"
        "    show(sum(\"ab\", \"\"));\n"
        "    std::cout << quarter(12).value_or(-1) << ' ' << quarter(6).value_or(-1) << '\\n';\n"
        "    std::cout << twice(\"ab\").value() << ' ' << twice(\"\").error() << '\\n';\n"
        "}\n";
    const auto translation = translate(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-try-operator");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "try-operator.cpp";
    const auto executable_path = scratch / "try-operator";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{120});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "ok 3\nerror empty\nerror empty\n3 -1\nabab none\n");
}

TEST_CASE("try operator returns early before a later initialization",
          "[experiments][try-operator][review]") {
    const auto result = translate(std::string{prelude} +
        "f: (o: std::optional<int>, out x: int) -> std::optional<int> = {\n"   // 12
        "    y := o?;\n"                                                       // 13
        "    x = 1;\n"                                                         // 14
        "    return y;\n"                                                      // 15
        "}\n"
        "g: (o: std::optional<int>, out x: int) -> std::optional<int> = {\n"   // 17
        "    x = 1;\n"                                                         // 18
        "    y := o?;\n"                                                       // 19
        "    return y;\n"                                                      // 20
        "}\n");
    const auto* early = diagnostic_named(result, "use-before-initialization");
    REQUIRE(early != nullptr);
    CHECK(early->span.begin.line == 13);
    CHECK(early->message == "x must be initialized before 'return'");
    const auto codes = codes_of(result);
    CHECK(std::count(codes.begin(), codes.end(), "use-before-initialization") == 1);
}
