#include <catch2/catch_all.hpp>
#include <cppfrontier/language_server.hpp>

TEST_CASE("language server reports symbols", "[language-server]") {
    cppfrontier::language_server server;
    auto symbols = server.open_document({"file:///hello.cpp2", 7, "hello: () = \"hello\";\n"});
    CHECK(symbols.version == 7);
    REQUIRE(symbols.symbols.size() == 1);
    CHECK(symbols.symbols[0].name == "hello");
    CHECK(symbols.symbols[0].kind == cppfrontier::declaration_kind::function);
    CHECK(symbols.symbols[0].id != cppfrontier::invalid_symbol);
    CHECK(server.diagnostics("file:///hello.cpp2", 7).empty());
}

TEST_CASE("language server resolves a call to its declaration", "[language-server]") {
    const auto source = std::string{
        "name: () -> int = { return 1; }\n"
        "main: () -> int = { return name(); }\n"};

    cppfrontier::language_server server;
    const auto symbols = server.open_document({"file:///definitions.cpp2", 3, source});
    REQUIRE(symbols.symbols.size() == 2);
    const auto& name = symbols.symbols[0];
    CHECK(name.name == "name");

    const auto definition = server.goto_definition("file:///definitions.cpp2", 3, {2, 28});
    CHECK(definition.found);
    CHECK(definition.span.begin.line == name.span.begin.line);
    CHECK(definition.span.begin.column == name.span.begin.column);
}

TEST_CASE("language server ignores stale document versions", "[language-server]") {
    cppfrontier::language_server server;
    server.open_document({"file:///stale.cpp2", 1, "first: () = { }\n"});
    const auto symbols = server.open_document({"file:///stale.cpp2", 2, "second: () = { }\n"});

    REQUIRE(symbols.symbols.size() == 1);
    CHECK(symbols.symbols[0].name == "second");
    CHECK(server.diagnostics("file:///stale.cpp2", 1).empty());
    CHECK_FALSE(server.goto_definition("file:///stale.cpp2", 1, {1, 1}).found);
}

TEST_CASE("language server does not treat declaration names as references", "[language-server][symbols]") {
    cppfrontier::language_server server;
    server.open_document({"file:///declaration.cpp2", 1, "name: () = \"hello\";\n"});

    CHECK_FALSE(server.goto_definition("file:///declaration.cpp2", 1, {1, 1}).found);
}

TEST_CASE("language server prefers a local declaration over a global name", "[language-server][symbols]") {
    const auto source = std::string{
        "value: () -> int = { return 1; }\n"
        "main: () -> int = {\n"
        "    value := 2;\n"
        "    return value;\n"
        "}\n"};
    cppfrontier::language_server server;
    const auto symbols = server.open_document({"file:///shadow.cpp2", 1, source});
    REQUIRE(symbols.symbols.size() == 3);

    const auto definition = server.goto_definition("file:///shadow.cpp2", 1, {4, 12});
    REQUIRE(definition.found);
    CHECK(definition.span.begin.line == 3);
}

TEST_CASE("language server resolves type names to their declarations", "[language-server][symbols]") {
    const auto source = std::string{
        "widget: type = { value: int = 0; }\n"
        "main: () -> int = { item: widget = (); return item.value; }\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///types.cpp2", 1, source});

    const auto type = server.goto_definition("file:///types.cpp2", 1, {2, 27});
    REQUIRE(type.found);
    CHECK(type.span.begin.line == 1);
    CHECK(type.span.begin.column == 1);
    const auto member = server.goto_definition("file:///types.cpp2", 1, {2, 52});
    REQUIRE(member.found);
    CHECK(member.span.begin.line == 1);
    CHECK(member.span.begin.column == 18);
}

TEST_CASE("language server resolves member access only to type members", "[language-server][symbols]") {
    const auto source = std::string{
        "N: namespace = { size: int = 1; }\n"
        "main: () -> int = { v: std::vector<int> = (); return v.size(); }\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///member-scope.cpp2", 1, source});

    CHECK_FALSE(server.goto_definition("file:///member-scope.cpp2", 1, {2, 56}).found);
}

