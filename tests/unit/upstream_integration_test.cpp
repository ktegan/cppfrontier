#include <catch2/catch_all.hpp>
#include <cppfrontier/pipeline.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"
#include "upstream_cases.hpp"
#include "upstream_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

auto upstream_directory() -> std::filesystem::path {
    return std::filesystem::path{CPPFRONTIER_UPSTREAM_DIR} / "regression-tests";
}

auto has_unlowered_interpolation(const cppfrontier::translation_result& translation) -> bool {
    const auto& text = translation.cpp1;
    auto position = std::size_t{0};
    while (position < text.size()) {
        const auto open = text.find('(', position);
        if (open == std::string::npos) return false;
        const auto close = text.find(')', open + 1);
        if (close == std::string::npos) return false;
        if (close + 1 < text.size() && text[close + 1] == '$') return true;
        position = close + 1;
    }
    return false;
}

auto expected_cpp1_exists(std::string_view file_name) -> bool {
    auto stem = std::string{file_name};
    stem.resize(stem.size() - std::string{".cpp2"}.size());
    return std::filesystem::exists(
        std::filesystem::path{CPPFRONTIER_UPSTREAM_TESTS_DIR} / (stem + ".cpp"));
}

}

// Sharded so that `ctest -j` compiles the cases in parallel.
TEMPLATE_TEST_CASE("verified upstream cases translate to compilable Cpp1",
                   "[corpus][upstream][integration]", (std::integral_constant<int, 0>), (std::integral_constant<int, 1>), (std::integral_constant<int, 2>), (std::integral_constant<int, 3>)) {
    constexpr auto shard_count = 4;
    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto directory = upstream_directory();
    const auto scratch = test_support::scratch_directory("cppfrontier-upstream-integration");
    std::filesystem::create_directories(scratch);

    auto index = 0;
    for (const auto& upstream_case : cppfrontier::test::upstream_cases) {
        if (index++ % shard_count != TestType::value) continue;
        const auto file_name = upstream_case.file_name;
        CAPTURE(file_name);
        const auto source_path = directory / file_name;
        REQUIRE(std::filesystem::exists(source_path));
        REQUIRE(expected_cpp1_exists(file_name));

        const auto source = cppfrontier::test::add_pure_cpp2_preamble(
            test_support::read_file(source_path), file_name);
        const auto translation = cppfrontier::translator{}.translate_source(source);
        INFO(translation.cpp1);
        for (const auto& diagnostic : translation.diagnostics) {
            INFO(diagnostic.code << ": " << diagnostic.message);
        }
        REQUIRE_FALSE(translation.has_errors());
        REQUIRE_FALSE(translation.cpp1.empty());
        CHECK_FALSE(has_unlowered_interpolation(translation));

        const auto cpp1_path = scratch / (std::string{file_name} + ".cpp");
        test_support::write_file(cpp1_path, translation.cpp1);
        const auto compiler_result = test_support::run_process(
            {compiler, "-std=c++23", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-fsyntax-only",
             cpp1_path.string()}, scratch, std::chrono::seconds{90});
        INFO(compiler_result.output);
        CHECK_FALSE(compiler_result.timed_out);
        CHECK(compiler_result.exit_status == 0);
    }
}

TEST_CASE("upstream initialization-loop diagnostics are reproduced",
          "[corpus][upstream][integration][diagnostics]") {
    const auto directory = upstream_directory();
    const auto cases = std::array{
        "pure2-initialization-loop-error.cpp2",
        "pure2-initialization-loop-2-error.cpp2",
    };

    for (const auto file_name : cases) {
        CAPTURE(file_name);
        const auto source = cppfrontier::test::add_pure_cpp2_preamble(
            test_support::read_file(directory / file_name), file_name);
        const auto translation = cppfrontier::translator{}.translate_source(source);
        INFO(translation.cpp1);
        for (const auto& diagnostic : translation.diagnostics) {
            INFO(diagnostic.code << ": " << diagnostic.message);
        }
        CHECK(translation.has_errors());
        CHECK(translation.cpp1.empty());
        CHECK(std::any_of(translation.diagnostics.begin(), translation.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.code == "initialization-in-loop";
            }));
    }
}

