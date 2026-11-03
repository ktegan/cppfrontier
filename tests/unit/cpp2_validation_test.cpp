#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <string>

namespace {

auto has_diagnostic(const cppfrontier::translation_result& result, std::string_view code) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}

auto translates(const std::string& source) -> cppfrontier::translation_result {
    return cppfrontier::translator{}.translate_source(source);
}

}

TEST_CASE("multi-word fundamental types are rejected", "[validation][types]") {
    const auto cases = std::vector<std::string>{
        "f: (value: unsigned char) = { }\n",
        "f: (value: long long) = { }\n",
        "f: (value: signed short int) = { }\n",
        "f: (value: long double) = { }\n",
    };
    for (const auto& source : cases) {
        CAPTURE(source);
        const auto result = translates(source);
        CHECK(has_diagnostic(result, "cpp1-multiword-type"));
        CHECK(result.has_errors());
        CHECK(result.cpp1.empty());
    }
}

TEST_CASE("single-word fundamental types remain valid", "[validation][types]") {
    for (const auto& type : {"char", "short", "int", "long", "double", "signed", "unsigned"}) {
        CAPTURE(type);
        const auto result = translates(std::string{"f: (value: "} + type + ") = { }\n");
        CHECK_FALSE(has_diagnostic(result, "cpp1-multiword-type"));
    }
}

TEST_CASE("prefix address-of is rejected but binary ampersand is not",
          "[validation][expressions][address-of]") {
    const auto prefix = translates("f: () = { value := 1; pointer := &value; }\n");
    INFO(prefix.cpp1);
    CHECK(has_diagnostic(prefix, "cpp1-prefix-address-of"));
    CHECK(prefix.has_errors());

    const auto binary = translates("f: (left: int, right: int) -> int = left & right;\n");
    INFO(binary.cpp1);
    CHECK_FALSE(has_diagnostic(binary, "cpp1-prefix-address-of"));
}

TEST_CASE("postfix address-of remains valid", "[validation][expressions][address-of]") {
    const auto result = translates("f: () = { value := 1; pointer := value&; }\n");
    INFO(result.cpp1);
    CHECK_FALSE(has_diagnostic(result, "cpp1-prefix-address-of"));
}

TEST_CASE("a postfix operator before a binary one is not a prefix operator",
          "[validation][expressions][address-of]") {
    for (const auto* source : {
             "f: (x: int, p: int) = { d := x& & p; }\n",
             "f: (x: u32) = { d := x~ & 0xFFu; }\n",
             "f: (p: *int) = { d := p* * 2; e := p* & 1; }\n",
             "f: () = { p: *int = nullptr; v: std::vector<*int> = (); "
             "m: std::map<int, *const int> = (); }\n"}) {
        CAPTURE(source);
        const auto result = translates(source);
        CHECK_FALSE(has_diagnostic(result, "cpp1-prefix-address-of"));
        CHECK_FALSE(has_diagnostic(result, "cpp1-prefix-dereference"));
    }
}

TEST_CASE("adjacent forms that are one operand are not juxtaposed",
          "[validation][expressions][juxtaposition]") {
    for (const auto* source : {
             "f: () -> std::string = \"a\" \"b\";\n",
             "f: () = { using std::operator\"\"sv; std::cout << \"x\"sv; }\n",
             "f: (x: std::any) -> bool = x.operator bool();\n",
             "f: (x: std::string) = { s := requires (i: int) { x + i; }; }\n",
             "W: @value @ordered <U> type = { public u: U = 2; }\n"}) {
        CAPTURE(source);
        const auto result = translates(source);
        CHECK_FALSE(has_diagnostic(result, "juxtaposed-operands"));
        CHECK_FALSE(has_diagnostic(result, "unexpected-metafunction"));
    }
}

TEST_CASE("a typed receiver is one error", "[validation][parameters]") {
    const auto result = translates("X: type = { operator=: (out this, that: X) = { } }\n");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].message ==
          "a 'this' or 'that' parameter knows its type; remove the ':'");
}

TEST_CASE("capture syntax is rejected explicitly rather than miscompiled",
          "[validation][captures]") {
    const auto result = translates("f: (value: int) -> int = value$;\n");
    INFO(result.cpp1);
    CHECK(has_diagnostic(result, "unsupported-capture"));
    CHECK(result.has_errors());
    CHECK(result.cpp1.empty());
}

TEST_CASE("the capture diagnostic names every place a capture may appear",
          "[validation][captures][review]") {
    const auto result = translates("f: (value: int) -> int = value$;\n");
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& value) {
        return value.code == "unsupported-capture" &&
            value.message == "$ captures are supported only in unnamed function expressions, "
                             "postconditions and string interpolations";
    }));
}

TEST_CASE("a postfix expression captures at most once", "[validation][captures]") {
    const auto result = translates(
        "S: type = { show: (this) -> int = 1; }\n"
        "main: () = { x: S = (); f := :() = x$.show$(); _ = f(); }\n");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "repeated-capture");
    CHECK(result.diagnostics.front().message ==
          "$ (capture) can appear at most once in a single postfix-expression");
    CHECK(result.diagnostics.front().span.begin.column == 43);
    CHECK_FALSE(translates("main: () = { v: std::vector<int> = (1); "
                           "f := :() = v$.size() + v$.size(); _ = f(); }\n").has_errors());
}

