#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <chrono>
#include <filesystem>
#include <string>

#include "test_support.hpp"

namespace {

auto run_program(const std::string& cpp1, const std::string& name) -> test_support::process_result {
    const auto scratch = test_support::scratch_directory("cppfrontier-regex");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (name + ".cpp");
    const auto executable_path = scratch / name;
    test_support::write_file(cpp1_path, cpp1);
    const auto compiled = test_support::run_process({CPPFRONTIER_GXX, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{120});
    if (compiled.timed_out || compiled.exit_status != 0) return compiled;
    return test_support::run_process({executable_path.string()}, scratch, std::chrono::seconds{10});
}

}

TEST_CASE("@regex replaces regex members with generated matchers", "[regex]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "matchers: @regex type = {\n"
        "    regex := R\"((\\w+) (\\w+))\";\n"
        "    regex_no_case := R\"(/(ab)+/i)\";\n"
        "    other: int = 1;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("#include \"cpp2regex.h\"") != std::string::npos);
    CHECK(result.cpp1.find("template<typename CharT> struct regex_matcher {") != std::string::npos);
    CHECK(result.cpp1.find("cpp2::regex::regular_expression<char, regex_matcher<char>> regex {};") !=
          std::string::npos);
    CHECK(result.cpp1.find("cpp2::regex::regular_expression<char, regex_no_case_matcher<char>> "
                           "regex_no_case {};") != std::string::npos);
    CHECK(result.cpp1.find("match_context<CharT, Iter, 3>") != std::string::npos);
    CHECK(result.cpp1.find("return R\"((\\w+) (\\w+))\";") != std::string::npos);
    CHECK(result.cpp1.find("return R\"(/(ab)+/i)\";") != std::string::npos);
    CHECK(result.cpp1.find("int other {1};") != std::string::npos);
}

TEST_CASE("@regex matchers search, match, and capture", "[regex][runtime]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "m: @regex type = {\n"
        "    regex := R\"((?<first>\\w+) (\\w+))\";\n"
        "    regex_ci := R\"(/(ab)+c?/i)\";\n"
        "    regex_alt := R\"(x(?:ab|cd){2,}+y)\";\n"
        "    regex_behind := R\"((?<=a)b\\d*)\";\n"
        "    regex_ref := R\"((a|b)\\1)\";\n"
        "    regex_dollar := R\"(\\)\\x24)\";\n"
        "}\n"
        "main: () -> int = {\n"
        "    v: m = ();\n"
        "    r := v.regex.match(\"Margaret Hamilton\");\n"
        "    if !r.matched || r.group(2) != \"Hamilton\" { return 1; }\n"
        "    if r.group(\"first\") != \"Margaret\" { return 2; }\n"
        "    if v.regex_ci.search(\"xxAbaBC\").group(0) != \"AbaBC\" { return 3; }\n"
        "    if !v.regex_alt.match(\"xabcdaby\").matched { return 4; }\n"
        "    if v.regex_alt.match(\"xaby\").matched { return 5; }\n"
        "    if v.regex_behind.search(\"bab12\").group(0) != \"b12\" { return 6; }\n"
        "    if v.regex_ref.search(\"abba\").group_start(0) != 1 { return 7; }\n"
        "    if v.regex_dollar.search(\"x)\\044\").group_start(0) != 1 { return 8; }\n"
        "    return 0;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto run = run_program(result.cpp1, "regex-runtime");
    INFO(run.output);
    REQUIRE_FALSE(run.timed_out);
    CHECK(run.exit_status == 0);
}

TEST_CASE("@regex reports invalid patterns and initializers", "[regex]") {
    const auto message = [](const std::string& source) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().message;
    };
    CHECK(message("t: @regex type = { regex := R\"(a{2,1})\"; }\n") ==
          "Error during parsing of regex 'a{2,1}' at position '5': Min and max values in range "
          "are wrong it should hold 0 <= min <= max. Have 0 <= 2 <= 1");
    CHECK(message("t: @regex type = { regex_x := R\"(\\1(a))\"; }\n") ==
          "Error during parsing of regex '\\1(a)' at position '1': Group reference is used before "
          "the group is declared.");
    CHECK(message("t: @regex type = { regex := R\"([[:nope:]])\"; }\n").starts_with(
          "Error during parsing of regex '[[:nope:]]' at position '7': Unsupported character class."));
    CHECK(message("t: @regex type = { regex: std::string; }\n") ==
          "Regular expression must have an initializer.");
    CHECK(message("t: @regex type = { regex: std::string = std::string(); }\n") ==
          "Unknown string format 'std::string()'");
    // Only `regex` and `regex_*` members are replaced.
    CHECK(message("t: @regex type = { pattern := R\"(a)\"; }\n") ==
          "a type-scope object must have a declared type");
}

TEST_CASE("next is an ordinary name outside a loop's next clause", "[regex][statements]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (s: std::string) -> int = {\n"
        "    i := 0;\n"
        "    while i < 3 next i++ { }\n"
        "    next := s.begin();\n"
        "    if next != s.end() { next++; }\n"
        "    while next != s.end() next next++ { }\n"
        "    return i;\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("for (; cpp2::impl::cmp_less(i,3); ++i)") != std::string::npos);
    CHECK(result.cpp1.find("if (next != CPP2_UFCS(end)(s)) {") != std::string::npos);
    CHECK(result.cpp1.find("for (; next != CPP2_UFCS(end)(s); ++next)") != std::string::npos);
}

TEST_CASE("a == function expression keeps a const call operator", "[regex][function-expressions]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () = {\n"
        "    count := 0;\n"
        "    f := :(x: int) -> bool == { count&$* += x; return true; };\n"
        "    _ = f(1);\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("[_0 = &(count)](cpp2::impl::in<int> x) -> bool") != std::string::npos);
}

TEST_CASE("@regex escapes quote non-word characters, in and out of classes",
          "[regex][runtime][review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "m: @regex type = {\n"
        "    regex_dollar := R\"(a\\$b)\";\n"
        "    regex_dash := R\"(\\-\\/\\#)\";\n"
        "    regex_class := R\"([\\n\\t\\\\\\-\\.\\$\\]])\";\n"
        "    regex_range := R\"(^[\\x00-\\x1f\\101-\\o{103}]+$)\";\n"
        "    regex_short := R\"(^[\\d-z]+$)\";\n"
        "    regex_caret := R\"(^[a^b]$)\";\n"
        "    regex_hex := R\"(^\\x4g[\\x4g]$)\";\n"
        "}\n"
        "main: () -> int = {\n"
        "    v: m = ();\n"
        "    if !v.regex_dollar.match(\"a$b\").matched { return 1; }\n"
        "    if !v.regex_dash.match(\"-/#\").matched { return 2; }\n"
        "    for (\"\\n\", \"\\t\", \"\\\\\", \"-\", \".\", \"$\", \"]\") do (s) {\n"
        "        if !v.regex_class.match(s).matched { return 3; }\n"
        "    }\n"
        "    if v.regex_class.search(\"ab\").matched { return 4; }\n"
        "    if !v.regex_range.match(\"\\001AC\").matched { return 5; }\n"
        "    if v.regex_range.search(\"D\").matched { return 6; }\n"
        "    if !v.regex_short.match(\"1-z\").matched { return 7; }\n"
        "    if v.regex_short.search(\"y\").matched { return 8; }\n"
        "    if !v.regex_caret.match(\"^\").matched { return 9; }\n"
        "    if !v.regex_hex.match(\"\\004gg\").matched { return 10; }\n"
        "    if !v.regex_hex.match(\"\\004g\\004\").matched { return 11; }\n"
        "    return 0;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto run = run_program(result.cpp1, "regex-escapes");
    INFO(run.output);
    REQUIRE_FALSE(run.timed_out);
    CHECK(run.exit_status == 0);
}

TEST_CASE("@regex rejects unclosed groups, unknown escapes, and wide characters",
          "[regex][review]") {
    const auto message = [](const std::string& pattern) {
        const auto result = cppfrontier::translator{}.translate_source(
            "t: @regex type = { regex := R\"(" + pattern + ")\"; }\n");
        return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().message;
    };
    for (const auto* pattern : {"a(b", "(a|b", "(?:b", "(?=b", "(?<n>b", "(?>b", "(?|b",
                                "(*pla:b", "(?i:b"}) {
        INFO(pattern);
        CHECK(message(pattern).ends_with("Group without closing bracket."));
    }
    for (const auto* pattern : {"\\Q", "\\E", "\\R", "\\X", "\\cK", "\\l", "\\U", "\\p{L}"}) {
        INFO(pattern);
        CHECK(message(pattern).find("Unknown escape sequence: \\") != std::string::npos);
    }
    for (const auto* pattern : {"[\\N]", "[\\cK]", "[\\R]"}) {
        INFO(pattern);
        CHECK(message(pattern).find("Unknown escape sequence in character class") !=
              std::string::npos);
    }
    for (const auto* pattern : {"\\x{100}", "\\o{777}", "\\777", "[\\x{141}]"}) {
        INFO(pattern);
        CHECK(message(pattern).ends_with("Unicode is not supported."));
    }
    CHECK(message("a\\").ends_with("Escape without a following character."));
    CHECK(message("\\x{4g}").ends_with("Could not convert hexadecimal to int."));
    CHECK(message("\\o{8}").ends_with("Could not convert octal to int."));
    CHECK(message("[z-a]").ends_with("Invalid range in character class."));
    const auto concatenated = cppfrontier::translator{}.translate_source(
        "t: @regex type = { regex := R\"(a)\" R\"(b)\"; }\n");
    REQUIRE_FALSE(concatenated.diagnostics.empty());
    CHECK(concatenated.diagnostics.front().message == "Unknown string format 'R\"(a)\" R\"(b)\"'");
}

TEST_CASE("@regex reads Perl's POSIX classes and numbers groups in nested branch resets",
          "[regex][runtime][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "m: @regex type = {\n"
        "    regex_digit := R\"([[:digit:]]+)\";\n"
        "    regex_digits := R\"([[:digits:]]+)\";\n"
        "    regex_not_alpha := R\"([[:^alpha:]x]+)\";\n"
        "    regex_reset := R\"((?|x|(?:(b)|c)(d))(e))\";\n"
        "}\n"
        "main: () -> int = {\n"
        "    v: m = ();\n"
        "    if v.regex_digit.search(\"ab12c\").group(0) != \"12\" { return 1; }\n"
        "    if v.regex_digits.search(\"ab12c\").group(0) != \"12\" { return 2; }\n"
        "    if v.regex_not_alpha.search(\"ab1-xc\").group(0) != \"1-x\" { return 3; }\n"
        "    r := v.regex_reset.search(\"bde\");\n"
        "    if r.group_number() != 4 || r.group(1) != \"b\" || r.group(2) != \"d\" { return 4; }\n"
        "    if r.group(3) != \"e\" { return 5; }\n"
        "    return 0;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto run = run_program(result.cpp1, "regex-posix-branch-reset");
    INFO(run.output);
    REQUIRE_FALSE(run.timed_out);
    CHECK(run.exit_status == 0);
}

TEST_CASE("@regex reads a brace that does not start a quantifier as a literal",
          "[regex][runtime][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "m: @regex type = {\n"
        "    regex_digits := R\"(a{2B)\";\n"
        "    regex_open := R\"(a{)\";\n"
        "    regex_comma := R\"(a{,}b)\";\n"
        "    regex_first := R\"(x|{2})\";\n"
        "    regex_modifier := R\"((?i){2})\";\n"
        "    regex_blanks := R\"(a{ 2 , 3 })\";\n"
        "    regex_after := R\"(a{2}{)\";\n"
        "}\n"
        "main: () -> int = {\n"
        "    v: m = ();\n"
        "    if v.regex_digits.search(\"aa{2B\").group(0) != \"a{2B\" { return 1; }\n"
        "    if v.regex_open.search(\"aa{\").group(0) != \"a{\" { return 2; }\n"
        "    if v.regex_comma.search(\"a{,}b\").group(0) != \"a{,}b\" { return 3; }\n"
        "    if v.regex_first.search(\"{2}\").group(0) != \"{2}\" { return 4; }\n"
        "    if v.regex_modifier.search(\"{2}\").group(0) != \"{2}\" { return 5; }\n"
        "    if v.regex_blanks.search(\"aaaa\").group(0) != \"aaa\" { return 6; }\n"
        "    if v.regex_after.search(\"aaa{\").group(0) != \"aa{\" { return 7; }\n"
        "    return 0;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    const auto run = run_program(result.cpp1, "regex-literal-brace");
    INFO(run.output);
    REQUIRE_FALSE(run.timed_out);
    CHECK(run.exit_status == 0);
}

TEST_CASE("@regex rejects the braces and quantifiers that Perl rejects", "[regex][diagnostics]") {
    const auto message = [](const std::string& pattern) {
        const auto result = cppfrontier::translator{}.translate_source(
            "t: @regex type = { regex := R\"(" + pattern + ")\"; }\n");
        return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().message;
    };
    CHECK(message("\\d{x").ends_with("A '{' after \\d must be escaped as '\\{'."));
    CHECK(message("\\n{2B").ends_with("A '{' after \\n must be escaped as '\\{'."));
    CHECK(message("\\x61{x").empty());
    CHECK(message("a{02}").ends_with("Range bound 02 has a leading zero."));
    CHECK(message("a{1,00}").ends_with("Range bound 00 has a leading zero."));
    CHECK(message("a{99999999999}").ends_with("Could not convert range to number."));
    CHECK(message("a{2B(}").ends_with("Group without closing bracket."));
    CHECK(message("x|*").ends_with("'*' without previous element."));
    CHECK(message("(?i)+").ends_with("'+' without previous element."));
    for (const auto* pattern : {"a**", "a{2}*", "a+?+", "a?+?", "a{2}{3}", "a{2}++", "a*{2,}",
                                "a*(?#c)*", "(?x)a* *"}) {
        INFO(pattern);
        CHECK(message(pattern).ends_with("Nested quantifiers."));
    }
    for (const auto* pattern : {"a*+", "a*?", "a{2}?", "a{2}+", "a*{", "(?:a*)*", "a(?#c)*"}) {
        INFO(pattern);
        CHECK(message(pattern).empty());
    }
}

TEST_CASE("@regex undoes \\K of an atomic group that fails later", "[regex][runtime][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "m: @regex type = {\n"
        "    regex_atomic := R\"((?>a\\K)c|a)\";\n"
        "    regex_possessive := R\"((?:a\\K)++c|a)\";\n"
        "    regex_kept := R\"((?>a\\K)c|a)\";\n"
        "}\n"
        "main: () -> int = {\n"
        "    v: m = ();\n"
        "    r := v.regex_atomic.search(\"ab\");\n"
        "    if r.group_start(0) != 0 || r.group_end(0) != 1 { return 1; }\n"
        "    s := v.regex_possessive.search(\"ab\");\n"
        "    if s.group_start(0) != 0 || s.group_end(0) != 1 { return 2; }\n"
        "    t := v.regex_kept.search(\"ac\");\n"
        "    if t.group_start(0) != 1 || t.group_end(0) != 2 { return 3; }\n"
        "    return 0;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    const auto run = run_program(result.cpp1, "regex-keep-atomic");
    INFO(run.output);
    REQUIRE_FALSE(run.timed_out);
    CHECK(run.exit_status == 0);
}

TEST_CASE("@regex rejects \\K in a lookaround and a lookbehind longer than Perl allows",
          "[regex][diagnostics]") {
    const auto message = [](const std::string& pattern) {
        const auto result = cppfrontier::translator{}.translate_source(
            "t: @regex type = { regex := R\"(" + pattern + ")\"; }\n");
        return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().message;
    };
    for (const auto* pattern : {"(?<=a\\K)b", "(?<!a\\K)b", "(?=a\\K)a", "(?!a\\K)b",
                                "(?<=a(?>\\K))b", "(?=(?<=\\K))"}) {
        INFO(pattern);
        CHECK(message(pattern).ends_with("\\K is not permitted in a lookahead or lookbehind."));
    }
    for (const auto* pattern : {"(a)(?<=\\1)b", "(a)(?<!\\1)b", "(?<n>a)(?<=\\k<n>)b",
                                "(a)(?<=a(?:\\g{-1})?)b", "(a)(?=(?<=\\1))b", "(?<=a+)b", "(?<=a*)b",
                                "(?<=a{1,}?)b", "(?<=a{256})b", "(?<=a{0,255}b)c",
                                "(?<=(?:a{100}){3})b", "(?<=(?>a+))b", "(?<=x(?<=a+))b"}) {
        INFO(pattern);
        CHECK(message(pattern).ends_with("Lookbehind longer than 255 not implemented."));
    }
    for (const auto* pattern : {"(?<=a{1,255})b", "(?<=a{0,254}b)c", "(?<=a|bc)b", "(?<=\\b*)b",
                                "(?<=(?=a+))b", "(?<=(?!a+)x)b", "(?<=.{255})b", "(?<=a{100}){3}b",
                                "(?<=[ab]{200}.{55})b"}) {
        INFO(pattern);
        CHECK(message(pattern).empty());
    }
    // A lookahead inside a lookbehind matches forward, so it may hold a backreference.
    CHECK(message("(a)(?<=(?=\\1)a)b").empty());
    CHECK(message("a\\Kb").empty());
}

TEST_CASE("@regex matches an empty alternative as the empty string", "[regex][runtime]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "m: @regex type = {\n"
        "    regex_first := R\"(|a)\";\n"
        "    regex_last := R\"(a|)\";\n"
        "    regex_group := R\"((|a))\";\n"
        "    regex_middle := R\"(a||b)\";\n"
        "    regex_loop := R\"((a|)+b)\";\n"
        "}\n"
        "main: () -> int = {\n"
        "    v: m = ();\n"
        "    r := v.regex_first.search(\"aab\");\n"
        "    if !r.matched || r.group_end(0) != 0 { return 1; }\n"
        "    if v.regex_last.search(\"aab\").group(0) != \"a\" { return 2; }\n"
        "    g := v.regex_group.search(\"aab\");\n"
        "    if !g.matched || g.group_end(0) != 0 || !g.ctx.get_group(1).matched { return 3; }\n"
        "    if v.regex_middle.search(\"b\").group(0) != \"\" { return 4; }\n"
        "    l := v.regex_loop.search(\"aab\");\n"
        "    if l.group(0) != \"aab\" || l.group_start(1) != 2 || l.group_end(1) != 2 { return 5; }\n"
        "    return 0;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    const auto run = run_program(result.cpp1, "regex-empty-alternative");
    INFO(run.output);
    REQUIRE_FALSE(run.timed_out);
    CHECK(run.exit_status == 0);
}
