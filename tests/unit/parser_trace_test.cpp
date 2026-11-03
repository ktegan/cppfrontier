#include <catch2/catch_all.hpp>
#include <cppfrontier/pipeline.hpp>

TEST_CASE("parser tracing is disabled without an injected sink", "[parser][trace]") {
    const auto source = cppfrontier::source_text::from_bytes("hello: () = 1;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    const auto result = cppfrontier::parse_cpp2(source, tokens, {});

    CHECK(result.diagnostics.empty());
}

TEST_CASE("parser accepts an injected stderr trace sink", "[parser][trace]") {
    const auto source = cppfrontier::source_text::from_bytes("hello: () = 1;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    const auto result = cppfrontier::parse_cpp2(
        source, tokens, {{cppfrontier::trace_sink_kind::stderr_stream}});

    CHECK(result.diagnostics.empty());
}