TEST_CASE("a postcondition with a contract group may capture",
          "[validation][captures][ports]") {
    const auto result = translates(
        "s: type = {\n"
        "    items: std::vector<int> = ();\n"
        "    report_violation: (this, m: * const char) = { }\n"
        "    push: (inout this, v: int)\n"
        "        post<this>(items.ssize() == items.ssize()$ + 1)\n"
        "        post<cpp2::testing, true>(items.ssize() > items.ssize()$)\n"
        "    = { items.push_back(v); }\n"
        "}\n");
    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("dollar signs inside string literals are not capture tokens",
          "[validation][captures][literals]") {
    const auto result = translates("f: () = { std::cout << \"$100\"; }\n");
    INFO(result.cpp1);
    CHECK_FALSE(has_diagnostic(result, "unsupported-capture"));
}

TEST_CASE("validation applies only to Cpp2 sections", "[validation][mixed]") {
    const auto result = translates(
        "int main() { int a = 1; int b = 2; return a & b; }\n"
        "#include <iostream>\n");
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("namespaces are rejected inside types but allowed inside namespaces",
          "[validation][namespaces][types]") {
    const auto inside_type = translates(
        "Widget: type = { nested: namespace = { value: int = 0; } }\n");
    INFO(inside_type.cpp1);
    CHECK(has_diagnostic(inside_type, "namespace-in-type"));
    CHECK(inside_type.has_errors());

    const auto inside_namespace = translates(
        "outer: namespace = { nested: namespace = { value: int = 0; } }\n");
    INFO(inside_namespace.cpp1);
    CHECK_FALSE(has_diagnostic(inside_namespace, "namespace-in-type"));
}

TEST_CASE("throw requires parentheses around its operand", "[validation][exceptions]") {
    const auto result = translates(
        "#include <exception>\n"
        "f: () = { throw 42; }\n");
    INFO(result.cpp1);
    CHECK(has_diagnostic(result, "throw-requires-parentheses"));
    CHECK(result.has_errors());
}

TEST_CASE("try requires a reachable catch and well-formed catch bodies",
          "[validation][exceptions]") {
    const auto missing_handler = translates("f: () = { try { throw(42); } }\n");
    CHECK(has_diagnostic(missing_handler, "missing-catch-handler"));

    const auto missing_parameter = translates(
        "f: () = { try { throw(42); } catch { return; } }\n");
    CHECK(has_diagnostic(missing_parameter, "expected-catch-parameters"));

    const auto missing_body = translates(
        "f: () = { try { throw(42); } catch (i: int) }\n");
    CHECK(has_diagnostic(missing_body, "expected-catch-body"));

    const auto typed_catch_all = translates(
        "f: () = { try { throw(42); } catch (_: int) { return; } }\n");
    CHECK(has_diagnostic(typed_catch_all, "invalid-catch-all"));

    const auto untyped_named_catch = translates(
        "f: () = { try { throw(42); } catch (error) { return; } }\n");
    CHECK(has_diagnostic(untyped_named_catch, "catch-parameter-requires-type"));
}

TEST_CASE("a function type written with ':' is diagnosed", "[validation][types][upstream-1310]") {
    CHECK(has_diagnostic(translates(
        "require_fn: type == std::function<:(copy b: bool, copy s: std::string) -> void>;\n"),
        "function-type-colon"));
    const auto plain = translates(
        "require_fn: type == std::function<(copy b: bool) -> void>;\n");
    INFO(plain.cpp1);
    CHECK_FALSE(has_diagnostic(plain, "function-type-colon"));
    CHECK(plain.cpp1.find("using require_fn = std::function<void(bool b)>;") != std::string::npos);
    CHECK_FALSE(has_diagnostic(translates("main: () = { x := f(1, :(y) = y); }\n"),
                               "function-type-colon"));
}

TEST_CASE("a typed value may end its type with '>='", "[validation][types][ports]") {
    const auto result = translates(
        "h: (v: std::vector<int>) = { }\n"
        "main: () = { h(:std::vector<int>=(1)); h((:std::vector<std::vector<int>>=())); }\n");
    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
    CHECK(has_diagnostic(translates("main: () = { h(:std::vector<int>); }\n"),
                         "typed-value-initializer"));
}

TEST_CASE("an inspect expression after '*' or '&' is a binary operand",
          "[validation][expressions][inspect][hunt]") {
    const auto result = translates(
        "main: () = {\n    x := 5;\n"
        "    std::cout << (2 * inspect x -> int { is 5 = 3; is _ = 2; });\n"
        "    y := 6 & inspect x -> int { is _ = 2; };\n}\n");
    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find("(2 * [&]") != std::string::npos);
    CHECK(result.cpp1.find("6 & [&]") != std::string::npos);
}

TEST_CASE("prefix increment is rejected but postfix increment is not",
          "[validation][expressions][review]") {
    CHECK(has_diagnostic(translates("main: () = { i := 0; ++i; }\n"), "cpp1-prefix-increment"));
    CHECK(has_diagnostic(translates("main: () = { i := 0; x := --i; }\n"), "cpp1-prefix-increment"));
    const auto postfix = translates("main: () = { i := 0; i++; j := i--; }\n");
    INFO(postfix.cpp1);
    CHECK_FALSE(postfix.has_errors());
}

TEST_CASE("pointer-to-member operators are rejected", "[validation][expressions][review]") {
    CHECK(has_diagnostic(translates(
        "V: type = { f: (this) -> int = 1; }\n"
        "main: () = { v: V = (); pm := V::f&; x := (v.*pm)(); }\n"),
        "pointer-to-member-operator"));
    const auto pointer_return = translates("f: (inout i: int) -> *int = i&;\n");
    INFO(pointer_return.cpp1);
    CHECK_FALSE(pointer_return.has_errors());
}

TEST_CASE("incomplete statements are rejected", "[validation][statements][review]") {
    CHECK(has_diagnostic(translates("main: () = { r := 0; r = ; }\n"), "incomplete-expression"));
    CHECK(has_diagnostic(translates("main: () = { r := 0; r += ; }\n"), "incomplete-expression"));
    CHECK(has_diagnostic(translates("main: () = { x := :int = 7;.neg(); }\n"),
                         "invalid-statement"));
    CHECK(has_diagnostic(translates("main: () = { (); }\n"), "invalid-statement"));
    CHECK(has_diagnostic(translates("main: () = { if true { } (); }\n"), "invalid-statement"));
    // `();` after an expression body's `;` invokes the function expression.
    CHECK_FALSE(translates("f: () -> int = { return :() -> int = 1; (); }\n").has_errors());
}

TEST_CASE("a contract condition cannot be a declaration", "[validation][contracts][review]") {
    CHECK(has_diagnostic(translates(
        "T: type = { f: (this) -> (r: int) post(r: r >= 0) = { r = 1; } }\n"),
        "contract-declaration"));
    const auto valid = translates(
        "f: (v: std::vector<int>) -> (r: int) pre<bounds_safety>(v.ssize() > 0, \"empty\") "
        "post(r >= 0) pre(std::all_of(v.begin(), v.end(), :(x) = x >= 0)) = { r = v[0]; }\n");
    for (const auto& diagnostic : valid.diagnostics) INFO(diagnostic.message);
    CHECK_FALSE(valid.has_errors());
}

TEST_CASE("a parenthesized for range is grouping unless it lists two or more elements",
          "[validation][syntax][for]") {
    const auto result = translates(
        "f: (v: std::vector<int>) = {\n"
        "    for (v) do (e) { _ = e; }\n"
        "    for (v,) do (e) { _ = e; }\n"
        "    for ((v)) do (e) { _ = e; }\n"
        "    for (0, 1, 2) do (e) { _ = e; }\n"
        "    for (0, 1, 2,) do (e) { _ = e; }\n"
        "    x := (1 + 2,) * 3;\n"
        "    _ = x;\n"
        "}\n");
    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find(": (v)) {") != std::string::npos);
    CHECK(result.cpp1.find(": { v }") == std::string::npos);
    CHECK(result.cpp1.find(": { v, }") == std::string::npos);
    CHECK(result.cpp1.find(": ((v))) {") != std::string::npos);
    CHECK(result.cpp1.find(": { 0, 1, 2 }) {") != std::string::npos);
    CHECK(result.cpp1.find(": { 0, 1, 2, }) {") != std::string::npos);
    CHECK(result.cpp1.find("auto x {(1 + 2) * 3};") != std::string::npos);
}

TEST_CASE("throw takes one parenthesized value, or none to rethrow", "[validation][exceptions]") {
    const auto rethrow = translates(
        "f: () = { try { throw(1); } catch (_) { throw; } }\n"
        "g: () = { throw(1,); }\n");
    INFO(rethrow.cpp1);
    CHECK_FALSE(rethrow.has_errors());
    CHECK(rethrow.cpp1.find("throw;") != std::string::npos);
    CHECK(rethrow.cpp1.find("throw(1);") != std::string::npos);

    for (const auto* source : {"f: () = { throw(); }\n", "f: () = { throw ( ); }\n",
                               "f: () = { throw(1, 2); }\n"}) {
        CAPTURE(source);
        const auto result = translates(source);
        CHECK(has_diagnostic(result, "invalid-throw"));
    }
}

