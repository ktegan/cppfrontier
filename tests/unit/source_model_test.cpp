#include <catch2/catch_all.hpp>
#include <cppfrontier/source.hpp>

TEST_CASE("mixed source preserves side-by-side boundaries", "[source][model]") {
    const auto source = cppfrontier::source_text::from_bytes("#include <iostream>\nmain: () = { }\nint cpp1();\n");
    const auto& lines = source.lines();

    REQUIRE(lines.size() == 3);
    CHECK(lines[0].category == cppfrontier::source_category::preprocessor);
    CHECK(lines[1].category == cppfrontier::source_category::cpp2);
    CHECK(lines[2].category == cppfrontier::source_category::cpp1);
    REQUIRE(source.cpp2_sections().size() == 1);
    CHECK(source.cpp2_sections()[0].begin_line == 2);
    CHECK(source.cpp2_sections()[0].end_line_exclusive == 3);
}

TEST_CASE("trailing comment on the last Cpp2 line remains in its section", "[source][model]") {
    const auto source = cppfrontier::source_text::from_bytes("main: () = {\n    x: int; // note\n}\n");
    const auto sections = source.cpp2_sections();

    REQUIRE(sections.size() == 1);
    CHECK(sections[0].begin_line == 1);
    CHECK(sections[0].end_line_exclusive == 4);
}

TEST_CASE("source text spans align after leading blank lines", "[source][positions]") {
    const auto source = cppfrontier::source_text::from_bytes("\nwhile i < 4 next i++ { }\n");
    CHECK(source.text_between({2, 7}, {2, 12}) == "i < 4");
}

TEST_CASE("blank line between Cpp2 declarations remains in one section", "[source][model]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "hello: () = \"hello\";\n\nname: () = \"name\";\n");
    const auto sections = source.cpp2_sections();
    REQUIRE(sections.size() == 1);
    CHECK(sections[0].begin_line == 1);
    CHECK(sections[0].end_line_exclusive == 4);
    CHECK(source.lines()[1].category == cppfrontier::source_category::empty);
}

TEST_CASE("comment between Cpp2 declarations starts a new section",
          "[source][model][comments][upstream-1449]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "first: () = { }\n"
        "// between declarations\n"
        "second: () = { }\n");
    const auto sections = source.cpp2_sections();
    REQUIRE(sections.size() == 2);
    CHECK(sections[0].end_line_exclusive == 2);
    CHECK(sections[1].begin_line == 3);
}

TEST_CASE("declarations after a Cpp2 block remain in the Cpp2 section", "[source][model][blocks]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "tools: namespace = {\n"
        "    value: int = 1;\n"
        "}\n"
        "answer: () -> int = 42;\n");
    const auto sections = source.cpp2_sections();
    REQUIRE(sections.size() == 1);
    CHECK(sections[0].begin_line == 1);
    CHECK(sections[0].end_line_exclusive == 5);
}

TEST_CASE("nested Cpp2 bodies remain in one section", "[source][model][blocks]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "Box: type = {\n"
        "    Inner: type = {\n"
        "        value: int = 0;\n"
        "    }\n"
        "}\n");
    REQUIRE(source.lines().size() == 5);
    for (const auto& line : source.lines()) {
        CHECK(line.category == cppfrontier::source_category::cpp2);
    }
    REQUIRE(source.cpp2_sections().size() == 1);
    CHECK(source.cpp2_sections()[0].begin_line == 1);
    CHECK(source.cpp2_sections()[0].end_line_exclusive == 6);
}

TEST_CASE("comments do not change Cpp2 block depth", "[source][model][blocks][comments]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "main: () = {\n"
        "    // } closes nothing\n"
        "    value: int = 1;\n"
        "}\n"
        "after: () -> int = 1;\n");

    REQUIRE(source.lines().size() == 5);
    CHECK(source.lines()[1].category == cppfrontier::source_category::comment);
    CHECK(source.lines()[2].category == cppfrontier::source_category::cpp2);
    CHECK(source.lines()[3].category == cppfrontier::source_category::cpp2);
    CHECK(source.lines()[4].category == cppfrontier::source_category::cpp2);
    REQUIRE(source.cpp2_sections().size() == 1);
    CHECK(source.cpp2_sections()[0].end_line_exclusive == 6);
}

