#include <catch2/catch_all.hpp>
#include <cppfrontier/pipeline.hpp>

#include <chrono>
#include <string>
#include <vector>

TEST_CASE("lexy lexer preserves token text and positions", "[lexer][lexy]") {
    const auto source = cppfrontier::source_text::from_bytes("hello: () = \"hello\";\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    using kind = cppfrontier::token_kind;
    REQUIRE(tokens.size() == 7);
    CHECK(tokens[0].kind == kind::identifier);
    CHECK(tokens[0].text == "hello");
    CHECK(tokens[0].span.begin.line == 1);
    CHECK(tokens[0].span.begin.column == 1);
    CHECK(tokens[1].kind == kind::punctuation);
    CHECK(tokens[1].text == ":");
    CHECK(tokens[1].span.begin.column == 6);
    CHECK(tokens[6].kind == kind::punctuation);
    CHECK(tokens[6].text == ";");
    CHECK(tokens[6].span.begin.column == 20);
}

TEST_CASE("lexy lexer lexes an entire Cpp2 section", "[lexer][lexy]") {
    const auto source = cppfrontier::source_text::from_bytes("f: () -> int = {\n    return 2;\n}\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 4);

    using kind = cppfrontier::token_kind;
    REQUIRE(tokens.size() == 12);
    CHECK(tokens[11].kind == kind::punctuation);
    CHECK(tokens[11].text == "}");
    CHECK(tokens[11].span.begin.line == 3);
}

TEST_CASE("lexy lexer classifies keywords and literals", "[lexer][lexy]") {
    const auto source = cppfrontier::source_text::from_bytes("Point: @value type = 42; type = \"value\";\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    using kind = cppfrontier::token_kind;
    REQUIRE(tokens.size() == 12);
    CHECK(tokens[2].kind == kind::punctuation);
    CHECK(tokens[2].text == "@");
    CHECK(tokens[3].kind == kind::identifier);
    CHECK(tokens[3].text == "value");
    CHECK(tokens[4].kind == kind::keyword);
    CHECK(tokens[4].text == "type");
    CHECK(tokens[6].kind == kind::integer_literal);
    CHECK(tokens[6].text == "42");
    CHECK(tokens[10].kind == kind::string_literal);
    CHECK(tokens[10].text == "\"value\"");
}

TEST_CASE("lexy lexer lexes qualified identifiers as one token", "[lexer][lexy][names]") {
    const auto source = cppfrontier::source_text::from_bytes("std::cout << \"x\";\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    REQUIRE(tokens.size() == 4);
    CHECK(tokens[0].kind == cppfrontier::token_kind::identifier);
    CHECK(tokens[0].text == "std::cout");
    CHECK(tokens[1].text == "<<");
}

TEST_CASE("lexy lexer classifies brackets as punctuation", "[lexer][lexy][brackets]") {
    const auto source = cppfrontier::source_text::from_bytes("callbacks[0](2);\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    REQUIRE(tokens.size() == 8);
    CHECK(tokens[1].kind == cppfrontier::token_kind::punctuation);
    CHECK(tokens[1].text == "[");
    CHECK(tokens[3].kind == cppfrontier::token_kind::punctuation);
    CHECK(tokens[3].text == "]");
}

TEST_CASE("lexy lexer lexes trailing qualified namespaces without reading past the end", "[lexer][lexy][names]") {
    const auto source = cppfrontier::source_text::from_bytes("std::\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    REQUIRE(tokens.size() == 2);
    CHECK(tokens[0].text == "std");
    CHECK(tokens[1].text == "::");
}

TEST_CASE("lexy lexer merges qualified names without reading past short token runs",
          "[lexer][lexy][names]") {
    for (const auto text : {"a", "a::", "a::b", "a::b::", "::a", "operator", "operator=", "<",
                            "<<"}) {
        CAPTURE(text);
        const auto source = cppfrontier::source_text::from_bytes(std::string{text} + "\n");
        const auto tokens = cppfrontier::tokenize(source, 1, 2);
        CHECK(std::none_of(tokens.begin(), tokens.end(), [](const auto& token) {
            return token.text.empty();
        }));
    }
}

TEST_CASE("lexy lexer lexes a lone namespace separator", "[lexer][lexy][names]") {
    const auto source = cppfrontier::source_text::from_bytes("::\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    REQUIRE(tokens.size() == 1);
    CHECK(tokens[0].text == "::");
}

TEST_CASE("lexy lexer reports unknown characters without abandoning tokens", "[lexer][lexy]") {
    const auto source = cppfrontier::source_text::from_bytes("hello: () = ?;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    REQUIRE(tokens.size() == 7);
    CHECK(tokens[5].kind == cppfrontier::token_kind::punctuation);
    CHECK(tokens[5].text == "?");
    CHECK(tokens[5].span.begin.column == 13);
    CHECK(tokens[4].text == "=");
    CHECK(tokens[6].text == ";");
}

TEST_CASE("lexy lexer treats next as a loop-control keyword", "[lexer][lexy]") {
    const auto source = cppfrontier::source_text::from_bytes("while i < 4\n    next i++ { }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 3);

    REQUIRE(tokens.size() == 9);
    CHECK(tokens[4].kind == cppfrontier::token_kind::keyword);
    CHECK(tokens[4].text == "next");
}

TEST_CASE("lexy lexer assigns real positions to parser EOF", "[lexer][lexy][positions]") {
    const auto source = cppfrontier::source_text::from_bytes("f: () = {\n}\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 3);
    REQUIRE(!tokens.empty());
    CHECK(tokens.back().span.begin.line == 2);
}

TEST_CASE("lexy lexer does not emit empty tokens", "[lexer][lexy]") {
    const auto source = cppfrontier::source_text::from_bytes("\nhello: () = \"hello\";\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 3);

    REQUIRE(tokens.size() == 7);
    CHECK(tokens.front().kind == cppfrontier::token_kind::identifier);
    CHECK(tokens.front().text == "hello");
    CHECK(std::all_of(tokens.begin(), tokens.end(), [](const auto& token) {
        return !token.text.empty();
    }));
}

TEST_CASE("lexy lexer skips line and block comments", "[lexer][lexy][comments]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "f: () = { // inline comment\n"
        "    /* block\ncomment */ value\n"
        "}\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 4);

    REQUIRE(!tokens.empty());
    CHECK(std::none_of(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.text.find("comment") != std::string::npos;
    }));
    CHECK(std::any_of(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.text == "value";
    }));
}

TEST_CASE("lexy lexer skips a line comment before an unindented line",
          "[lexer][lexy][comments]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "x: int = 1; // first\n"
        "main: () = {\n"
        "    x = 2; // second\n"
        "} // last");
    const auto tokens = cppfrontier::tokenize(source, 1, 5);

    REQUIRE(!tokens.empty());
    CHECK(std::none_of(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.text == "/" || token.text == "first" || token.text == "second" ||
            token.text == "last";
    }));
    CHECK(tokens.back().text == "}");
}

TEST_CASE("lexy lexer spans stop at the requested section boundary", "[lexer][lexy][positions]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "f: () = { // inline comment\n"
        "    /* block\ncomment */ value\n"
        "}\n"
        "cpp1();\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 4);

    REQUIRE(!tokens.empty());
    const auto& last_token = tokens.back();
    CHECK(last_token.span.begin.line <= 3);
    CHECK(last_token.span.end.line <= 4);
    CHECK(last_token.span.begin.line != 4);
}

TEST_CASE("lexer keeps brace-initialized expressions as ordinary tokens", "[lexer][lexy][braces]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "main: () -> int = { std::cout << std::string{\"x\"}; return 0; }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    const auto open = std::find_if(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.text == "{";
    });
    REQUIRE(open != tokens.end());
    const auto close = std::find_if(open, tokens.end(), [](const auto& token) {
        return token.kind == cppfrontier::token_kind::punctuation && token.text == "}";
    });
    CHECK(close != tokens.end());
    CHECK(std::count_if(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.text == "{";
    }) == 2);
    CHECK(std::count_if(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.text == "}";
    }) == 2);
    CHECK(std::any_of(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.kind == cppfrontier::token_kind::string_literal && token.text == "\"x\"";
    }));
}

TEST_CASE("lexer preserves character literals", "[lexer][lexy][literals]") {
    const auto source = cppfrontier::source_text::from_bytes("main: () = { std::cout << '\\n'; }\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    CHECK(std::any_of(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.kind == cppfrontier::token_kind::character_literal && token.text == "'\\n'";
    }));
}

TEST_CASE("lexer recognizes variadics and requires clauses", "[lexer][lexy][templates]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "f: <Ts...: type> () requires std::regular<Ts> = 0;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    CHECK(std::any_of(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.kind == cppfrontier::token_kind::punctuation && token.text == "...";
    }));
    CHECK(std::any_of(tokens.begin(), tokens.end(), [](const auto& token) {
        return token.kind == cppfrontier::token_kind::keyword && token.text == "requires";
    }));
}

TEST_CASE("lexer applies the documented identifier escape", "[lexer][lexy][names][keywords]") {
    const auto source = cppfrontier::source_text::from_bytes("__identifier__class: () = { };\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);

    REQUIRE(!tokens.empty());
    CHECK(tokens.front().kind == cppfrontier::token_kind::identifier);
    CHECK(tokens.front().text == "class");
    CHECK(tokens.front().escaped_identifier);
}

TEST_CASE("name predicates distinguish contextual and escaped names", "[lexer][lexy][names][keywords]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "type class __identifier__namespace ordinary;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    REQUIRE(tokens.size() == 5);

    CHECK(cppfrontier::is_contextual_name(tokens[0]));
    CHECK(cppfrontier::is_declaration_name(tokens[0]));
    CHECK_FALSE(cppfrontier::is_cpp2_name(tokens[0]));

    CHECK(tokens[1].kind == cppfrontier::token_kind::identifier);
    CHECK(cppfrontier::is_declaration_name(tokens[1]));

    CHECK(cppfrontier::is_cpp2_name(tokens[2]));
    CHECK(tokens[2].text == "namespace");

    CHECK(cppfrontier::is_cpp2_name(tokens[3]));
    CHECK(cppfrontier::is_declaration_name(tokens[3]));
}

TEST_CASE("operator names are one declaration name token",
          "[lexer][lexy][names][upstream-1096][upstream-1214]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "operator(): operator[]: operator<=>: operator+=: std::operator\"\"sv; operator ( );\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    auto names = std::vector<std::string>{};
    for (const auto& token : tokens) {
        if (token.text.find("operator") != std::string::npos) {
            names.push_back(token.text);
            CHECK(cppfrontier::is_declaration_name(token) == (token.text != "operator"));
        }
    }
    CHECK(names == std::vector<std::string>{
        "operator()", "operator[]", "operator<=>", "operator+=", "std::operator\"\"sv",
        "operator"});
}

namespace {

auto lexer_messages(std::string text) -> std::vector<std::string> {
    const auto source = cppfrontier::source_text::from_bytes(std::move(text));
    auto diagnostics = std::vector<cppfrontier::diagnostic>{};
    (void)cppfrontier::tokenize(source, 1, static_cast<std::uint32_t>(source.lines().size() + 1),
                                &diagnostics);
    auto messages = std::vector<std::string>{};
    for (const auto& diagnostic : diagnostics) messages.push_back(diagnostic.message);
    return messages;
}

}

TEST_CASE("lexer reports text that is not a token", "[lexer][lexy][diagnostics]") {
    using strings = std::vector<std::string>;
    CHECK(lexer_messages("x := 1 ` 2;\n") == strings{"unexpected text '`'"});
    CHECK(lexer_messages("x := 1 \\ 2 # 3;\n") ==
          strings{"unexpected text '\\'", "unexpected text '#'"});
    CHECK(lexer_messages("x := 1 \xc3\xa9 2;\n") == strings{"unexpected text '\xc3\xa9'"});
    CHECK(lexer_messages("x := 1 \x01;\n") == strings{"unexpected control character 0x01"});
    CHECK(lexer_messages("x := (1 - 2, 3)?.y$ * 4;\n").empty());
}

TEST_CASE("lexer reports unterminated literals and comments", "[lexer][lexy][diagnostics]") {
    using strings = std::vector<std::string>;
    CHECK(lexer_messages("x := \"abc;\ny := 1;\n") ==
          strings{"string literal is missing its closing \""});
    CHECK(lexer_messages("x := u8'a;\n") ==
          strings{"character literal is missing its closing '"});
    CHECK(lexer_messages("x := R\"(abc;\n") ==
          strings{"raw string literal is missing its closing )\""});
    CHECK(lexer_messages("x := 1; /* note\n") ==
          strings{"block comment is missing its closing */"});
    CHECK(lexer_messages("x := u8'a' + 'b' + '\\'';\n").empty());
}

TEST_CASE("lexer reports a character literal that is not one character",
          "[lexer][lexy][diagnostics][hunt]") {
    using strings = std::vector<std::string>;
    const auto message = [](const std::string& literal) {
        return "character literal " + literal + " must hold exactly one character or escape "
            "sequence - use a string literal for text";
    };
    CHECK(lexer_messages("x := 'ab';\n") == strings{message("'ab'")});
    CHECK(lexer_messages("x := '\\\\n';\n") == strings{message("'\\\\n'")});
    CHECK(lexer_messages("x := L'';\n") == strings{message("L''")});
    CHECK(lexer_messages("x := '\\x41g';\n") == strings{message("'\\x41g'")});
    CHECK(lexer_messages("x := 'a' + '\\n' + '\\\\' + '\\'' + '\\x41' + '\\101' + '\\0' + "
                         "u8'x' + U'\\U0001F600' + u'\\u00e9' + '\\x{41}' + '\\o{101}' + "
                         "u'\xc3\xa9' + U'\xc3\xa9' + L'\xc3\xa9' + '\"' + 1'000;\n")
              .empty());
    const auto code_units = [](const std::string& literal, const std::string& unprefixed) {
        return "character literal " + literal + " holds more than one UTF-8 code unit - write "
            "U" + unprefixed + " for one code point, or a string literal for text";
    };
    CHECK(lexer_messages("x := '\xc3\xa9';\n") ==
          strings{code_units("'\xc3\xa9'", "'\xc3\xa9'")});
    CHECK(lexer_messages("x := u8'\xc3\xa9';\n") ==
          strings{code_units("u8'\xc3\xa9'", "'\xc3\xa9'")});
    // A code point beyond 16 bits needs two UTF-16 code units.
    CHECK(lexer_messages("x := u'\xf0\x9f\x98\x80';\n") ==
          strings{"character literal u'\xf0\x9f\x98\x80' holds more than one UTF-16 code unit - "
                  "write U'\xf0\x9f\x98\x80' for one code point, or a string literal for text"});
    CHECK(lexer_messages("x := U'\xf0\x9f\x98\x80' + L'\xf0\x9f\x98\x80';\n").empty());
}

TEST_CASE("lexer reports an interpolation with no opening parenthesis",
          "[lexer][lexy][diagnostics][interpolation]") {
    using strings = std::vector<std::string>;
    CHECK(lexer_messages("s := \"\\(x)$\";\n") ==
          strings{"no matching ( for string interpolation ending in )$"});
    CHECK(lexer_messages("s := \"smile :)$\";\n") ==
          strings{"no matching ( for string interpolation ending in )$"});
    CHECK(lexer_messages("s := \"\\\\(x)$ (')')$ (f(\\\")\\\"))$\";\n").empty());
}

TEST_CASE("lexer reports an interpolation in a prefixed string literal",
          "[lexer][lexy][diagnostics][interpolation]") {
    using strings = std::vector<std::string>;
    const auto message = [](const std::string& prefix) {
        return "a string interpolation '(...)$' is not allowed in a '" + prefix + "' string "
            "literal - an interpolation is a std::string; use an unprefixed literal";
    };
    CHECK(lexer_messages("s := u8\"(x)$\";\n") == strings{message("u8")});
    CHECK(lexer_messages("s := u\"a (x)$ b\";\n") == strings{message("u")});
    CHECK(lexer_messages("s := U\"(x)$\";\n") == strings{message("U")});
    CHECK(lexer_messages("s := L\"(x)$\";\n") == strings{message("L")});
    CHECK(lexer_messages("s := $u8R\"(a(x)$)\";\n") == strings{message("u8")});
    CHECK(lexer_messages("s := $LR\"d(a)d\";\n") == strings{message("L")});
    CHECK(lexer_messages("s := u8\"(x)\" + u8R\"((x)$)\" + L\"\\\\(x)\" + $R\"((x)$)\" + "
                         "\"(x)$\";\n")
              .empty());
}

TEST_CASE("operator names lex as one token before a colon", "[lexer][lexy][names]") {
    for (const auto* name : {"operator*=", "operator*", "operator=", "operator->*"}) {
        const auto source = cppfrontier::source_text::from_bytes(
            std::string{name} + ": (inout this, y: int) = { }\n");
        const auto tokens = cppfrontier::tokenize(source, 1, 2);
        REQUIRE(tokens.size() > 1);
        CHECK(tokens[0].text == name);
        CHECK(tokens[1].text == ":");
    }
}

TEST_CASE("a bare identifier escape is an ordinary identifier", "[lexer][lexy][names]") {
    const auto source = cppfrontier::source_text::from_bytes("x := __identifier__;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    REQUIRE(tokens.size() == 4);
    CHECK(tokens[2].text == "__identifier__");
    CHECK_FALSE(tokens[2].escaped_identifier);
}

TEST_CASE("tokenizing every section is linear in the file size", "[lexer][lexy][performance]") {
    auto text = std::string{};
    constexpr auto sections = 40000;
    for (auto index = 0; index < sections; ++index) {
        text += "// note\nf" + std::to_string(index) + ": () = { }\n";
    }
    const auto start = std::chrono::steady_clock::now();
    const auto source = cppfrontier::source_text::from_bytes(text);
    REQUIRE(source.cpp2_sections().size() == sections);
    auto count = std::size_t{0};
    for (const auto& section : source.cpp2_sections()) {
        const auto tokens = cppfrontier::tokenize(source, section.begin_line,
                                                  section.end_line_exclusive);
        count += tokens.size();
        if (section.begin_line == 2 * sections) {
            REQUIRE(!tokens.empty());
            CHECK(tokens.front().span.begin.line == 2 * sections);
        }
    }
    CHECK(count == 7 * static_cast<std::size_t>(sections));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{10});
}

TEST_CASE("a declaration's template close before '=' is split", "[lexer][lexy][review]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "v: std::vector<std::vector<int>>= (); b: bool = a >= c;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 2);
    auto texts = std::vector<std::string>{};
    for (const auto& value : tokens) texts.push_back(value.text);
    CHECK(texts == std::vector<std::string>{"v", ":", "std::vector", "<", "std::vector", "<",
        "int", ">>", "=", "(", ")", ";", "b", ":", "bool", "=", "a", ">=", "c", ";"});
    CHECK(tokens[7].span.end.column == 33);
    CHECK(tokens[8].span.begin.column == 33);
    CHECK(tokens[8].span.end.column == 34);
}

TEST_CASE("every type's template close before '=' or '==' is split", "[lexer][lexy][review]") {
    const auto texts_of = [](std::string_view text) {
        const auto source = cppfrontier::source_text::from_bytes(std::string{text});
        auto texts = std::vector<std::string>{};
        for (const auto& value : cppfrontier::tokenize(source, 1, 2)) texts.push_back(value.text);
        return texts;
    };
    CHECK(texts_of("g: () -> std::vector<int>= 1;\n") == std::vector<std::string>{
        "g", ":", "(", ")", "->", "std::vector", "<", "int", ">", "=", "1", ";"});
    CHECK(texts_of("x: * const A<int>::B<int>= p;\n") == std::vector<std::string>{
        "x", ":", "*", "const", "A", "<", "int", ">", "::", "B", "<", "int", ">", "=", "p", ";"});
    CHECK(texts_of("w: A<B<int>>== v;\n") == std::vector<std::string>{
        "w", ":", "A", "<", "B", "<", "int", ">>", "==", "v", ";"});
    CHECK(texts_of("b := a < c >= d;\n") == std::vector<std::string>{
        "b", ":=", "a", "<", "c", ">=", "d", ";"});
}

TEST_CASE("not followed by whitespace is the operator !", "[lexer][lexy]") {
    const auto source = cppfrontier::source_text::from_bytes("x := not b && not(c) && not\n d;\n");
    const auto tokens = cppfrontier::tokenize(source, 1, 3);

    using kind = cppfrontier::token_kind;
    REQUIRE(tokens.size() == 13);
    CHECK(tokens[2].kind == kind::punctuation);
    CHECK(tokens[2].text == "!");
    CHECK(tokens[2].span.end.column == 9);
    CHECK(tokens[5].kind == kind::identifier);
    CHECK(tokens[5].text == "not");
    CHECK(tokens[10].text == "!");
}