TEST_CASE("conversion functions are diagnosed once wherever they are declared",
          "[validation][types]") {
    for (const auto* source : {
             "S: type = { operator int: (this) -> int = 1; }\n",
             "S: type = { public operator std::string: (this) -> std::string = { return \"\"; } }\n",
             "operator bool: () -> bool = true;\n"}) {
        CAPTURE(source);
        const auto result = translates(source);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().code == "conversion-function");
        CHECK(result.diagnostics.front().message.find("implicit out this") != std::string::npos);
    }
    // Calling a Cpp1 conversion function is an ordinary member call.
    CHECK_FALSE(translates("f: (x: std::any) -> bool = x.operator bool();\n").has_errors());
}

TEST_CASE("the order of defaulted parameters is left to the Cpp1 compiler",
          "[validation][templates][functions]") {
    for (const auto* source : {
             "f: <T = int, U> (x: U) = { }\n",
             "f: (a: int = 1, b: int) = { }\n",
             "f: <T = int, Ts...> (x...: Ts) = { }\n"}) {
        CAPTURE(source);
        CHECK_FALSE(translates(source).has_errors());
    }
}

TEST_CASE("template arguments use Cpp2 type syntax", "[validation][templates][types]") {
    const auto message = [](const std::string& source) {
        const auto result = translates(source);
        for (const auto& diagnostic : result.diagnostics) {
            if (diagnostic.code == "cpp1-declarator-in-template-argument") return diagnostic.message;
        }
        return std::string{};
    };
    CHECK(message("f: (p: *int) = { _ = unchecked_cast<Derived const&>(p); }\n") ==
          "'Derived const&' is a Cpp1 declarator; write the Cpp2 type 'const Derived'; "
          "Cpp2 types have no references");
    CHECK(message("v: std::vector<const char*> = ();\n") ==
          "'const char*' is a Cpp1 declarator; write the Cpp2 type '*const char'");
    CHECK(message("v: std::vector<int*> = ();\n") ==
          "'int*' is a Cpp1 declarator; write the Cpp2 type '*int'");
    CHECK(message("b: bool = std::is_same_v<char* const, int>;\n") ==
          "'char* const' is a Cpp1 declarator; write the Cpp2 type 'const *char'");
    CHECK(message("b: bool = std::is_same_v<std::string&&, int>;\n") ==
          "'std::string&&' is a Cpp1 declarator; write the Cpp2 type 'std::string'; "
          "Cpp2 types have no references");
    CHECK(message("v: std::vector<std::vector<int const>> = ();\n") ==
          "'int const' is a Cpp1 declarator; write the Cpp2 type 'const int'");

    // Cpp2 qualifiers, and postfix `*` and `&` expressions, are not declarators.
    for (const auto* source : {
             "v: std::vector<*const char> = ();\n",
             "v: std::vector<const * * const int> = ();\n",
             "v: std::vector<const int> = ();\n",
             "N: int == 2; a: std::array<int, N> = ();\n",
             "f: (p: *int) = { a: std::array<int, 1> = (); _ = a; _ = std::is_same_v<p*, int>; }\n",
             "f: (a: int, b: int) -> bool = a < b && b > a;\n"}) {
        CAPTURE(source);
        CHECK(message(source).empty());
    }
}

TEST_CASE("a template argument ending in a postfix star is rejected",
          "[validation][templates]") {
    const auto rejected = translates(
        "Foo: type = { }\n"
        "a: std::vector<Foo*> = ();\n"
        "b: std::map<int, std::string**> = ();\n");
    auto messages = std::vector<std::string>{};
    for (const auto& value : rejected.diagnostics) {
        if (value.code == "postfix-star-in-template-argument") messages.push_back(value.message);
    }
    CHECK(messages == std::vector<std::string>{
        "'Foo*' is a dereference in Cpp2; write '*Foo' for a pointer type, or '(Foo*)' for a "
        "dereference",
        "'std::string**' is a dereference in Cpp2; write '**std::string' for a pointer type, or "
        "'(std::string**)' for a dereference"});

    const auto accepted = translates(
        "Foo: type = { }\n"
        "n: int == 2;\n"
        "p: *const int == n&;\n"
        "a: std::vector<*Foo> = ();\n"
        "b: std::array<int, (p*)> = ();\n"
        "c: std::array<int, n * 2> = ();\n");
    CHECK_FALSE(accepted.has_errors());
}

TEST_CASE("requires-expression parameters use Cpp2 syntax", "[validation][requires]") {
    const auto result = translates(
        "a: <T> concept = requires (x: T, inout y: T, copy z: std::map<int, T>) { y += x; };\n"
        "b: <T> concept = std::integral<T> && requires (move x: T, f: (n: int) -> T) { f(x); };\n"
        "c: <T> (x: T) -> T requires requires (v: T) { v + v; } = x + x;\n"
        "d: <T> (x: T) -> T requires std::integral<T> && requires { x - x; } = x;\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    const auto has = [&](std::string_view text) {
        return result.cpp1.find(text) != std::string::npos;
    };
    CHECK(has("requires (T const& x, T& y, std::map<int, T> z) { y += x; }"));
    CHECK(has("std::integral<T> && requires (T&& x, T f(cpp2::impl::in<int> n)) { f(x); }"));
    CHECK(has("requires (requires (T const& v) { v + v; })"));
    CHECK(has("requires (std::integral<T> && requires { x - x; })"));

    CHECK(has_diagnostic(translates("c: <T> concept = requires (T x) { x + x; };\n"),
                         "cpp1-requires-parameter"));
}

TEST_CASE("a token outside every parsed expression is rejected",
          "[validation][expressions][review]") {
    for (const auto& source : {
             "f: (a: int) = { x := a + ; }\n",
             "f: () = { x := / 3; }\n",
             "g: (x: int) = { } f: () = { g(,); }\n",
             "f: () = { x := (1,,2); }\n",
             "f: (a: std::string) = { x := a.; }\n",
             "f: (a: int) = { x := a is; }\n",
             "f: () -> int = { return 1 +; }\n",
             "f: (a: int) pre(a > ) = { }\n",
             "f: (a: int) = { if a < { } }\n",
             "f: (a: int = 1 +) = { }\n",
             "f: <N: int = 1 +> () = { }\n",
             "f: (p: std::pair<int, int>) = { (a, b) := p; }\n",
         }) {
        CAPTURE(source);
        const auto result = translates(source);
        CHECK(has_diagnostic(result, "incomplete-expression"));
        CHECK(result.cpp1.empty());
    }
    const auto located = translates("f: () = { x := / 3; }\n");
    REQUIRE(located.diagnostics.size() == 1);
    CHECK(located.diagnostics.front().span.begin.column == 16);
    CHECK(located.diagnostics.front().message ==
          "'/' is not part of an expression - is an operand missing?");
    CHECK(has_diagnostic(translates("f: (a: int = 1 2) pre(a a) = { }\n"),
                         "juxtaposed-operands"));

    const auto valid = translates(
        "S: type = { size: int == 5; }\n"
        "f: (a: S) = { static_assert(decltype(a)::size == 5); x := decltype(a)::size; }\n"
        "g: () = { throw(1); }\n"
        "h: () = { type: int = 3; type = 4; }\n");
    INFO(valid.cpp1);
    CHECK_FALSE(valid.has_errors());
    CHECK(valid.cpp1.find("static_assert(decltype(a)::size == 5);") != std::string::npos);
}

TEST_CASE("a statement keyword is never an operand", "[validation][expressions][review]") {
    for (const auto& source : {
             "f: (a: bool) -> int = { return if a { 1; } }\n",
             "g: (x: int) = { } f: (a: bool) = { g(while a { }); }\n",
             "f: (a: bool) = { x: int = (if a { }); }\n",
             "f: (a: bool) = { x := 1 + if a { }; }\n",
             "f: () = { y := :(x) = if x > 0 { }; }\n",
         }) {
        CAPTURE(source);
        CHECK(has_diagnostic(translates(source), "statement-as-initializer"));
    }
}