TEST_CASE("trailing comments do not break Cpp2 brace bookkeeping",
          "[source][model][blocks][comments]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "main: () = { // } opens nothing\n"
        "    value: int = 1;\n"
        "}\n"
        "after: () -> int = 1;\n");

    REQUIRE(source.lines().size() == 4);
    CHECK(source.lines()[0].category == cppfrontier::source_category::cpp2);
    CHECK(source.lines()[1].category == cppfrontier::source_category::cpp2);
    CHECK(source.lines()[2].category == cppfrontier::source_category::cpp2);
    CHECK(source.lines()[3].category == cppfrontier::source_category::cpp2);
    REQUIRE(source.cpp2_sections().size() == 1);
    CHECK(source.cpp2_sections()[0].end_line_exclusive == 5);
}

TEST_CASE("comment markers inside ordinary strings do not change source state",
          "[source][model][comments][strings][upstream-442]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "main: () = {\n"
        "    first := \"// not a comment\";\n"
        "    second := \"/* not a block comment\";\n"
        "    third := \"*/ is ordinary text\";\n"
        "}\n"
        "after: () -> int = 1;\n");

    REQUIRE(source.lines().size() == 6);
    for (const auto& line : source.lines()) {
        CHECK(line.category == cppfrontier::source_category::cpp2);
    }
    REQUIRE(source.cpp2_sections().size() == 1);
    CHECK(source.cpp2_sections()[0].end_line_exclusive == 7);
}

TEST_CASE("comment markers inside raw strings do not change source state",
          "[source][model][comments][raw][upstream-442]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "text: std::string = R\"(/* // raw text */\n"
        "still raw)\";\n"
        "after: () -> int = 1;\n");

    REQUIRE(source.lines().size() == 3);
    CHECK(source.lines()[1].category == cppfrontier::source_category::raw_string);
    CHECK(source.lines()[2].category == cppfrontier::source_category::cpp2);
    REQUIRE(source.cpp2_sections().size() == 1);
    CHECK(source.cpp2_sections()[0].end_line_exclusive == 4);
}

TEST_CASE("block comments treat quotes and raw markers as comment text",
          "[source][model][comments]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "main: () = {\n"
        "    /* a quote \" and R\"( do not close this comment\n"
        "       } is comment text too */\n"
        "    value := 1;\n"
        "}\n");

    REQUIRE(source.lines().size() == 5);
    CHECK(source.lines()[1].category == cppfrontier::source_category::comment);
    CHECK(source.lines()[2].category == cppfrontier::source_category::comment);
    CHECK(source.lines()[3].category == cppfrontier::source_category::cpp2);
    CHECK(source.lines()[4].category == cppfrontier::source_category::cpp2);
    REQUIRE(source.cpp2_sections().size() == 1);
}

TEST_CASE("code after a block comment closing on a later line stays in the section",
          "[source][model][comments]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "main: () = {\n"
        "    /* comment\n"
        "    */ return 0; }\n");

    REQUIRE(source.lines().size() == 3);
    REQUIRE(source.cpp2_sections().size() == 1);
    CHECK(source.cpp2_sections()[0].end_line_exclusive == 4);
}

TEST_CASE("a trailing comment does not hide a following Cpp1 declaration",
          "[source][model][comments]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "value: int = 1; // trailing comment\n"
        "auto cpp1() -> int;\n");

    REQUIRE(source.lines().size() == 2);
    CHECK(source.lines()[0].category == cppfrontier::source_category::cpp2);
    CHECK(source.lines()[1].category == cppfrontier::source_category::cpp1);
    REQUIRE(source.cpp2_sections().size() == 1);
    CHECK(source.cpp2_sections()[0].end_line_exclusive == 2);
}

TEST_CASE("Cpp1 function braces do not become Cpp2 sections", "[source][model][cpp1]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "auto main() -> int\n{\n    return 0;\n}\n");

    REQUIRE(source.lines().size() == 4);
    for (const auto& line : source.lines()) {
        CHECK(line.category == cppfrontier::source_category::cpp1);
    }
    CHECK(source.cpp2_sections().empty());
}

