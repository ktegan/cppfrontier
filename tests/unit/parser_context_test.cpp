#include <catch2/catch_all.hpp>

#include "../../src/parser_context.hpp"
#include <cppfrontier/pipeline.hpp>

#include <chrono>
#include <random>
#include <string>

TEST_CASE("angle scanning consumes merged closers", "[parser][delimiters][templates]") {
    const auto tokens = cppfrontier::token_list{
        {cppfrontier::token_kind::punctuation, "<", {}},
        {cppfrontier::token_kind::identifier, "X", {}},
        {cppfrontier::token_kind::punctuation, "<", {}},
        {cppfrontier::token_kind::identifier, "Y", {}},
        {cppfrontier::token_kind::punctuation, ">>", {}},
        {cppfrontier::token_kind::punctuation, ";", {}},
    };

    const auto close = cppfrontier::parser_detail::angle_close(tokens, 0, tokens.size());
    REQUIRE(close);
    CHECK(*close == 4);
}

TEST_CASE("angle scanning skips brackets", "[parser][delimiters][templates]") {
    const auto tokens = cppfrontier::tokenize(
        cppfrontier::source_text::from_bytes("<N: int = (1 > 2)> x"), 1, 2);
    const auto close = cppfrontier::parser_detail::angle_close(tokens, 0, tokens.size());
    REQUIRE(close);
    CHECK(tokens[*close].text == ">");
    CHECK(tokens[*close + 1].text == "x");
}

TEST_CASE("bracket matching finds the matching closer", "[parser][delimiters][blocks]") {
    const auto tokens = cppfrontier::tokenize(
        cppfrontier::source_text::from_bytes("{ f(a[1]) } ;"), 1, 2);
    const auto close = cppfrontier::parser_detail::bracket_close(tokens, 0, tokens.size());
    REQUIRE(close);
    CHECK(*close == 8);
    CHECK(cppfrontier::parser_detail::bracket_open(tokens, 0, *close) == 0);
}

TEST_CASE("bracket matching rejects an unterminated or mismatched block",
          "[parser][delimiters][blocks]") {
    const auto unterminated = cppfrontier::tokenize(
        cppfrontier::source_text::from_bytes("value {"), 1, 2);
    CHECK_FALSE(cppfrontier::parser_detail::bracket_close(
        unterminated, 1, unterminated.size()));
    const auto mismatched = cppfrontier::tokenize(
        cppfrontier::source_text::from_bytes("{ f(x; }"), 1, 2);
    CHECK_FALSE(cppfrontier::parser_detail::bracket_close(mismatched, 2, mismatched.size()));
    CHECK(cppfrontier::parser_detail::bracket_close(mismatched, 0, mismatched.size()) == 5);
}

TEST_CASE("top-level items split at commas outside template arguments",
          "[parser][delimiters][templates]") {
    const auto tokens = cppfrontier::tokenize(
        cppfrontier::source_text::from_bytes("std::pair<int, int>(1, 2), a < b, c"), 1, 2);
    const auto items = cppfrontier::parser_detail::top_level_items(tokens, 0, tokens.size());
    REQUIRE(items.size() == 3);
    CHECK(tokens[items[1].first].text == "a");
    CHECK(tokens[items[2].first].text == "c");
}

TEST_CASE("declaration items read comparisons after the initializer",
          "[parser][delimiters][templates]") {
    const auto tokens = cppfrontier::tokenize(
        cppfrontier::source_text::from_bytes("a: int = x < y, b: int"), 1, 2);
    const auto items = cppfrontier::parser_detail::top_level_items(
        tokens, 0, tokens.size(), cppfrontier::parser_detail::angle_rule::declaration);
    REQUIRE(items.size() == 2);
    CHECK(tokens[items[1].first].text == "b");
}

TEST_CASE("template parameter cursor preserves merged closers", "[parser][templates][context]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "f: <T: std::vector<int>> () = 0;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    auto diagnostics = std::vector<cppfrontier::diagnostic>{};
    auto context = cppfrontier::parser_detail::token_cursor{
        source, tokens, diagnostics, {}};
    auto index = std::size_t{0};
    while (index < tokens.size() && tokens[index].text != "<") ++index;
    REQUIRE(index < tokens.size());

    auto parameters = std::vector<cppfrontier::template_parameter>{};
    REQUIRE(context.template_parameters(index, parameters));
    CHECK(diagnostics.empty());
    REQUIRE(parameters.size() == 1);
    CHECK(parameters[0].name == "T");
    CHECK(parameters[0].type.text == "std::vector<int>");
    REQUIRE(index < tokens.size());
    CHECK(tokens[index].text == "(");
}

TEST_CASE("template parameter cursor preserves nested default arguments",
          "[parser][templates][context][defaults]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "f: <T: type = std::vector<std::pair<int, int>>> () = 0;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    auto diagnostics = std::vector<cppfrontier::diagnostic>{};
    auto context = cppfrontier::parser_detail::token_cursor{
        source, tokens, diagnostics, {}};
    auto index = std::size_t{0};
    while (index < tokens.size() && tokens[index].text != "<") ++index;
    REQUIRE(index < tokens.size());

    auto parameters = std::vector<cppfrontier::template_parameter>{};
    REQUIRE(context.template_parameters(index, parameters));
    CHECK(diagnostics.empty());
    REQUIRE(parameters.size() == 1);
    CHECK(parameters[0].name == "T");
    CHECK(parameters[0].is_type_parameter());
    CHECK(parameters[0].default_type.text == "std::vector<std::pair<int, int>>");
    REQUIRE(index < tokens.size());
    CHECK(tokens[index].text == "(");
}


TEST_CASE("template argument closes found in one pass match the scan from each '<'",
          "[parser][delimiters][templates]") {
    const auto words = std::array<std::string_view, 22>{
        "a", "1", "<", "<", "<", ">", ">", ">>", "(", ")", "[", "]", ",", ":", "=", "&&", "||",
        ";", "{", "}", "+", "<="};
    auto random = std::mt19937{12345};
    auto pick = std::uniform_int_distribution<std::size_t>{0, words.size() - 1};
    for (auto round = 0; round < 2000; ++round) {
        auto text = std::string{};
        for (auto count = 0; count < 24; ++count) text += std::string{words[pick(random)]} + " ";
        const auto tokens = cppfrontier::tokenize(cppfrontier::source_text::from_bytes(text), 1, 2);
        const auto closes = cppfrontier::parser_detail::template_argument_closes(tokens);
        for (auto open = std::size_t{0}; open < tokens.size(); ++open) {
            if (tokens[open].text != "<") continue;
            CAPTURE(text, open);
            CHECK(closes[open] == cppfrontier::parser_detail::template_argument_close(tokens, open));
        }
    }
}

TEST_CASE("many comparisons in one bracket group are parsed in linear time",
          "[parser][templates][scaling]") {
    auto text = std::string{"f: (xs...: bool) = { }\nmain: () = { a := 1; b := 2; f(a<b"};
    for (auto count = 1; count < 8000; ++count) text += ", a<b";
    text += "); }\n";
    const auto source = cppfrontier::source_text::from_bytes(text);
    const auto tokens = cppfrontier::tokenize(source, 1, source.lines().size() + 1);
    const auto start = std::chrono::steady_clock::now();
    CHECK(cppfrontier::validate_cpp2(tokens).empty());
    const auto parsed = cppfrontier::parse_cpp2(source, tokens);
    CHECK(parsed.diagnostics.empty());
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
}