TEST_CASE("a contract after the requires constraint is rejected",
          "[validation][contracts][review]") {
    const auto result = translates(
        "f: <T> (x: T) -> int requires std::is_integral_v<T> pre(x > 0) = x;\n");
    CHECK(has_diagnostic(result, "contract-after-requires"));
    CHECK(result.cpp1.empty());
    const auto valid = translates(
        "f: <T> (x: T) -> int pre(x > 0) requires std::is_integral_v<T> = x;\n");
    INFO(valid.cpp1);
    CHECK_FALSE(valid.has_errors());
}

TEST_CASE("string interpolations are checked as expressions",
          "[validation][expressions][review]") {
    CHECK(has_diagnostic(translates("f: (a: int) = { s := \"(a a)$\"; }\n"),
                         "juxtaposed-operands"));
    CHECK(has_diagnostic(translates("f: () = { s := \"(std::max({1, 2}))$\"; }\n"),
                         "braced-list"));
    const auto incomplete = translates("f: () = { s := \"x (1 +)$\"; }\n");
    REQUIRE(incomplete.diagnostics.size() == 1);
    CHECK(incomplete.diagnostics.front().code == "incomplete-expression");
    // At the `+`.
    CHECK(incomplete.diagnostics.front().span.begin.column == 22);
    const auto valid = translates("f: (a: int) = { s := \"(a)$ and (a + 1)$ (a:5)$\"; }\n");
    INFO(valid.cpp1);
    CHECK_FALSE(valid.has_errors());
}

TEST_CASE("an assignment is never used as a value", "[validation][expressions][review]") {
    for (const auto& source : {
             "f: (a: int) = { x: int = a = 3; }\n",
             "f: (inout a: int) = { x := a = 3; }\n",
             "f: (inout a: int, inout b: int) = { a = (b = 3); }\n",
             "g: (x: int) = { } f: (inout a: int) = { g(a = 3); }\n",
             "f: (inout a: int, inout b: int) = { a = b = 3; }\n",
             "f: (inout a: int) = { _ = a = 3; }\n",
             "f: (inout a: int) -> int = { return a = 3; }\n",
             "g: int = 1; x: int = g = 3;\n",
         }) {
        CAPTURE(source);
        CHECK(has_diagnostic(translates(source), "chained-assignment"));
    }
    const auto located = translates("f: (inout a: int) = { x := a = 3; }\n");
    REQUIRE(located.diagnostics.size() == 1);
    CHECK(located.diagnostics.front().span.begin.column == 30);
    const auto valid = translates(
        "f: (inout a: int) = { a = 3; (a += 2); while a < 9 next (a = a + 1) { } }\n"
        "g: (inout a: int) = a = 3;\n"
        "C: <T> concept = requires (inout a: T, b: T) { a = b; };\n");
    INFO(valid.cpp1);
    CHECK_FALSE(valid.has_errors());
    for (const auto& condition : {"if a = 3 { }", "if (a = 3) { }", "while ((a = 3)) { }"}) {
        CAPTURE(condition);
        CHECK(has_diagnostic(translates("f: (inout a: int) = { " + std::string{condition} + " }\n"),
                             "assignment-condition"));
    }
}

TEST_CASE("text the lexer cannot tokenize is reported once", "[validation][expressions][review]") {
    const auto result = translates("f: () = {\n    x := 1 ` 2;\n    z := 1 @ 2;\n}\n");
    // After a lexical error only the lexical errors are reported.
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "lexer");
    CHECK(has_diagnostic(translates("f: () = {\n    z := 1 @ 2;\n}\n"),
                         "unexpected-metafunction"));
}

TEST_CASE("a malformed member is skipped without leaving the type",
          "[validation][recovery][review]") {
    const auto result = translates(
        "D: type = {\n"
        "    g: (bad bad this) -> int = 3;\n"
        "    operator=: (out this, x: int) = { _ = x; }\n"
        "    k: (1) = { x := 1; }\n"
        "    h: (this) = { }\n"
        "}\n"
        "main: () = { }\n");
    REQUIRE(result.diagnostics.size() == 2);
    CHECK(result.diagnostics[0].span.begin.line == 2);
    CHECK(result.diagnostics[1].span.begin.line == 4);
}

TEST_CASE("an unclosed bracket is reported once, at the bracket",
          "[validation][recovery][review]") {
    const auto result = translates("f: () = {\n    x := g(1, 2;\n    y := 3;\n}\n");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "unmatched-open-parenthesis");
    CHECK(result.diagnostics.front().span.begin.line == 2);
    CHECK(result.diagnostics.front().span.begin.column == 11);
    const auto bracket = translates("f: () = { x := a[1; y := 3; }\n");
    REQUIRE(bracket.diagnostics.size() == 1);
    CHECK(bracket.diagnostics.front().code == "unmatched-open-bracket");
}

TEST_CASE("nesting that is too deep is the only diagnostic", "[validation][limits][review]") {
    auto source = std::string{"main: () = { x: "};
    for (auto index = 0; index < 3000; ++index) source += "std::vector<";
    source += "int";
    for (auto index = 0; index < 3000; ++index) source += ">";
    const auto result = translates(source + " = (); }\n");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "nesting-too-deep");
}

TEST_CASE("a requires expression is part of a condition", "[validation][conditions][review]") {
    const auto result = translates(
        "f: (x: _) -> int = {\n"
        "    if constexpr requires { x.size(); } { return 1; }\n"
        "    else if (requires (y: int) { x.at(y); }) { return 2; }\n"
        "    return 0;\n"
        "}\n");
    INFO(result.cpp1);
    CHECK(result.diagnostics.empty());
    CHECK(result.cpp1.find("if constexpr (requires { CPP2_UFCS_NONLOCAL(size)(x); }) {") !=
          std::string::npos);
    CHECK(result.cpp1.find("else if ((requires (int const& y) { CPP2_UFCS_NONLOCAL(at)(x, y); })) {") !=
          std::string::npos);
}

TEST_CASE("a function body follows '='", "[validation][functions][review]") {
    for (const auto* source : {"main: () = { _ = :(x) x + 1; }\n", "f: (x: int) x + 1;\n",
                               "t: type = { f: (x: int) x + 1; }\n"}) {
        CAPTURE(source);
        const auto result = translates(source);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().code == "missing-equal-before-body");
    }
    const auto braced = translates("main: () = { _ = :(x) { }; }\n");
    REQUIRE(braced.diagnostics.size() == 1);
    CHECK(braced.diagnostics.front().code == "missing-equal-before-body");
}

TEST_CASE("a local function is rejected by name", "[validation][functions][review]") {
    const auto result = translates("g: () = {\n    f: (x: int) -> int = x + 1;\n}\n");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "local-function");
    CHECK(result.diagnostics.front().message.find("'f := :(x) = x + 1;'") != std::string::npos);
}

TEST_CASE("an extra semicolon between declarations is rejected", "[validation][review]") {
    for (const auto* source : {"f := :(x: int) -> int = x + 1;;\n",
                               "t: type = { x: int = 1;; }\n"}) {
        CAPTURE(source);
        const auto result = translates(source);
        REQUIRE_FALSE(result.diagnostics.empty());
        CHECK(result.diagnostics.back().code == "empty-declaration");
    }
}