TEST_CASE("a one-line raw string does not hide following Cpp2", "[source][model][raw]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "auto text = R\"(raw)\";\n"
        "main: () -> int = { return 0; }\n");

    REQUIRE(source.lines().size() == 2);
    CHECK(source.lines()[0].category != cppfrontier::source_category::raw_string);
    CHECK(source.lines()[1].category == cppfrontier::source_category::cpp2);
    CHECK_FALSE(source.cpp2_sections().empty());
}

TEST_CASE("multiline file-scope raw strings keep following Cpp2 visible",
          "[source][model][raw][upstream-1425]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "raw_text: std::string = R\"test(first (line)\n"
        "\n"
        "second (line))test\";\n"
        "\n"
        "main: () -> int = { return 0; }\n");

    REQUIRE(source.lines().size() == 5);
    CHECK(source.lines()[0].category == cppfrontier::source_category::cpp2);
    CHECK(source.lines()[4].category == cppfrontier::source_category::cpp2);
    REQUIRE(source.cpp2_sections().size() == 1);
    CHECK(source.cpp2_sections()[0].begin_line == 1);
    CHECK(source.cpp2_sections()[0].end_line_exclusive == 6);
}

TEST_CASE("top-level using statements begin a Cpp2 section", "[source][model][using]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "using std::string;\n"
        "main: () = { }\n"
        "auto cpp1() -> int;\n");

    REQUIRE(source.lines().size() == 3);
    CHECK(source.lines()[0].category == cppfrontier::source_category::cpp2);
    CHECK(source.lines()[1].category == cppfrontier::source_category::cpp2);
    CHECK(source.lines()[2].category == cppfrontier::source_category::cpp1);
}

TEST_CASE("preprocessor continuation lines stay in the directive", "[source][model][preprocessor]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "#define ADD(a, b) \\\n"
        "    ((a) + (b))\n"
        "#define TWO \\  \n"
        "x: int = 2;\n"
        "f: () -> int = ADD(1, 2);\n");
    const auto& lines = source.lines();

    REQUIRE(lines.size() == 5);
    for (auto index = 0; index < 4; ++index) {
        CHECK(lines[index].category == cppfrontier::source_category::preprocessor);
    }
    CHECK(lines[4].category == cppfrontier::source_category::cpp2);
}

TEST_CASE("Cpp2 does not begin inside Cpp1 braces", "[source][model][cpp1]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "int main() {\n"
        "    y: int = 2;\n"
        "loop:\n"
        "    using T = int;\n"
        "}\n"
        "int g(int a,\n"
        "      b: int);\n");

    for (const auto& line : source.lines()) {
        CHECK(line.category == cppfrontier::source_category::cpp1);
    }
    CHECK_FALSE(source.has_cpp2());
}

TEST_CASE("a Cpp2 section ends only outside parentheses and brackets",
          "[source][model][blocks]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "f: () -> int = (\n"
        "    :() -> int = { return 3; }\n"
        ")();\n"
        "v: std::array<int, 2> = [\n"
        "    1, 2];\n"
        "int g();\n");
    const auto sections = source.cpp2_sections();

    REQUIRE(sections.size() == 1);
    CHECK(sections[0].end_line_exclusive == 6);
    CHECK(source.lines()[5].category == cppfrontier::source_category::cpp1);
}

TEST_CASE("multiline raw strings with encoding prefixes", "[source][model][raw]") {
    for (const auto prefix : {"u8", "u", "U", "L"}) {
        const auto source = cppfrontier::source_text::from_bytes(
            std::string{"auto text = "} + prefix + "R\"(first \"\n"
            "x: int = 1;\n"
            ")\";\n"
            "y: int = 2;\n");
        const auto& lines = source.lines();

        REQUIRE(lines.size() == 4);
        CHECK(lines[1].category == cppfrontier::source_category::raw_string);
        CHECK(lines[2].category == cppfrontier::source_category::raw_string);
        CHECK(lines[3].category == cppfrontier::source_category::cpp2);
    }
}

TEST_CASE("a line splice continues a Cpp1 line or comment", "[source][model][review]") {
    const auto source = cppfrontier::source_text::from_bytes(
        "const char* s = \"abc\\\nmain: () = { }\";\n// note \\\nf: () = { }\ng: () = { }\n");
    const auto& lines = source.lines();
    REQUIRE(lines.size() == 5);
    CHECK(lines[1].category == cppfrontier::source_category::cpp1);
    CHECK(lines[3].category == cppfrontier::source_category::comment);
    CHECK(lines[4].category == cppfrontier::source_category::cpp2);
}

