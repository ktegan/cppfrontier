#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <string>

namespace {

auto translate(const std::string& source, std::initializer_list<std::string_view> experiments =
                                              {"next-list"}) -> cppfrontier::translation_result {
    auto options = cppfrontier::translation_options{};
    for (const auto name : experiments) REQUIRE(options.experiments.enable(name));
    return cppfrontier::translator{}.translate_source(source, options);
}

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code,
                    std::uint32_t line, std::uint32_t column) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const cppfrontier::diagnostic& value) {
            return value.code == code && value.span.begin.line == line &&
                value.span.begin.column == column;
        });
}

auto contains(const cppfrontier::translation_result& result, std::string_view text) -> bool {
    return result.cpp1.find(text) != std::string::npos;
}

}

TEST_CASE("a next list is one error without the experiment", "[experiments][next-list]") {
    const auto result = translate(
        "f: () = {\n"
        "    a := 0;\n"
        "    b := 0;\n"
        "    while a < 3 next (a += 1, b++) { }\n"
        "    while a < 3 next (a++, b++) { }\n"
        "    while a < 3 next (a += 1) { }\n"
        "    while a < 3 next (a += 1,) { }\n"
        "}\n", {});
    CHECK(has_diagnostic(result, "next-clause-list", 4, 22));
    CHECK(has_diagnostic(result, "next-clause-list", 5, 22));
    CHECK(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto& value) {
                            return value.severity == cppfrontier::diagnostic_severity::error;
                        }) == 2);
}

TEST_CASE("a next list runs one statement per item", "[experiments][next-list]") {
    const auto result = translate(
        "f: () = {\n"
        "    a := 0;\n"
        "    b := 0;\n"
        "    while a < 3 next (a += 1, b++) { }\n"
        "    do { } next (a += 1, _ = b) while a < 6;\n"
        "    for (1, 2) next (a += 1, b++,) do (x) { _ = x; }\n"
        "    while a < 9 next (a += 1) { }\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(contains(result, "for (; cpp2::impl::cmp_less(a,3); [&]{ a += 1; ++b; }()) {\n"));
    CHECK(contains(result,
        "} while ([&]{ a += 1; static_cast<void>(b); return true; }() && "
        "cpp2::impl::cmp_less(a,6));\n"));
    CHECK(contains(result, "        } while (false);\n#line 6\n        a += 1;\n        ++b;\n"));
    CHECK(contains(result, "for (; cpp2::impl::cmp_less(a,9); (a += 1)) {\n"));
}

TEST_CASE("each item of a next list is checked as a next clause", "[experiments][next-list]") {
    const auto result = translate(
        "f: () = {\n"
        "    a := 0;\n"
        "    b := 0;\n"
        "    while a < 3 next (a = b = 1, b++) { }\n"
        "    while a < 3 next (a++, (a, b)) { }\n"
        "    while a < 3 next (a++, b c) { }\n"
        "}\n");
    CHECK(has_diagnostic(result, "chained-assignment", 4, 29));
    CHECK(has_diagnostic(result, "comma-operator", 5, 28));
    CHECK(has_diagnostic(result, "juxtaposed-operands", 6, 30));
}