TEST_CASE("a thrown list is rejected", "[validation][throw][review]") {
    const auto result = translates("f: () = { throw((1, 2)); }\n");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "invalid-throw");
    CHECK_FALSE(translates("f: () = { throw((1)); throw(g(1, 2)); }\n").has_errors());
}

TEST_CASE("operators Cpp2 cannot declare are named", "[validation][operators][review]") {
    for (const auto* source : {
             "t: type = { operator new: (size: std::size_t) -> *void = nullptr; }\n",
             "operator new[]: (size: std::size_t) -> *void = nullptr;\n",
             "operator delete: (p: *void) = { }\n",
             "t: type = { operator co_await: (this) -> int = 1; }\n",
             "operator \"\" _x: (v: u64) -> u64 = v;\n",
             "operator\"\"_x: (v: u64) -> u64 = v;\n"}) {
        CAPTURE(source);
        const auto result = translates(source);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().code == "unsupported-operator");
    }
}

TEST_CASE("a type parameter's default uses Cpp2 type syntax", "[validation][templates][review]") {
    for (const auto* source : {"f: <T: type = int*> () = { }\n", "f: <T = int const> () = { }\n",
                               "f: <U, T = int&> () = { }\n"}) {
        CAPTURE(source);
        const auto result = translates(source);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().code == "cpp1-declarator-in-template-argument");
    }
    const auto cpp2 = translates("f: <T: type = *int> () = { }\ng: <U, T = *const int> () = { }\n");
    CHECK(cpp2.diagnostics.empty());
    CHECK(cpp2.cpp1.find("template<typename U, typename T = int const*> auto g() -> void;") !=
          std::string::npos);
}

TEST_CASE("a nested template argument closed by '>>' is checked once, as itself",
          "[validation][templates][hunt]") {
    for (const auto* source : {"v: std::vector<std::vector<int*>> = ();\n",
                               "v: std::vector<std::pair<int, char*>> = ();\n",
                               "v: std::vector<std::vector<std::vector<int*>>> = ();\n"}) {
        CAPTURE(source);
        const auto result = translates(source);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().code == "cpp1-declarator-in-template-argument");
        CHECK(result.diagnostics.front().message.starts_with("'" +
            std::string{result.diagnostics.front().message.find("char") != std::string::npos
                ? "char*" : "int*"} + "'"));
    }
}

namespace {

// Whether `source` is rejected with one diagnostic, `code` at `line`:`column`.
auto rejects_once(const std::string& source, std::string_view code, std::uint32_t line,
                  std::uint32_t column) -> bool {
    const auto result = translates(source);
    for (const auto& issue : result.diagnostics) {
        UNSCOPED_INFO(issue.span.begin.line << ':' << issue.span.begin.column << ' '
                      << issue.code << ": " << issue.message);
    }
    return result.diagnostics.size() == 1 && result.diagnostics.front().code == code &&
        result.diagnostics.front().span.begin.line == line &&
        result.diagnostics.front().span.begin.column == column;
}

}

TEST_CASE("a local with an empty initializer is reported once, at its '=' or ':='",
          "[validation][recovery][hunt]") {
    CHECK(rejects_once("main: () = {\n    x: int = ;\n    y := x;\n}\n",
                       "missing-initializer", 2, 12));
    CHECK(rejects_once("main: () = {\n    x := ;\n    y := x;\n}\n",
                       "missing-initializer", 2, 7));
}

TEST_CASE("an object with an empty initializer is reported once, at the '=', in every scope",
          "[validation][recovery][hunt]") {
    CHECK(rejects_once("c: int = ;\nmain: () = { }\n", "missing-initializer", 1, 8));
    CHECK(rejects_once("n: namespace = {\n    b: int = ;\n    c: int = 1;\n}\n",
                       "missing-initializer", 2, 12));
    CHECK(rejects_once("t: type = {\n    b: int = ;\n    c: int = 1;\n}\n",
                       "missing-initializer", 2, 12));
    CHECK(rejects_once("e: @enum type = {\n    a;\n    b := ;\n    c := 2;\n}\n",
                       "missing-initializer", 3, 8));
}

TEST_CASE("a parenthesis left open in an expression is reported at it",
          "[validation][recovery][hunt]") {
    CHECK(rejects_once("main: () = {\n    x := 1;\n    if (x > 0 {\n        x = 2;\n    }\n}\n",
                       "unmatched-open-parenthesis", 3, 8));
    CHECK(rejects_once("main: () = {\n    v: std::vector<int> = ();\n    x := v[(1 + 2];\n}\n",
                       "unmatched-open-parenthesis", 3, 12));
    // A closed group holds the error.
    CHECK(rejects_once("main: () = {\n    x := (1 + );\n}\n", "incomplete-expression", 2, 13));
}

TEST_CASE("after a lexical error only the lexical errors are reported",
          "[validation][recovery][hunt]") {
    CHECK(rejects_once("main: () = {\n    s := \"hello;\n    std::cout << s;\n}\n", "lexer", 2, 10));
    CHECK(rejects_once("f: () = {\n    x: int;\n    std::cout << x;\n}\n"
                       "g: () = {\n    s := \"abc;\n}\n", "lexer", 6, 10));
    CHECK(rejects_once("main: () = {\n    r := R\"x(text)y\";\n    std::cout << r;\n}\n",
                       "lexer", 2, 10));
}

TEST_CASE("a block comment left open in a declaration is reported",
          "[validation][recovery][hunt]") {
    const auto result = translates("main: () = {\n    /* open\n    x := 1;\n}\n");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().message == "block comment is missing its closing */");
    CHECK(result.diagnostics.front().span.begin.line == 2);
}

TEST_CASE("a statement reports its first error only", "[validation][recovery][hunt]") {
    CHECK(rejects_once("main: () = {\n    throw 1;\n}\n", "throw-requires-parentheses", 2, 5));
    CHECK(rejects_once("main: () = {\n    a := 1;\n    p := a&;\n    x := a.*p;\n}\n",
                       "pointer-to-member-operator", 4, 11));
    // Statements on one line are counted apart.
    const auto two = translates("main: () = { throw 1; throw 2; }\n");
    CHECK(two.diagnostics.size() == 2);
}

TEST_CASE("a type operator without its operand is reported once",
          "[validation][recovery][hunt]") {
    auto options = cppfrontier::translation_options{};
    REQUIRE(options.experiments.enable("type-operators"));
    const auto result = cppfrontier::translator{}.translate_source("f: (x: int | ) = {}\n", options);
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "type-operator-single");
}

TEST_CASE("an inspect alternative without its ';' is reported once",
          "[validation][recovery][hunt]") {
    const auto source = [](std::string_view alternatives) {
        return "main: () = {\n    x := 1;\n    r := inspect x -> int {\n" +
            std::string{alternatives} + "    };\n    std::cout << r;\n}\n";
    };
    CHECK(rejects_once(source("        is 1 = 10\n        is _ = 20;\n"), "invalid-inspect", 4, 16));
    CHECK(rejects_once(source("        is 1 = 10;\n        is _ = 20\n"), "invalid-inspect", 5, 16));
    CHECK(rejects_once(source("        is 1 = x as int\n        is _ = 20;\n"), "invalid-inspect",
                       4, 21));
    CHECK(translates(source("        is 1 = x as int;\n        is _ = 20;\n")).diagnostics.empty());
}

