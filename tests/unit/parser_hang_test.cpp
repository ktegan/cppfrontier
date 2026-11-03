#include <catch2/catch_all.hpp>
#include <cppfrontier/pipeline.hpp>

#include <vector>

TEST_CASE("declaration punctuation has a fail-forward recovery limit", "[parser][recovery]") {
    const auto source = cppfrontier::source_text::from_bytes("order == 6;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, source.lines().size() + 1);
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);

    CHECK_FALSE(parsed.diagnostics.empty());
}