TEST_CASE("language server resolves the innermost block declaration", "[language-server][symbols]") {
    const auto source = std::string{
        "main: () -> int = {\n"
        "    value := 1;\n"
        "    {\n"
        "        value := 2;\n"
        "        return value;\n"
        "    }\n"
        "}\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///nested.cpp2", 1, source});

    const auto definition = server.goto_definition("file:///nested.cpp2", 1, {5, 16});
    REQUIRE(definition.found);
    CHECK(definition.span.begin.line == 4);
}

TEST_CASE("language server resolves a parameter to its declaration", "[language-server][symbols]") {
    const auto source = std::string{
        "identity: (value: int) -> int = { return value; }\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///parameter.cpp2", 1, source});

    const auto definition = server.goto_definition("file:///parameter.cpp2", 1, {1, 43});
    REQUIRE(definition.found);
    CHECK(definition.span.begin.line == 1);
    CHECK(definition.span.begin.column == 12);
}

TEST_CASE("language server resolves locals in member functions", "[language-server][symbols][types]") {
    const auto source = std::string{
        "Widget: type = {\n"
        "    read: (this) -> int = {\n"
        "        value := 2;\n"
        "        return value;\n"
        "    }\n"
        "}\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///member.cpp2", 1, source});

    const auto definition = server.goto_definition("file:///member.cpp2", 1, {4, 16});
    REQUIRE(definition.found);
    CHECK(definition.span.begin.line == 3);
}

TEST_CASE("language server resolves value members after dot", "[language-server][symbols][types]") {
    const auto source = std::string{
        "Widget: type = {\n"
        "    value: int = 42;\n"
        "}\n"
        "main: () -> int = {\n"
        "    item: Widget = ();\n"
        "    return item.value;\n"
        "}\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///member-value.cpp2", 1, source});

    const auto definition = server.goto_definition("file:///member-value.cpp2", 1, {6, 17});
    REQUIRE(definition.found);
    CHECK(definition.span.begin.line == 2);
}

TEST_CASE("language server resolves a member-function parameter", "[language-server][symbols][types]") {
    const auto source = std::string{
        "Widget: type = {\n"
        "    add: (this, amount: int) -> int = {\n"
        "        return amount;\n"
        "    }\n"
        "}\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///member-parameter.cpp2", 1, source});

    const auto definition = server.goto_definition("file:///member-parameter.cpp2", 1, {3, 16});
    REQUIRE(definition.found);
    CHECK(definition.span.begin.line == 2);
    CHECK(definition.span.begin.column == 17);
}

TEST_CASE("language server resolves qualified names declared later", "[language-server][symbols][qualified]") {
    const auto source = std::string{
        "main: () -> int = { return tools::answer(); }\n"
        "tools: namespace = {\n"
        "    answer: () -> int = 42;\n"
        "}\n"};
    cppfrontier::language_server server;
    const auto symbols = server.open_document({"file:///qualified.cpp2", 1, source});
    REQUIRE(symbols.symbols.size() >= 3);

    const auto definition = server.goto_definition("file:///qualified.cpp2", 1, {1, 35});
    REQUIRE(definition.found);
    CHECK(definition.span.begin.line == 3);
}

TEST_CASE("language server resolves qualified type members", "[language-server][symbols][qualified][types]") {
    const auto source = std::string{
        "main: () -> int = { return Widget::answer(); }\n"
        "Widget: type = {\n"
        "    answer: () -> int = 42;\n"
        "}\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///qualified-type.cpp2", 1, source});

    const auto definition = server.goto_definition("file:///qualified-type.cpp2", 1, {1, 36});
    REQUIRE(definition.found);
    CHECK(definition.span.begin.line == 3);
}