TEST_CASE("a contract left open is reported at its '('", "[validation][recovery][hunt]") {
    CHECK(rejects_once("f: (x: int) -> int\n    pre(x > 0\n= x;\nmain: () = { }\n",
                       "unmatched-open-parenthesis", 2, 8));
    CHECK(rejects_once("main: () = {\n    x := 1;\n    assert(x > 0;\n    y := 2;\n}\n",
                       "unmatched-open-parenthesis", 3, 11));
}

TEST_CASE("an invalid parameter is reported at it", "[validation][recovery][hunt]") {
    CHECK(rejects_once("f: (in x: int, inout) = { }\n", "invalid-parameter-declaration", 1, 16));
    CHECK(rejects_once("f: (a, , b) = { }\n", "invalid-parameter-declaration", 1, 8));
}

TEST_CASE("a declared name is not qualified", "[validation][recovery][hunt]") {
    CHECK(rejects_once("main: () = {\n    std::x := 1;\n}\n", "qualified-declaration-name", 2, 5));
    CHECK(rejects_once("T: type = {\n    a::b: int = 0;\n}\n", "qualified-declaration-name", 2, 5));
    CHECK(rejects_once("f: (std::x: int) = { }\n", "qualified-declaration-name", 1, 5));
    CHECK(rejects_once("f: (x::y) = { }\n", "invalid-parameter-declaration", 1, 5));
    CHECK(rejects_once("main: () = {\n    for (1, 2) do (a::b) { }\n}\n",
                       "invalid-for-parameter", 2, 19));
}

TEST_CASE("a '{' after a declared type is missing its '='", "[validation][recovery][hunt]") {
    CHECK(rejects_once("f: (x: int) -> int {\n    return x;\n}\n", "missing-equal-before-body", 1, 20));
    CHECK(rejects_once("x: int { 1 };\n", "missing-equal-before-body", 1, 8));
    CHECK(rejects_once("f: () = {\n    y: int { 2 };\n}\n", "invalid-declaration-initializer", 2, 12));
}

TEST_CASE("a Cpp1 local declaration is named as one", "[validation][recovery][hunt]") {
    const auto message = [](std::string_view statement) {
        const auto result = translates("main: () = {\n    " + std::string{statement} + "\n}\n");
        return result.diagnostics.size() == 1 ? result.diagnostics.front().message : "";
    };
    CHECK(message("int y = 3;") == "to define a variable y of type int, write 'y: int = /* initializer */'");
    CHECK(message("int y;") == "to define a variable y of type int, write 'y: int = /* initializer */'");
    CHECK(message("std::string s(\"a\");") ==
          "to define a variable s of type std::string, write 's: std::string = /* initializer */'");
    CHECK(message("auto y = 3;") ==
          "to define a variable y with deduced type, write 'y := /* initializer */'");
    CHECK(message("retrun y;") == "invalid statement expression");
}

TEST_CASE("a template parameter is a name before its ':' or '='", "[validation][recovery][hunt]") {
    CHECK(rejects_once("f: <T type> () = { }\n", "invalid-template-parameter", 1, 7));
    CHECK(rejects_once("f: <(T)> () = { }\n", "invalid-template-parameter", 1, 5));
    CHECK(rejects_once("f: <> () = { }\nmain: () = { }\n", "empty-template-parameter-list", 1, 4));
    CHECK(translates("f: <Ts...: type, N: int = 3, U: std::vector<int> = std::vector<int>> () = { }\n"
                     "main: () = { }\n").diagnostics.empty());
}

TEST_CASE("a function has one parameter list", "[validation][recovery][hunt]") {
    CHECK(rejects_once("f: (x) () = { }\n", "missing-equal-before-body", 1, 8));
    CHECK(rejects_once("f: () -> int (x) = 1;\n", "missing-equal-before-body", 1, 14));
    CHECK(rejects_once("T: type = {\n    f: (this) (x) = { }\n}\n", "missing-equal-before-body", 2, 15));
}

TEST_CASE("a '>>' closes only the lists its type opens", "[validation][recovery][hunt]") {
    CHECK(rejects_once("v: std::vector<int>> = ();\n", "unexpected-type-token", 1, 19));
    CHECK(rejects_once("f: (x: std::map<int, int>>) = { }\n", "unexpected-type-token", 1, 25));
    CHECK(translates("v: std::vector<std::vector<int>> = ();\nmain: () = { }\n").diagnostics.empty());
}

TEST_CASE("a stray ')' does not end a declaration's braces", "[validation][recovery][hunt]") {
    const auto result = translates(
        "f: () = {\n    x := 1;\n    _ = x is decltype.x);\n}\nmain: () = { }\n");
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.front().span.begin.line == 3);
    CHECK(std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& value) {
        return value.code == "unmatched-close-brace" || value.code == "unterminated-block";
    }));
}

TEST_CASE("a condition and a next clause are not empty", "[validation][recovery][hunt]") {
    CHECK(rejects_once("f: () = {\n    if { }\n}\n", "missing-condition", 2, 8));
    CHECK(rejects_once("f: () = {\n    x := true;\n    if x { } else if { }\n}\n",
                       "missing-condition", 3, 22));
    CHECK(rejects_once("f: () = {\n    while { }\n}\n", "missing-condition", 2, 11));
    CHECK(rejects_once("f: () = {\n    do { } while;\n}\n", "missing-condition", 2, 17));
    CHECK(rejects_once("f: () = {\n    x := true;\n    while x next { }\n}\n",
                       "missing-next-expression", 3, 13));
    CHECK(rejects_once("f: () = {\n    x := true;\n    do { } next while x;\n}\n",
                       "missing-next-expression", 3, 12));
}

TEST_CASE("a for loop has a range", "[validation][recovery][hunt]") {
    CHECK(rejects_once("f: () = {\n    for do (i) { }\n}\n", "missing-range", 2, 9));
}

TEST_CASE("a function declaration without a body ends with ';'", "[validation][recovery][hunt]") {
    CHECK(rejects_once("I: @interface type = {\n    f: (this) -> int\n}\n", "missing-semicolon", 2, 18));
    CHECK(rejects_once("f: () -> int\nmain: () = { }\n", "unexpected-type-token", 2, 1));
    CHECK(rejects_once("f: (x: int) x;\n", "missing-equal-before-body", 1, 13));
}

TEST_CASE("generated code is not reported invalid after a syntax error",
          "[validation][recovery][hunt]") {
    CHECK(rejects_once("U: @union type = {\n    a: int;\n    b: ;\n}\nmain: () = { }\n",
                       "type-scope-object-requires-type", 3, 5));
}

TEST_CASE("a member declaration declares one name", "[validation][recovery][hunt]") {
    CHECK(rejects_once("T: type = {\n    a, b: int;\n    c: int = 0;\n}\n", "top-level-comma", 2, 6));
    CHECK(rejects_once("E: @enum type = {\n    a, b;\n}\n", "top-level-comma", 2, 6));
}

TEST_CASE("a metafunction on an object is reported once", "[validation][recovery][hunt]") {
    CHECK(rejects_once("x: @copy_constructible int = 0;\n", "metafunction-requires-type", 1, 1));
}

TEST_CASE("a parameter's '=' is followed by its default", "[validation][recovery][hunt]") {
    CHECK(rejects_once("f: (x: int = ) = { }\n", "empty-default-argument", 1, 12));
    CHECK(rejects_once("f: (x := ) = { }\n", "empty-default-argument", 1, 7));
    CHECK(rejects_once("f: () = {\n    g := :(x: int = ) = x;\n}\n", "empty-default-argument", 2, 19));
}

