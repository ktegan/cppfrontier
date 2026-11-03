#include <catch2/catch_all.hpp>

#if defined(CPPFRONTIER_HAS_LIBCLANG)

#include <cppfrontier/translator.hpp>
#include "clang_semantic_oracle.hpp"
#include "test_support.hpp"
#include "upstream_cases.hpp"
#include "upstream_support.hpp"

#include <array>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

auto source_path() -> std::filesystem::path {
    return std::filesystem::path{CPPFRONTIER_UPSTREAM_DIR} / "regression-tests";
}

auto describe(const cppfrontier::test::semantic_entity& entity, std::string_view indent = {})
    -> std::string {
    auto output = std::ostringstream{};
    output << indent << entity.kind << ' ' << entity.name << " : " << entity.type
           << " canonical=" << entity.canonical_type
           << " access=" << entity.access << " definition=" << entity.definition << '\n';
    const auto describe_statement = [&](const auto& self,
                                        const cppfrontier::test::semantic_statement& statement,
                                        std::string_view statement_indent) -> void {
        output << statement_indent << "statement " << statement.kind;
        if (!statement.label.empty()) output << " label=" << statement.label;
        for (const auto& declaration : statement.declarations) {
            output << " declares=" << declaration;
        }
        output << '\n';
        for (const auto& child : statement.children) {
            self(self, child, std::string{statement_indent} + "  ");
        }
    };
    for (const auto& statement : entity.statements) {
        describe_statement(describe_statement, statement, std::string{indent} + "  ");
    }
    for (const auto& child : entity.children) {
        output << describe(child, std::string{indent} + "  ");
    }
    return output.str();
}

auto describe(const cppfrontier::test::semantic_program& program) -> std::string {
    auto output = std::ostringstream{};
    for (const auto& entity : program.entities) output << describe(entity);
    return output.str();
}

}

// Sharded so that `ctest -j` compares the cases in parallel; shard N checks every
// eighth case.
TEMPLATE_TEST_CASE("generated Cpp1 preserves normalized upstream declarations and control flow",
                   "[corpus][upstream][semantic]", (std::integral_constant<int, 0>),
                   (std::integral_constant<int, 1>), (std::integral_constant<int, 2>),
                   (std::integral_constant<int, 3>), (std::integral_constant<int, 4>),
                   (std::integral_constant<int, 5>), (std::integral_constant<int, 6>),
                   (std::integral_constant<int, 7>)) {
    constexpr auto shard_count = 8;
    const auto scratch = test_support::scratch_directory("cppfrontier-semantic");
    std::filesystem::create_directories(scratch);

    auto index = 0;
    for (const auto& upstream_case : cppfrontier::test::upstream_cases) {
        if (!upstream_case.compare_semantics) continue;
        if (index++ % shard_count != TestType::value) continue;
        const auto file_name = upstream_case.file_name;
        CAPTURE(file_name);
        const auto source = cppfrontier::test::add_pure_cpp2_preamble(
            test_support::read_file(source_path() / file_name), file_name);
        const auto translation = cppfrontier::translator{}.translate_source(source);
        INFO(translation.cpp1);
        for (const auto& diagnostic : translation.diagnostics) {
            INFO(diagnostic.code << ": " << diagnostic.message);
        }
        REQUIRE_FALSE(translation.has_errors());

        auto expected_name = std::string{file_name};
        expected_name.resize(expected_name.size() - std::string{".cpp2"}.size());
        const auto live = cppfrontier::test::translate_with_upstream(file_name, scratch);
        auto expected_path = std::filesystem::path{CPPFRONTIER_UPSTREAM_TESTS_DIR} /
            (expected_name + ".cpp");
        if (live.available) {
            REQUIRE(live.succeeded);
            expected_path = scratch / (expected_name + "-upstream.cpp");
            test_support::write_file(expected_path, live.output);
        }
        const auto generated_path = scratch / (expected_name + "-generated.cpp");
        test_support::write_file(generated_path, translation.cpp1);

        const auto expected = cppfrontier::test::read_semantic_program(
            expected_path.string(), CPPFRONTIER_RUNTIME_INCLUDE);
        const auto generated = cppfrontier::test::read_semantic_program(
            generated_path.string(), CPPFRONTIER_RUNTIME_INCLUDE);
        const auto expected_description = describe(expected);
        const auto generated_description = describe(generated);
        INFO("expected:\n" << expected_description);
        INFO("generated:\n" << generated_description);
        if (generated != expected) {
            test_support::write_file(scratch / (expected_name + "-expected-semantic.txt"),
                                     expected_description);
            test_support::write_file(scratch / (expected_name + "-generated-semantic.txt"),
                                     generated_description);
        }
        CHECK_FALSE(expected.entities.empty());
        CHECK_FALSE(generated.entities.empty());
        CHECK(generated == expected);
    }
}

#endif
