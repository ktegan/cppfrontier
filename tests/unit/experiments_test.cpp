#include <catch2/catch_all.hpp>

#include <cppfrontier/experiments.hpp>
#include <cppfrontier/json.hpp>
#include <cppfrontier/lsp.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <string>

namespace json = cppfrontier::json;

namespace {

// Translates `source` with `experiment` enabled, or none when it is empty.
auto translate(const std::string& source, std::string_view experiment = {})
    -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    if (!experiment.empty()) REQUIRE(options.experiments.enable(experiment));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto contains(const cppfrontier::translation_result& result, std::string_view text) -> bool {
    return result.cpp1.find(text) != std::string::npos;
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code) -> bool {
    return std::ranges::any_of(result.diagnostics,
                               [&](const auto& diagnostic) { return diagnostic.code == code; });
}

}

TEST_CASE("experiments are registered once with a description and a decision", "[experiments]") {
    auto names = std::set<std::string_view>{};
    for (const auto& value : cppfrontier::all_experiments) {
        CHECK(names.insert(value.name).second);
        CHECK_FALSE(value.description.empty());
        CHECK_FALSE(value.decision.empty());
    }
    auto experiments = cppfrontier::experiment_set{};
    CHECK_FALSE(experiments.enable("no-such-experiment"));
    CHECK_FALSE(experiments.enabled("unsafe"));
    CHECK(experiments.enable("unsafe"));
    CHECK(experiments.enabled("unsafe"));
    CHECK_FALSE(experiments.enabled("defer"));

    auto all = cppfrontier::experiment_set{};
    CHECK(all.enable("all"));
    CHECK(all.enabled("unsafe"));
    CHECK(all.enabled("defer"));
}

TEST_CASE("nonnegative-literal-comparisons accepts unsuffixed decimal literals",
          "[experiments][safety]") {
    const auto source = std::string{
        "f: (s: std::size_t, n: int) -> bool = {\n"
        "    a := s > 0;\n"
        "    b := 0 <= s < 10;\n"
        "    c := n >= 1;\n"
        "    return a && b && c;\n"
        "}\n"};
    const auto standard = translate(source);
    CHECK(has_diagnostic(standard, "mixed-sign-comparison"));

    const auto experiment = translate(source, "nonnegative-literal-comparisons");
    INFO(experiment.cpp1);
    CHECK_FALSE(experiment.has_errors());
    CHECK(contains(experiment, "auto a {cpp2::impl::cmp_greater_literal(s,0)};"));
    CHECK(contains(experiment, "[_1 = s]{ return cpp2::impl::cmp_less_eq_literal(0,_1) && "
                               "cpp2::impl::cmp_less_literal(_1,10); }()"));
    CHECK(contains(experiment, "auto c {cpp2::impl::cmp_greater_eq_literal(n,1)};"));

    // Other literals and named operands keep the check.
    for (const auto* rejected : {"f: (s: std::size_t, n: int) -> bool = s > n;\n",
                                 "f: (n: int) -> bool = n > 1u;\n"}) {
        CAPTURE(rejected);
        CHECK(has_diagnostic(translate(rejected, "nonnegative-literal-comparisons"),
                             "mixed-sign-comparison"));
    }
    const auto hexadecimal = translate("f: (s: std::size_t) -> bool = s > 0x10;\n",
                                       "nonnegative-literal-comparisons");
    CHECK(contains(hexadecimal, "cpp2::impl::cmp_greater(s,0x10)"));
    // cmp_less_literal still rejects a bool operand in the Cpp1 compile.
    const auto boolean = translate("f: (b: bool) -> bool = b < 1;\n",
                                   "nonnegative-literal-comparisons");
    CHECK(contains(boolean, "cpp2::impl::cmp_less_literal(b,1)"));
}