TEST_CASE("a declaration with a syntax error gets no flow diagnostics",
          "[validation][recovery][hunt]") {
    CHECK(rejects_once("f: (out x: int) = {\n    x = ;\n}\n", "incomplete-expression", 2, 7));
    CHECK(rejects_once("f: (c: bool) = {\n    x: int;\n    if c x = 1;\n    _ = x;\n}\n",
                       "unbraced-body", 3, 10));
}

TEST_CASE("a 'do' body without 'while' is reported once, after the body",
          "[validation][recovery][hunt]") {
    CHECK(rejects_once("main: () = {\n    do { }\n}\n", "expected-do-while", 3, 1));
    CHECK(rejects_once("main: () = {\n    x := 1;\n    do { } x = 2;\n}\n",
                       "expected-do-while", 3, 12));
}

TEST_CASE("a statement parameter's initializer is checked as an expression",
          "[validation][recovery][hunt]") {
    CHECK(rejects_once("main: () = {\n    (x: int = { return 1; }) { }\n}\n",
                       "object-initializer-not-expression", 2, 6));
    CHECK(rejects_once("main: () = {\n    (x: int = { 1 }) { }\n}\n", "braced-list", 2, 15));
    CHECK(rejects_once("main: () = {\n    a := 1;\n    (x := a a) { }\n}\n",
                       "juxtaposed-operands", 3, 13));
    CHECK_FALSE(cppfrontier::translator{}.translate_source(
        "main: () = {\n    (copy v: std::vector<int> = (1, 2, 3)) v.push_back(4);\n}\n")
        .has_errors());
}

TEST_CASE("a typed value after a complete operand is juxtaposed, at its ':'",
          "[validation][expressions][hunt]") {
    CHECK(rejects_once("main: () = {\n    a := 1;\n    b := a~ :int = 6;\n}\n",
                       "juxtaposed-operands", 3, 13));
    CHECK(rejects_once("main: () = {\n    a := 1;\n    b := a++ :int = 6;\n}\n",
                       "juxtaposed-operands", 3, 14));
    CHECK(rejects_once("main: () = {\n    a := 1;\n    _ = a~ :int = 6;\n}\n",
                       "juxtaposed-operands", 3, 12));
    CHECK(rejects_once("main: () = {\n    a := 1;\n    b := (a) :int = 6;\n}\n",
                       "incomplete-expression", 3, 14));
    // A `*` in a type is no operator before a function expression.
    CHECK(rejects_once("f: (i: int) -> int = i;\nmain: () = {\n    p: * :(i: int) -> int = f&;\n}\n",
                       "unexpected-type-token", 3, 10));
}

TEST_CASE("an expression problem after a joined token is reported at its token",
          "[validation][expressions][hunt]") {
    // The lexer reads `>=` and `>>` as one token, the expression parser as two.
    CHECK(rejects_once("f: (a: bool, b: int) = { }\nmain: () = {\n    a := 1;\n    c := 2;\n"
                       "    f(a >= 1, c = 3);\n}\n", "chained-assignment", 5, 17));
    CHECK(rejects_once("main: () = {\n    a := 1;\n    x: bool = a >= 1 a;\n}\n",
                       "juxtaposed-operands", 3, 22));
    CHECK(rejects_once("main: () = {\n    a := 1;\n    static x: int = a >> 1 a;\n}\n",
                       "juxtaposed-operands", 3, 28));
    CHECK(rejects_once("main: () = {\n    a := 1;\n    x := 0..<a + (a >= 3) a;\n}\n",
                       "juxtaposed-operands", 3, 27));
}

TEST_CASE("a problem in a parenthesized operand is reported inside it",
          "[validation][expressions][hunt]") {
    const auto in_body = [](const std::string& statement) {
        return "main: () = {\n    x := 1;\n    " + statement + "\n}\n";
    };
    CHECK(rejects_once(in_body("y := 1 + (x +);"), "incomplete-expression", 3, 17));
    CHECK(rejects_once(in_body("y := x + (2 * (x +));"), "incomplete-expression", 3, 22));
    CHECK(rejects_once(in_body("std::cout << (sizeof(x +));"), "incomplete-expression",
                       3, 28));
    CHECK(rejects_once(in_body("f(1 << (x +));"), "incomplete-expression", 3, 15));
    CHECK(rejects_once(in_body("y := x + [1];"), "incomplete-expression", 3, 12));
}

TEST_CASE("a problem in an interpolation is reported at its token",
          "[validation][interpolation][hunt]") {
    const auto in_body = [](const std::string& statement) {
        return "main: () = {\n    x := true;\n    y := 2;\n    " + statement + "\n}\n";
    };
    CHECK(rejects_once(in_body("s := \"a (x y)$\";"), "juxtaposed-operands", 4, 16));
    CHECK(rejects_once(in_body("s := \"a \\\"q\\\" (x \\\"z\\\" y)$\";"),
                       "juxtaposed-operands", 4, 22));
    CHECK(rejects_once(in_body("s := $R\"(a\n b (x y)$)\";"), "juxtaposed-operands", 5, 7));
    CHECK(rejects_once(in_body("s := \"(f(\\\"(x y)$\\\"))$\";"), "juxtaposed-operands", 4,
                       19));
    CHECK(rejects_once(in_body("s := \"(y * -1)$\";"), "misleading-postfix-spacing", 4, 14));
    // After a prefix operator, also `not`.
    CHECK(rejects_once(in_body("s := \"(not x y)$\";"), "juxtaposed-operands", 4, 18));
    CHECK(rejects_once(in_body("s := \"(-y x)$\";"), "juxtaposed-operands", 4, 15));
    CHECK_FALSE(translates(in_body("s := \"(not x)$ (-y)$ (!x)$\";")).has_errors());
    // A literal whose capture a function expression renames is reported at its start.
    CHECK(rejects_once(in_body("f := :() -> std::string = \"(y$ y)$ (y y)$\";"),
                       "juxtaposed-operands", 4, 31));
}

TEST_CASE("a diagnostic quotes a capture as written", "[validation][captures][hunt]") {
    const auto message = [](const std::string& body) {
        const auto result = translates("main: () = {\n    x := 1;\n    " + body + "\n}\n");
        REQUIRE(result.diagnostics.size() == 1);
        return result.diagnostics.front().message;
    };
    CHECK(message("f := :() x$;") == "expected '=' before the function body 'x$'");
    CHECK(message("f := :() = { return x$ (x + 1)$; };") ==
          "unexpected text '(x + 1)$' after an expression - is an operator or ';' missing?");
}

TEST_CASE("a value template argument of a type is checked as an expression",
          "[validation][expressions][hunt]") {
    const auto in_type = [](const std::string& argument) {
        return "N: int == 3;\nmain: () = {\n    x: std::array<int, " + argument +
            "> = ();\n}\n";
    };
    CHECK(rejects_once(in_type("N * -1 + 5"), "misleading-postfix-spacing", 3, 26));
    CHECK(rejects_once(in_type("N& + 1"), "pointer-arithmetic", 3, 27));
    CHECK(rejects_once(in_type("(1, 2)"), "comma-operator", 3, 24));
    CHECK(rejects_once("N: int == 3;\nA: type == std::array<int, N * -1>;\n",
                       "misleading-postfix-spacing", 2, 30));
    CHECK(rejects_once("N: int == 3;\nf: <T: type = std::array<int, N * -1>> () = { }\n",
                       "misleading-postfix-spacing", 2, 33));
    for (const auto* argument : {"N * 2", "(N > 2) + 1", "std::max(N, 4)", "N", ":i32 = 3",
                                 "sizeof(int)"}) {
        INFO(argument);
        CHECK_FALSE(translates(in_type(argument)).has_errors());
    }
}