TEST_CASE("language server resolves captures, statement and range parameters, and using",
          "[language-server][symbols][review]") {
    const auto source = std::string{
        "N: namespace = { f: (x: int) -> int = x; }\n"
        "T: type = {\n"
        "    public m: int = 1;\n"
        "    g: (this) -> int = {\n"
        "        l := :() -> int = m;\n"
        "        return l();\n"
        "    }\n"
        "}\n"
        "main: () -> int = {\n"
        "    x := 1;\n"
        "    l := :(x: int) -> int = x + x$;\n"
        "    do { x := 2; _ = x; } while x < 0;\n"
        "    (copy y := x) { _ = y; }\n"
        "    for (1, 2) do (e) { _ = e; }\n"
        "    using N::f;\n"
        "    return f(x) + l(x);\n"
        "}\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///probe.cpp2", 1, source});
    const auto at = [&](std::uint32_t line, std::uint32_t column) {
        const auto found = server.goto_definition("file:///probe.cpp2", 1, {line, column});
        if (!found.found) return std::string{"none"};
        return std::to_string(found.span.begin.line) + ":" +
            std::to_string(found.span.begin.column);
    };
    // A member through this in a function expression.
    CHECK(at(5, 27) == "3:5");
    // A function expression's parameter, and a capture of the local it hides.
    CHECK(at(11, 29) == "11:12");
    CHECK(at(11, 33) == "10:5");
    // A do condition is outside the body.
    CHECK(at(12, 33) == "10:5");
    // A statement parameter's initializer and the parameter.
    CHECK(at(13, 17) == "10:5");
    CHECK(at(13, 26) == "13:11");
    CHECK(at(14, 30) == "14:20");
    // A using-declaration names its target.
    CHECK(at(16, 12) == "1:18");
}

TEST_CASE("language server resolves a dereference to its pointer", "[language-server][symbols]") {
    const auto source = std::string{
        "main: () -> int = {\n"
        "    a := 1;\n"
        "    p := a&;\n"
        "    return p*;\n"
        "}\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///dereference.cpp2", 1, source});

    const auto definition = server.goto_definition("file:///dereference.cpp2", 1, {4, 12});
    CHECK(definition.found);
    CHECK(definition.span.begin.line == 3);
    CHECK(definition.span.begin.column == 5);
}

TEST_CASE("language server lists only the names the source declares", "[language-server][hunt]") {
    const auto source = std::string{
        "color: @enum type = { red; green; }\n"
        "point: @value type = { public x: int = 0; }\n"
        "u: @union type = { i: int; }\n"
        "main: () = {\n"
        "    (copy n := 1) while n > 0 next n-- { }\n"
        "    f := :(a: int) = a;\n"
        "}\n"};
    cppfrontier::language_server server;
    const auto symbols = server.open_document({"file:///declared.cpp2", 1, source});
    auto names = std::vector<std::string>{};
    for (const auto& symbol : symbols.symbols) names.push_back(symbol.name);
    CHECK(names == std::vector<std::string>{
        "color", "red", "green", "point", "x", "u", "i", "main", "n", "f", "a"});
}

TEST_CASE("language server resolves an enumerator to where it is written", "[language-server][hunt]") {
    const auto source = std::string{
        "color: @enum type = { red; green; }\n"
        "main: () = { c := color::green; }\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///enumerator.cpp2", 1, source});

    const auto definition = server.goto_definition("file:///enumerator.cpp2", 1, {2, 27});
    CHECK(definition.found);
    CHECK(definition.span.begin.line == 1);
    CHECK(definition.span.begin.column == 28);
}

TEST_CASE("language server resolves generated members to the source declarations",
          "[language-server][hunt]") {
    const auto source = std::string{
        "u: @union type = {\n"
        "    i: int;\n"
        "}\n"
        "main: () = {\n"
        "    x: u = ();\n"
        "    x.set_i(1);\n"
        "    _ = x.i();\n"
        "}\n"};
    cppfrontier::language_server server;
    server.open_document({"file:///generated.cpp2", 1, source});

    // `set_i` is generated in `u`; `i()` replaces the alternative `i`.
    auto definition = server.goto_definition("file:///generated.cpp2", 1, {6, 8});
    CHECK(definition.found);
    CHECK(definition.span.begin.line == 1);
    CHECK(definition.span.begin.column == 1);
    definition = server.goto_definition("file:///generated.cpp2", 1, {7, 11});
    CHECK(definition.found);
    CHECK(definition.span.begin.line == 2);
    CHECK(definition.span.begin.column == 5);
}