TEST_CASE("a block comment that a Cpp2 line opens stays in Cpp2", "[source][model][hunt]") {
    using cppfrontier::source_category;
    const auto source = cppfrontier::source_text::from_bytes(
        "d: () -> int = 8; /* open\n\n  still */ e: () -> int = 9;\nint g();\n"
        "int h(); /* one\ntwo */\n");
    const auto& lines = source.lines();
    REQUIRE(lines.size() == 6);
    for (auto index = std::size_t{0}; index < 3; ++index) {
        CAPTURE(index);
        CHECK(lines[index].category == source_category::cpp2);
    }
    CHECK(lines[3].category == source_category::cpp1);
    CHECK(lines[4].category == source_category::cpp1);
    CHECK(lines[5].category == source_category::comment);
}

TEST_CASE("a line that continues a Cpp1 expression does not begin Cpp2",
          "[source][model][hunt]") {
    using cppfrontier::source_category;
    const auto source = cppfrontier::source_text::from_bytes(
        "const int v = flag ?\n  a : b;\nint w = v +\n// note\n  c : d;\nf: () = { }\n");
    const auto& lines = source.lines();
    REQUIRE(lines.size() == 6);
    CHECK(lines[1].category == source_category::cpp1);
    CHECK(lines[4].category == source_category::cpp1);
    CHECK(lines[5].category == source_category::cpp2);
}

TEST_CASE("only a one-line using-declaration begins Cpp2", "[source][model][using][review]") {
    using cppfrontier::source_category;
    const auto source = cppfrontier::source_text::from_bytes(
        "using std::string;\n"
        "using ::std::_;\n"
        "using enum Color;\n"
        "using std::cout, std::endl;\n"
        "using Callback\n"
        "    = void(*)(int);\n"
        "using typename Base::type;\n"
        "using namespace std;\n");
    const auto& lines = source.lines();
    REQUIRE(lines.size() == 8);
    CHECK(lines[0].category == source_category::cpp2);
    CHECK(lines[1].category == source_category::cpp2);
    for (auto index = std::size_t{2}; index < lines.size(); ++index) {
        CAPTURE(index);
        CHECK(lines[index].category == source_category::cpp1);
    }
}

TEST_CASE("only the first branch of a conditional counts Cpp1 braces",
          "[source][model][preprocessor][review]") {
    using cppfrontier::source_category;
    const auto source = cppfrontier::source_text::from_bytes(
        "#if A\n"
        "int g(long x) {\n"
        "#elif B\n"
        "int g(short x) {\n"
        "#else\n"
        "#if C\n"
        "{\n"
        "#endif\n"
        "int g(int x) {\n"
        "#endif\n"
        "    return x;\n"
        "}\n"
        "f: () -> int = g(3);\n");
    const auto& lines = source.lines();
    REQUIRE(lines.size() == 13);
    CHECK_FALSE(lines[1].in_later_branch);
    CHECK(lines[3].in_later_branch);
    CHECK(lines[6].in_later_branch);
    CHECK(lines[8].in_later_branch);
    CHECK_FALSE(lines[10].in_later_branch);
    CHECK(lines[12].category == source_category::cpp2);
}

TEST_CASE("a line that begins inside a block comment does not begin Cpp2",
          "[source][model][comments][review]") {
    using cppfrontier::source_category;
    const auto source = cppfrontier::source_text::from_bytes(
        "/* block\n"
        "   between */ h: () -> int = 5;\n"
        "/* one line */ k: () -> int = 6;\n");
    const auto& lines = source.lines();
    REQUIRE(lines.size() == 3);
    CHECK(lines[1].category == source_category::cpp1);
    CHECK(lines[2].category == source_category::cpp2);
}

TEST_CASE("a leading UTF-8 byte order mark is skipped", "[source][model][review]") {
    const auto source = cppfrontier::source_text::from_bytes("\xEF\xBB\xBFmain: () = { }\n");
    REQUIRE(source.lines().size() == 1);
    CHECK(source.lines()[0].category == cppfrontier::source_category::cpp2);
    CHECK(source.lines()[0].text == "main: () = { }");
    CHECK(source.text_between({1, 1}, {1, 5}) == "main");
}