TEST_CASE("a postfix * or & spaced from its operand cannot precede a prefix-looking operator",
          "[validation][expressions][pointers]") {
    const auto code = std::string{"misleading-postfix-spacing"};
    const auto in_body = [](const std::string& statement) {
        return translates("f: (copy a: int, p: *int, q: **int) = {\n    " + statement + "\n}\n");
    };
    for (const auto* statement : {
             "x := a * -1;", "x := a * +1;", "x := a *-1;", "x := a * - 1;", "x := p * + +1;",
             "x := p * - -1;", "x := (p) * -1;", "x := (p*) * -1;", "x := f(p * -1);",
             "x := q * ** -1;", "x := p* * -1;", "x := a * *p;", "x := a * &a;", "x := a & &a;",
             "x := a & -a;", "x := a * ++a;", "x := a * ~a;", "x := \"(a * -1)$\";",
             "x := a /*c*/ * -1;", "a = a * -1;"}) {
        INFO(statement);
        CHECK(has_diagnostic(in_body(statement), code));
    }
    for (const auto* statement : {
             "x := a*-1;", "x := p* -1;", "x := p* - 1;", "x := p* - -1;", "x := p* + +1;",
             "x := a * (-1);", "x := a * (- -1);", "x := (p *) - 1;", "x := q** - 1;",
             "x := p* * a;", "x := a & (-a);", "x := a && -a;", "x := a * !a;", "a *= -1;",
             "x := p *;", "f(p *, 1);"}) {
        INFO(statement);
        CHECK_FALSE(has_diagnostic(in_body(statement), code));
    }
    // The first spaced operator of a run, also inside a joined `p*`.
    const auto run = in_body("x := q * ** -1;");
    REQUIRE_FALSE(run.diagnostics.empty());
    CHECK(run.diagnostics.front().span.begin.column == 12);
    const auto joined = in_body("x := p* * -1;");
    REQUIRE_FALSE(joined.diagnostics.empty());
    CHECK(joined.diagnostics.front().span.begin.column == 13);
    // The hint fits the token after the operator.
    const auto message = [&](const std::string& statement) {
        const auto result = in_body(statement);
        REQUIRE(result.diagnostics.size() == 1);
        return result.diagnostics.front().message;
    };
    const auto additive = [](const std::string& op, const std::string& meaning) {
        return "this '" + op + "' is a postfix " + meaning + " of the operand before it, not a "
            "binary '" + op + "' - to " + (op == "*" ? "multiply" : "take a bitwise and") +
            ", parenthesize the right operand, as 'a " + op + " (-b)'; otherwise remove the "
            "space before '" + op + "'";
    };
    const auto prefix = [](const std::string& op, const std::string& meaning) {
        return "this '" + op + "' is a postfix " + meaning + " of the operand before it, not a "
            "binary '" + op + "' - remove the space before '" + op + "'; to " +
            (op == "*" ? "multiply" : "take a bitwise and") + ", write the right operand with "
            "postfix operators, as 'a " + op + " p*'";
    };
    CHECK(message("x := a * -1;") == additive("*", "dereference"));
    CHECK(message("x := a & +a;") == additive("&", "address-of"));
    for (const auto* statement : {"x := a * *p;", "x := a * &a;", "x := a * ~a;",
                                  "x := a * ++a;", "x := a * --a;", "x := q * ** -1;",
                                  "x := \"(a * *p)$\";"}) {
        INFO(statement);
        CHECK(message(statement) == prefix("*", "dereference"));
    }
    CHECK(message("x := a & &a;") == prefix("&", "address-of"));
    // Before `...`, a `*` or `&` is postfix, so a spaced one does not fold.
    const auto fold = [](const std::string& body) {
        return translates("f: (xs...: _) = {\n    " + body + "\n}\n");
    };
    for (const auto* body : {"_ = (xs * ...);", "_ = (1 * ... * xs);", "_ = (xs & ...);",
                             "_ = (xs * ... * 1);"}) {
        INFO(body);
        CHECK(has_diagnostic(fold(body), code));
    }
    for (const auto* body : {"_ = (... * xs);", "_ = (xs* + ...);", "g(xs*...);",
                             "r := 1; (r *= ... *= xs);"}) {
        INFO(body);
        CHECK_FALSE(fold(body).has_errors());
    }
    CHECK(fold("_ = (xs * ...);").diagnostics.front().message ==
          "this '*' is a postfix dereference of the operand before it, not a binary '*' - "
          "remove the space before '*'; for a fold over '*', write '(... * xs)'");
}

TEST_CASE("arithmetic on an address is pointer arithmetic", "[validation][pointers]") {
    const auto in_body = [](const std::string& statement) {
        return translates("f: (copy a: int, p: *int) = {\n    " + statement + "\n}\n");
    };
    for (const auto* statement : {"x := a& + 1;", "x := a& - 1;", "x := 1 + a&;", "x := (a&) + 1;",
                                  "x := a&[1];", "x := (a&)[1];", "x := a&++;", "x := a& - p;",
                                  "x := \"(a& + 1)$\";"}) {
        INFO(statement);
        CHECK(has_diagnostic(in_body(statement), "pointer-arithmetic"));
    }
    for (const auto* statement : {"x := a&;", "x := a& == p;", "x := (a&)*;", "x := a & 1;"}) {
        INFO(statement);
        CHECK_FALSE(in_body(statement).has_errors());
    }
}

TEST_CASE("a fundamental type is not an operand", "[validation][types]") {
    const auto in_body = [](const std::string& statement) {
        return translates("f: (v) = {\n    " + statement + "\n}\n");
    };
    CHECK(has_diagnostic(in_body("x := v is int | double;"), "type-after-is"));
    CHECK(has_diagnostic(in_body("x := v is std::string | i32;"), "type-after-is"));
    CHECK(has_diagnostic(in_body("x := 1 + double;"), "type-as-operand"));
    CHECK(has_diagnostic(in_body("x := -int;"), "type-as-operand"));
    for (const auto* statement : {"x := v is int || v is double;", "x := int(3) + 1;",
                                  "x := sizeof(int) + 1;", "x := std::is_void_v<* i32> && true;",
                                  "x := :std::vector<int>=(1);"}) {
        INFO(statement);
        CHECK_FALSE(in_body(statement).has_errors());
    }
}

TEST_CASE("a type subject of is is not tested against a value", "[validation][types]") {
    const auto code = std::string{"type-subject-value"};
    const auto source = [](const std::string& body) {
        return translates("B: type = { }\nf: <T> (x: T) -> bool = " + body + ";\n");
    };
    for (const auto* body : {"T is 0", "int is (0)", "B is (:(v) = true)",
                             "inspect T -> bool { is int = true; is (1) = false; is _ = true; }"}) {
        INFO(body);
        CHECK(has_diagnostic(source(body), code));
    }
    for (const auto* body : {"x is 0", "T is int", "B is B", "(x) is (0)",
                             "inspect x -> bool { is 1 = true; is _ = false; }"}) {
        INFO(body);
        CHECK_FALSE(source(body).has_errors());
    }
}