TEST_CASE("move-only-to-known-callees moves call arguments only into known parameters",
          "[experiments][last-use]") {
    const auto source = std::string{
        "by_copy: (copy s: std::string) = { }\n"
        "by_ref: (inout s: std::string) = { }\n"
        "overloaded: (copy s: std::string) = { }\n"
        "overloaded: (inout s: std::string, n: int) = { }\n"
        "a: (copy x: std::string) = by_copy(x);\n"
        "b: (copy x: std::string) = by_ref(x);\n"
        "c: (copy x: std::string) = overloaded(x);\n"
        "d: (copy x: std::string) = std::exchange(x, \"\");\n"
        "e: (copy x: std::string) = x.size();\n"
        "f: (copy x: std::string) -> std::string = x + \"\";\n"
        "g: (copy x: std::string, inout v: std::vector<std::string>) = v.push_back(move x);\n"
        "H: type = { release: (move this) = { } peek: (this) = { } }\n"
        "consume: (move s: std::string) = { }\n"
        "h: (copy x: H) = x.release();\n"
        "i: (copy x: H) = x.peek();\n"
        "j: (copy x: std::string) = x.consume();\n"};
    const auto standard = translate(source);
    REQUIRE_FALSE(standard.has_errors());
    for (const auto* text : {"by_copy(CPP2_FORWARD(x))", "by_ref(CPP2_FORWARD(x))",
                             "overloaded(CPP2_FORWARD(x))", "std::exchange(CPP2_FORWARD(x), \"\")",
                             "CPP2_UFCS(size)(CPP2_FORWARD(x))", "CPP2_FORWARD(x) + \"\""}) {
        CAPTURE(text);
        CHECK(contains(standard, text));
    }

    const auto experiment = translate(source, "move-only-to-known-callees");
    REQUIRE_FALSE(experiment.has_errors());
    for (const auto* text : {"by_copy(CPP2_FORWARD(x))", "by_ref(x)", "overloaded(CPP2_FORWARD(x))",
                             "std::exchange(x, \"\")", "CPP2_UFCS(size)(x)",
                             "CPP2_FORWARD(x) + \"\"", "(v, std::move(CPP2_FORWARD(x)))",
                             "CPP2_UFCS(release)(cpp2::move(x))", "CPP2_UFCS(peek)(x)",
                             "CPP2_UFCS(consume)(CPP2_FORWARD(x))"}) {
        CAPTURE(text);
        CHECK(contains(experiment, text));
    }
}

TEST_CASE("LSP initialization options enable experiments", "[experiments][lsp]") {
    const auto initialize = [](json::value names) {
        return json::serialize(json::object{{"jsonrpc", "2.0"}, {"id", 1},
            {"method", "initialize"}, {"params", json::object{{"capabilities", json::object{}},
                {"initializationOptions", json::object{{"experiments", std::move(names)}}}}}});
    };
    const auto open = json::serialize(json::object{{"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"}, {"params", json::object{{"textDocument",
            json::object{{"uri", "file:///s.cpp2"}, {"version", 1},
                         {"text", "f: (s: std::size_t) -> bool = { unsafe { return s > 1u; } }\n"}}}}}});
    const auto diagnostics = [&](cppfrontier::lsp::session& server) {
        const auto replies = server.handle(open);
        REQUIRE(replies.size() == 1);
        return json::parse(replies[0])->find("params")->find("diagnostics")->as_array().size();
    };

    auto standard = cppfrontier::lsp::session{};
    static_cast<void>(standard.handle(initialize(json::array{})));
    CHECK(diagnostics(standard) > 0);

    auto experiment = cppfrontier::lsp::session{};
    static_cast<void>(experiment.handle(initialize(json::array{"unsafe"})));
    CHECK(diagnostics(experiment) == 0);

    auto unknown = cppfrontier::lsp::session{};
    const auto replies = unknown.handle(initialize(json::array{"no-such-experiment"}));
    REQUIRE(replies.size() == 1);
    const auto error = *json::parse(replies[0])->find("error");
    CHECK(error.find("code")->as_number() == -32602);
    CHECK(error.find("message")->as_string().find("no-such-experiment") != std::string::npos);
}

#if CPPFRONTIER_POSIX_PROCESS

TEST_CASE("CLI lists experiments and rejects unknown ones", "[experiments][cli]") {
    const auto run = [](std::string argument) {
        return test_support::run_process({CPPFRONTIER_CLI, std::move(argument), "x.cpp2"},
                                         std::filesystem::temp_directory_path(),
                                         std::chrono::seconds{10});
    };
    const auto listed = run("-experiment=list");
    CHECK(listed.exit_status == 0);
    CHECK(listed.output.starts_with(std::string{cppfrontier::all_experiments.front().name} + "\n"));

    const auto unknown = run("-experiment=no-such-experiment");
    CHECK(unknown.exit_status == 1);
    CHECK(unknown.output == "cppfrontier: error: unknown experiment 'no-such-experiment' "
                            "(try -experiment=list)\n");
}

#endif
