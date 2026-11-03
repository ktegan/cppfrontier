#include <catch2/catch_all.hpp>
#include <cppfrontier/pipeline.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

TEST_CASE("empty initializers are diagnosed for every local type", "[safety][initializers]") {
    for (const auto type : {"int", "std::string", "std::vector<int>", "Widget"}) {
        const auto result = cppfrontier::translator{}.translate_source(
            std::string{"main: () = { value: "} + type + " = ; }\n");
        INFO(type);
        INFO(result.cpp1);
        CHECK(result.has_errors());
        CHECK(result.cpp1.empty());
        CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "missing-initializer"; }));
    }
}

TEST_CASE("uninitialized function locals are legal when assigned before use",
          "[safety][initializers]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = { value: int; value = 42; return value; }\n");

    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("uninitialized locals use deferred construction",
          "[safety][initializers][deferred]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = { value: i16; value = 3; return value; }\n");

    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find("cpp2::impl::deferred_init<short> value") != std::string::npos);
    CHECK(result.cpp1.find("value.construct(std::type_identity_t<short>{3})") != std::string::npos);
}

TEST_CASE("deferred locals cannot be read before construction",
          "[safety][initializers][deferred][diagnostics]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = { value: int; return value + 1; }\n");
    INFO(result.cpp1);
    CHECK(result.has_errors());
    CHECK(result.cpp1.empty());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "use-before-initialization";
        }));
}

TEST_CASE("deferred locals must be initialized on every path",
          "[safety][initializers][deferred][diagnostics]") {
    for (const auto source : {"f: () = { value: int; }\n"}) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        INFO(source);
        INFO(result.cpp1);
        CHECK(result.has_errors());
        CHECK(result.cpp1.empty());
        CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.code == "use-before-initialization";
            }));
    }
}

TEST_CASE("an uninitialized local is reported as never initialized",
          "[safety][initializers][deferred][diagnostics][upstream-1288]") {
    const auto message = [](const std::string& source) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().message;
    };
    CHECK(message("main: () = { x: int; }\n") == "local variable x is never initialized");
    CHECK(message("f: (c: bool) = { x: int; if c { x = 1; } }\n").find(
              "local variable x") != std::string::npos);
}

TEST_CASE("an out argument initializes a deferred local",
          "[safety][initializers][deferred][out]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "fill: (out value: int) = { value = 1; }\n"
        "main: () -> int = { value: int; fill(out value); return value; }\n");
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("an out argument initializes only where it is certainly evaluated",
          "[safety][initializers][deferred][out][review]") {
    const auto prefix = std::string{
        "f: (out x: int) -> bool = { x = 1; return true; }\n"
        "main: () = {\n"
        "    x: int;\n"
        "    b := std::rand() < 0;\n"
        "    v := std::rand() % 2;\n"};
    const auto first = [&](const std::string& body) {
        const auto result = cppfrontier::translator{}.translate_source(prefix + body + "}\n");
        return result.diagnostics.empty() ? cppfrontier::diagnostic{} : result.diagnostics.front();
    };
    for (const auto* body : {"    if b && f(out x) { }\n    std::cout << x;\n",
                             "    _ = b || f(out x);\n    std::cout << x;\n",
                             "    _ = inspect v -> bool { is 1 = f(out x); is _ = false; };\n"
                             "    std::cout << x;\n",
                             "    _ = b && f(out x);\n    x = 2;\n"}) {
        CAPTURE(body);
        const auto diagnostic = first(body);
        CHECK(diagnostic.code == "use-before-initialization");
        CHECK(diagnostic.message.find("an 'out' argument after '&&' or '||'") != std::string::npos);
        CHECK(diagnostic.span.begin.line == 7);
    }
    for (const auto* body : {"    if f(out x) && b { }\n    std::cout << x;\n",
                             "    _ = inspect v -> bool { is 1 = f(out x); is _ = f(out x); };\n"
                             "    std::cout << x;\n"}) {
        CAPTURE(body);
        CHECK(first(body).message.empty());
    }
}

TEST_CASE("an out argument initializes the reads sequenced after it",
          "[safety][initializers][deferred][out]") {
    const auto prefix = std::string{
        "f: (out x: int) -> int = { x = 1; return 2; }\n"
        "main: () = {\n"
        "    a: int;\n"
        "    b: int;\n"
        "    c := std::rand() < 0;\n"
        "    v: std::vector<int> = (0, 0, 0);\n"};
    const auto codes = [&](const std::string& body) {
        const auto result = cppfrontier::translator{}.translate_source(prefix + body + "}\n");
        auto found = std::vector<std::string>{};
        for (const auto& diagnostic : result.diagnostics) {
            found.push_back(std::to_string(diagnostic.span.begin.line) + " " + diagnostic.code);
        }
        return found;
    };
    // `<<`, `>>`, `&&`, `||` and a list evaluate left to right, and an
    // assignment its right operand first.
    for (const auto* body : {"    std::cout << f(out a) << a;\n    b = 1;\n",
                             "    w: std::vector<int> = (f(out a), a);\n    b = 1;\n",
                             "    _ = f(out a) == 2 && a == 1;\n    b = 1;\n",
                             "    v[a] = f(out a);\n    b = 1;\n",
                             "    _ = inspect f(out a) -> int { is 2 = a; is _ = 0; };\n"
                             "    b = 1;\n",
                             "    b = f(out a);\n"}) {
        CAPTURE(body);
        CHECK(codes(body) == std::vector<std::string>{});
    }
    // Unsequenced operands, a read before the `out` argument, and one that
    // may be evaluated without it.
    for (const auto* body : {"    _ = f(out a) + a;\n    b = 1;\n",
                             "    std::cout << a << f(out a);\n    b = 1;\n",
                             "    _ = (c && f(out a) == 2) || a == 1;\n    b = 1;\n"}) {
        CAPTURE(body);
        CHECK(codes(body) == std::vector<std::string>{"7 use-before-initialization"});
    }
    // The read follows the `out` argument whenever it is evaluated; the
    // argument still initializes `a` on some paths only, and `b` never.
    CHECK(codes("    _ = c && f(out a) == 2 && a == 1;\n") ==
          std::vector<std::string>{"3 use-before-initialization", "4 use-before-initialization"});
    // `a = f(out b)` initializes `b` first, against the declaration order.
    CHECK(codes("    a = f(out b);\n") == std::vector<std::string>{"7 initialization-order"});
}

TEST_CASE("a path that throws needs no local initialized",
          "[safety][initializers][deferred][out][review]") {
    for (const auto* source : {
             "f: (out x: int) = { throw(1); }\n",
             "parse: (s: std::string, out v: int) = {\n"
             "    if !s.empty() { v = 1; return; }\n"
             "    throw(std::runtime_error(\"empty\"));\n"
             "}\n",
             "parse: (s: std::string) -> (v: int) = {\n"
             "    if !s.empty() { v = 1; return; }\n"
             "    throw(std::runtime_error(\"empty\"));\n"
             "}\n",
             "f: (c: bool) = {\n"
             "    x: int;\n"
             "    if c { x = 1; std::cout << x; return; }\n"
             "    throw(1);\n"
             "}\n",
             "f: (c: bool) = { if c { y: int; throw(1); } }\n"}) {
        CAPTURE(source);
        const auto result = cppfrontier::translator{}.translate_source(source);
        for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
        CHECK_FALSE(result.has_errors());
    }
    // A path that returns still needs it.
    CHECK(cppfrontier::translator{}.translate_source(
              "f: (c: bool, out x: int) = { if c { return; } throw(1); }\n").has_errors());
}

TEST_CASE("initialization diagnostics point at the use", "[safety][initializers][review]") {
    const auto span_of = [](const std::string& source) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        REQUIRE_FALSE(result.diagnostics.empty());
        return result.diagnostics.front().span.begin;
    };
    const auto call = span_of("f: (a: int, b: int) -> int = a + b;\n"
                              "main: () = {\n    x: int;\n    std::cout << f(1,\n        x);\n}\n");
    CHECK(call.line == 5);
    CHECK(call.column == 9);
    const auto initializer = span_of("main: () = { x: int; y := 1 + x; }\n");
    CHECK(initializer.column == 31);
    const auto returned = span_of("f: () -> int = { x: int; return 1 + x; }\n");
    CHECK(returned.column == 37);
    const auto order = span_of("main: () = { x: int; y: int; y = 1; x = 2; }\n");
    CHECK(order.column == 30);
}

TEST_CASE("a constructor reads only the members initialized before",
          "[safety][initializers][members]") {
    const auto reads = [](const std::string& body) {
        const auto result = cppfrontier::translator{}.translate_source(
            "X: type = {\n    x: int;\n    y: int;\n    z: int = 5;\n"
            "    operator=: (out this, a: int) = { " + body + " }\n}\n"
            "main: () = { v: X = (1); }\n");
        auto found = std::vector<std::string>{};
        for (const auto& diagnostic : result.diagnostics) {
            if (diagnostic.code == "use-before-initialization") {
                found.push_back(diagnostic.message.substr(0, diagnostic.message.find(" is")) +
                                "@" + std::to_string(diagnostic.span.begin.column));
            }
        }
        return found;
    };
    CHECK(reads("x = y; y = 2;") == std::vector<std::string>{"member y@43"});
    CHECK(reads("x = this.y; y = 2;") == std::vector<std::string>{"member y@43"});
    CHECK(reads("x = x + 1; y = 2;") == std::vector<std::string>{"member x@43"});
    CHECK(reads("x = \"(y)$\"; y = 2;") == std::vector<std::string>{"member y@43"});
    CHECK(reads("x = 1; y = x + a; z = y; std::cout << y;").empty());
    // A member with a default initializer is initialized at its position too.
    CHECK(reads("x = 1; y = x + z + a; z = y;") == std::vector<std::string>{"member z@54"});
    CHECK(reads("x = z; y = 2;") == std::vector<std::string>{"member z@43"});
    CHECK(reads("x = sizeof(y); y = 2;").empty());
    CHECK(reads("x = 1; y = 2; x = y;").empty());
    // Preconditions are checked before any member is initialized.
    const auto pre = cppfrontier::translator{}.translate_source(
        "X: type = { n: int = 0; operator=: (out this, a: int) pre(n == a) = { n = a; }\n"
        "    f: (this) pre(n >= 0) = { } }\n");
    CHECK(std::count_if(pre.diagnostics.begin(), pre.diagnostics.end(), [](const auto& value) {
        return value.code == "use-before-initialization";
    }) == 1);
    // A parameter hides the member.
    const auto hidden = cppfrontier::translator{}.translate_source(
        "X: type = { x: int; y: int; operator=: (out this, y: int) = { x = y; this.y = y; } }\n");
    CHECK_FALSE(hidden.has_errors());
    // A copy constructor copies the members it does not set in declaration order.
    const auto copy = cppfrontier::translator{}.translate_source(
        "X: type = { x: int; y: int; operator=: (out this, that) = { x = y; } }\n");
    CHECK(std::any_of(copy.diagnostics.begin(), copy.diagnostics.end(), [](const auto& value) {
        return value.code == "use-before-initialization";
    }));
}

TEST_CASE("an argument list of 16000 items translates in linear time", "[safety][review]") {
    // Four times the items take about four times as long; a quadratic pass takes 16.
    const auto seconds = [](int items) {
        auto source = std::string{"f: (args...) = { }\nmain: () = { a := 1; b := 2; f("};
        for (auto index = 0; index < items; ++index) source += index == 0 ? "a + b" : ", a + b";
        source += "); }\n";
        const auto start = std::chrono::steady_clock::now();
        const auto result = cppfrontier::translator{}.translate_source(source);
        CHECK_FALSE(result.has_errors());
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    const auto small = seconds(4000);
    const auto large = seconds(16000);
    INFO(small << " s for 4000 items, " << large << " s for 16000");
    CHECK(large < 8 * small);
}

TEST_CASE("local type aliases are not deferred objects",
          "[safety][aliases][initializers]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = {\n"
        "    t1: type == bool;\n"
        "    value: t1 = true;\n"
        "    return value;\n"
        "}\n");
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("deferred locals in nested scopes do not leak to the outer scope",
          "[safety][initializers][deferred][scope]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: () -> int = {\n"
        "    value: int = 1;\n"
        "    { value: int; value = 2; }\n"
        "    return value;\n"
        "}\n");
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("partial branch initialization cannot be completed by assignment",
          "[safety][initializers][deferred][branches]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: () = {\n"
        "    value: int;\n"
        "    if true { value = 1; }\n"
        "    value = 2;\n"
        "}\n");
    INFO(result.cpp1);
    CHECK(result.has_errors());
    CHECK(result.cpp1.empty());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "initialization-branch-mismatch";
        }));
}

TEST_CASE("all branch alternatives may initialize a deferred local",
          "[safety][initializers][deferred][branches]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () -> int = {\n"
        "    value: int;\n"
        "    if true { value = 1; }\n"
        "    else { value = 2; }\n"
        "    return value;\n"
        "}\n");
    INFO(result.cpp1);
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("an inner shadow does not initialize an outer deferred local",
          "[safety][initializers][deferred][scope][shadowing]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: () = {\n"
        "    value: int;\n"
        "    { value: int = 1; _ = value; }\n"
        "    _ = value;\n"
        "}\n");
    INFO(result.cpp1);
    CHECK(result.has_errors());
    CHECK(result.cpp1.empty());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "use-before-initialization";
        }));
}

TEST_CASE("namespace-scope objects require initializers", "[safety][namespaces]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "value: std::string;\n"
        "tools: namespace = { nested: std::vector<int>; }\n");

    INFO(result.cpp1);
    CHECK(result.has_errors());
    CHECK(result.cpp1.empty());
    CHECK(std::count_if(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "uninitialized-namespace-object";
    }) == 2);
}

TEST_CASE("type-scope data members may use a default initializer",
          "[safety][types][initializers]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = { value: int; }\n");

    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
    CHECK(result.cpp1.find("private: int value;") != std::string::npos);
}

TEST_CASE("type-scope objects require an explicit type", "[safety][types][diagnostics]") {
    for (const auto declaration : {"value;", "value: _;"}) {
        const auto result = cppfrontier::translator{}.translate_source(
            std::string{"Widget: type = { "} + declaration + " }\n");
        INFO(declaration);
        INFO(result.cpp1);
        CHECK(result.has_errors());
        CHECK(result.cpp1.empty());
        // `value: _;` is first an object with a deduced type and no initializer.
        CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.code == "type-scope-object-requires-type" ||
                    diagnostic.code == "deduced-object-initializer";
            }));
    }
}

TEST_CASE("explicit auto is rejected in Cpp2 object declarations", "[safety][types][diagnostics]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = { value: auto = 0; }\n");

    INFO(result.cpp1);
    CHECK(result.has_errors());
    CHECK(result.cpp1.empty());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "cpp1-auto-type"; }));
}

TEST_CASE("uninitialized locals in namespace and member functions remain legal",
          "[safety][namespaces][types]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "tools: namespace = {\n"
        "    helper: () -> int = { value: int; value = 1; return value; }\n"
        "}\n"
        "Widget: type = {\n"
        "    read: (this) -> int = { value: int; value = 2; return value; }\n"
        "}\n");

    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("empty member initializers are diagnosed", "[safety][types][initializers]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "Widget: type = { value: std::string = ; }\n");

    INFO(result.cpp1);
    CHECK(result.has_errors());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "missing-initializer";
    }));
}

TEST_CASE("uninitialized locals cannot be initialized inside a loop",
          "[safety][initializers][loops]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: () = {\n"
        "    value: int;\n"
        "    while true { value = 42; }\n"
        "}\n");

    INFO(result.cpp1);
    CHECK(result.has_errors());
    CHECK(result.cpp1.empty());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "initialization-in-loop"; }));
}

TEST_CASE("an initialized local can be assigned inside a loop", "[safety][initializers][loops]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: () = {\n"
        "    value: int;\n"
        "    value = 0;\n"
        "    while true { value = 42; }\n"
        "}\n");

    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
}

TEST_CASE("a loop assignment is checked in either branch", "[safety][initializers][loops]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: () = {\n"
        "    value: int;\n"
        "    if true { while true { value = 1; } }\n"
        "    else { value = 2; }\n"
        "    value = 3;\n"
        "}\n");

    INFO(result.cpp1);
    CHECK(result.has_errors());
    CHECK(result.cpp1.empty());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "initialization-in-loop"; }));
}

TEST_CASE("division checks follow translation options", "[safety][division][options]") {
    const auto source = std::string{
        "main: () -> int = { divisor := 0; return 12 / divisor; }\n"};

    const auto checked = cppfrontier::translator{}.translate_source(source);
    INFO(checked.cpp1);
    REQUIRE_FALSE(checked.has_errors());
    CHECK(checked.cpp1.find("CPP2_ASSERT_NOT_ZERO") != std::string::npos);

    auto options = cppfrontier::translation_options{};
    options.check_division_by_zero = false;
    const auto unchecked = cppfrontier::translator{}.translate_source(source, options);
    INFO(unchecked.cpp1);
    REQUIRE_FALSE(unchecked.has_errors());
    CHECK(unchecked.cpp1.find("CPP2_ASSERT_NOT_ZERO") == std::string::npos);
}

TEST_CASE("named return values and out parameters are initialized before return",
          "[safety][initializers][named-returns][diagnostics]") {
    const auto message = [](const std::string& source) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().message;
    };
    CHECK(message("f: (c: bool) -> (x: int) = { if c { return; } x = 1; }\n") ==
          "x must be initialized before 'return'");
    CHECK(message("f: (c: bool, out x: int) = { if c { return; } x = 1; }\n") ==
          "x must be initialized before 'return'");
    CHECK(message("f: () -> (x: int, y: int) = { x = 1; }\n") ==
          "local variable y is never initialized");
    CHECK(message("f: (c: bool) -> (x: int) = { if c { x = 1; return; } x = 2; }\n").empty());
    CHECK(message("f: (c: bool) = { x: int; if c { x = 1; return; } x = 2; _ = x; }\n").empty());
}

TEST_CASE("an if whose branches all return ends the body initialized from each branch",
          "[safety][initializers][named-returns][hunt]") {
    const auto message = [](const std::string& source) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().message;
    };
    // From upstream's reflect.h2 `grab_until_impl`.
    CHECK(message("f: (c: bool, out x: int) -> bool = "
                  "{ if c { x = 1; return true; } else { x = 2; return false; } }\n").empty());
    CHECK(message("f: (c: bool) = { x: int; if c { x = 1; return; } else { x = 2; return; } }\n")
              .empty());
    CHECK(message("f: (c: bool) = { x: int; if c { return; } else { return; } }\n") ==
          "local variable x is never initialized");
}

TEST_CASE("deferred locals cannot be read in conditions, parameters, captures or interpolations",
          "[safety][initializers][deferred][review]") {
    for (const auto* body : {
             "x: int; if x > 0 { } x = 1;",
             "x: int; while x > 0 { } x = 1;",
             "x: int; do { } while x > 0; x = 1;",
             "x: int; i := 0; while i < 1 next x = 2 { i++; } x = 1;",
             "x: int; for (x) do (e) { } x = 1;",
             "x: int; (copy y := x) { } x = 1;",
             "x: int; l := :() = x$; x = 1;",
             "x: int; s := \"(x)$\"; x = 1;",
         }) {
        const auto result = cppfrontier::translator{}.translate_source(
            std::string{"main: () = { "} + body + " }\n");
        INFO(body);
        CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "use-before-initialization"; }));
    }
    const auto condition = cppfrontier::translator{}.translate_source(
        "main: () = { x: int; if x > 0 { } x = 1; }\n");
    REQUIRE_FALSE(condition.diagnostics.empty());
    CHECK(condition.diagnostics.front().message ==
          "local variable x is used in a condition before it was initialized");
    for (const auto* body : {
             "x: int; if f(out x) { } _ = x;",
             "x: int; (copy y := 2) { x = y; } _ = x;",
             "x: int; x = 1; if x > 0 { _ = :() = x$; }",
         }) {
        const auto result = cppfrontier::translator{}.translate_source(
            std::string{"f: (out x: int) -> bool = { x = 1; return true; }\nmain: () = { "} +
            body + " }\n");
        INFO(body);
        CHECK_FALSE(result.has_errors());
    }
}

TEST_CASE("a comparison chain must be transitive", "[safety][comparisons][ports]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "a: int = 1;\n"
        "ok: bool = a <= a < a == (a >= a > a);\n"
        "f: () -> bool = a != a != a;\n"
        "t: type = { m: bool = a > a < a; }\n"
        "g: () = { if a >= a <= a { } h := :(x: int) -> bool = a < x > a; }\n");

    const auto count = [&](std::string_view message) {
        return std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) {
                return diagnostic.code == "comparison-chain" && diagnostic.message == message;
            });
    };
    CHECK(count("!= comparisons cannot appear in a comparison chain") == 1);
    CHECK(count("a comparison chain must be all < and <=, all > and >=, or all ==") == 3);
    CHECK(result.diagnostics.size() == 4);
}

TEST_CASE("comparison diagnostics point at the comparison", "[safety][comparisons][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "main: () = {\n"
        "    a: u32 = 1;\n"
        "    b: i32 = 2;\n"
        "    x: bool =\n"
        "        (a < b);\n"
        "    _ = f(1,\n"
        "          b < b > b);\n"
        "    y := b < b > b;\n"
        "}\n");
    REQUIRE(result.diagnostics.size() == 3);
    const auto at = [&](std::size_t index) {
        const auto& value = result.diagnostics[index];
        return std::pair{value.span.begin.line, value.span.begin.column};
    };
    CHECK(result.diagnostics[0].code == "mixed-sign-comparison");
    CHECK(at(0) == std::pair{5u, 10u});
    CHECK(result.diagnostics[1].code == "comparison-chain");
    CHECK(at(1) == std::pair{7u, 11u});
    CHECK(at(2) == std::pair{8u, 10u});
}

TEST_CASE("mixed-sign comparisons are rejected in every expression a function evaluates",
          "[safety][comparisons][review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f2: (i: int, u: u32) = { std::cout << \"(i < u)$\"; }\n"
        "f3: (i: int, u: u32) = { (copy b := i <= u) { std::cout << b; } }\n"
        "f4: (i: int, u: u32) pre(i > u) = { }\n"
        "f5: (i: int, u: u32) post(true, \"(i >= u)$\") = { }\n"
        "f7: (i: int, u: u32) = { assert(true, \"(u < i)$\"); }\n");
    auto lines = std::vector<std::uint32_t>{};
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == "mixed-sign-comparison") lines.push_back(diagnostic.span.begin.line);
    }
    CHECK(lines == std::vector<std::uint32_t>{1, 2, 3, 4, 5});
}

TEST_CASE("mixed-sign relational comparisons of known integer types are rejected",
          "[safety][comparisons]") {
    const auto source = std::string{
        "f: (i: int, u: unsigned) -> bool = i < u;\n"
        "g: (i: i32, s: std::size_t) -> bool = { if s >= i { return true; } return 0 <= s; }\n"
        "t: type = { n: u64 = 0; big: (this, i: i64) -> bool = n > i; }\n"
        "h: (n: int) = { (copy k: ulong = 0) while k <= n next k++ { } }\n"
        "c: (i: int, u: u32) -> bool = i < 1 < u;\n"};
    const auto result = cppfrontier::translator{}.translate_source(source);
    const auto messages = [&] {
        auto found = std::vector<std::string>{};
        for (const auto& diagnostic : result.diagnostics) {
            if (diagnostic.code == "mixed-sign-comparison") found.push_back(diagnostic.message);
        }
        return found;
    }();
    REQUIRE(messages.size() == 6);
    CHECK(messages[0] == "'i < u' is a mixed signed/unsigned comparison, which is unsafe - use "
                         "'as' to change the signedness of one operand, or unchecked_cmp_less "
                         "to opt out");
    CHECK(messages[1].starts_with("'s >= i' "));
    CHECK(messages[1].ends_with("unchecked_cmp_greater_eq to opt out"));
    CHECK(messages[2].starts_with("'0 <= s' "));
    CHECK(messages[2].find("write '0u' for an unsigned literal") != std::string::npos);
    CHECK(messages[0].find("unsigned literal") == std::string::npos);
    CHECK(messages[3].starts_with("'n > i' "));
    CHECK(messages[4].starts_with("'k <= n' "));
    CHECK(messages[5].starts_with("'1 < u' "));
    CHECK(result.diagnostics.size() == 6);

    auto options = cppfrontier::translation_options{};
    options.check_comparisons = false;
    const auto unchecked = cppfrontier::translator{}.translate_source(source, options);
    INFO(unchecked.cpp1);
    CHECK_FALSE(unchecked.has_errors());
    CHECK(unchecked.cpp1.find("return i < u;") != std::string::npos);
}

TEST_CASE("same-sign and unknown-sign relational comparisons are left to cmp_*",
          "[safety][comparisons]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (i: int, j: i64, u: u8, s: std::size_t, x) -> bool = {\n"
        "    if i < j && u <= s && s > 0u && i >= -1 && 0x10 < s && x > i { return true; }\n"
        "    c: char = 'a';\n"
        "    n := 0;\n"
        "    return c < u || n < s || i == s || i != u || unchecked_cmp_less(i, u);\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("cpp2::impl::cmp_less(i,j) && cpp2::impl::cmp_less_eq(u,s) && "
                           "cpp2::impl::cmp_greater(s,0u) && cpp2::impl::cmp_greater_eq(i,-1) && "
                           "cpp2::impl::cmp_less(0x10,s) && cpp2::impl::cmp_greater(x,i)") !=
          std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::cmp_less(cpp2::move(c),u) || "
                           "cpp2::impl::cmp_less(cpp2::move(n),s) || i == s || i != u || "
                           "cpp2::unchecked_cmp_less(i, u)") != std::string::npos);
}

namespace {

auto member_copy_hints(const cppfrontier::translation_result& result) -> std::vector<std::string> {
    auto messages = std::vector<std::string>{};
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == "member-copied-not-moved") {
            CHECK(diagnostic.severity == cppfrontier::diagnostic_severity::warning);
            messages.push_back(diagnostic.message);
        }
    }
    return messages;
}

auto translate_move_function(std::string_view body) -> cppfrontier::translation_result {
    return cppfrontier::translator{}.translate_source(
        "Widget: type = {\n"
        "    a: std::string = ();\n"
        "    b: std::string = ();\n"
        "    v: std::vector<std::string> = ();\n"
        "    id: int = 0;\n"
        "    operator=: (out this) = { }\n"
        "    operator=: (out this, that) = { a = that.a; b = that.b; }\n"
        "    operator=: (out this, move that) = " + std::string{body} + "\n"
        "    f: (s: std::string) -> std::string = s;\n"
        "}\n");
}

}

TEST_CASE("a member copied from a move parameter before its last use is hinted",
          "[safety][last-use][move-that]") {
    const auto result = translate_move_function("{ a = that.a; b = that.b; }");
    INFO(result.cpp1);
    CHECK_FALSE(result.has_errors());
    CHECK_FALSE(result.cpp1.empty());
    CHECK(member_copy_hints(result) == std::vector<std::string>{
        "'that.a' is copied, not moved: only the last use of 'that' moves; "
        "write '(move that.a)' to move it"});
}

TEST_CASE("member copy hints fire for whole operands that copy",
          "[safety][last-use][move-that]") {
    for (const auto* body : {"{ x := that.a; that.id = 0; }",
                             "{ x: std::string = that.a; that.id = 0; }",
                             "{ x: std::string = (); x = that.a; that.id = 0; }"}) {
        INFO(body);
        const auto result = translate_move_function(body);
        CHECK_FALSE(result.has_errors());
        CHECK(member_copy_hints(result).size() == 1);
    }
    const auto result = cppfrontier::translator{}.translate_source(
        "P: type = { public name: std::string = (); }\n"
        "f: (move p: P) -> std::string = { x := p.name; return p.name; }\n"
        "g: (move p: P) = { std::cout << p.name; x := p.name; }\n");
    CHECK_FALSE(result.has_errors());
    CHECK(member_copy_hints(result) == std::vector<std::string>{
        "'p.name' is copied, not moved: only the last use of 'p' moves; "
        "write '(move p.name)' to move it"});
}

TEST_CASE("member copy hints do not fire where a move would not help",
          "[safety][last-use][move-that]") {
    for (const auto* body : {"{ a = that.a; }",
                             "{ id = that.id; a = that.a; }",
                             "{ x := that.a.size(); y := that.v[0]; a = that.a; }",
                             "{ that.b = \"\"; a = that.a; }",
                             "{ a = (move that.a); b = std::move(that.b); that.id = 0; }",
                             "{ x: decltype(that.a) = (); a = that.a; }",
                             "{ std::cout << that.a; b = that.b; }",
                             "{ x := f(that.v.front()); a = that.a; }",
                             "{ a = f(that.b); }",
                             "{ x := f(that.a); that.id = 0; }"}) {
        INFO(body);
        const auto result = translate_move_function(body);
        INFO(result.cpp1);
        CHECK_FALSE(result.has_errors());
        CHECK(member_copy_hints(result).empty());
    }
}

TEST_CASE("member copy hints do not fire in a loop, where a move is never safe",
          "[safety][last-use][move-that][hunt]") {
    for (const auto* body : {"{ for (1, 2) do (i) { x := that.a; } }",
                             "{ i := 0; while i < 2 next i++ { if i == 1 { x := that.a; } } }",
                             "{ do { x: std::string = that.a; } while false; }"}) {
        INFO(body);
        const auto result = translate_move_function(body);
        INFO(result.cpp1);
        CHECK_FALSE(result.has_errors());
        CHECK(member_copy_hints(result).empty());
    }
}

TEST_CASE("member copy hints are kept with -pure-cpp2",
          "[safety][last-use][move-that]") {
    auto options = cppfrontier::translation_options{};
    options.pure_cpp2 = true;
    const auto result = cppfrontier::translator{}.translate_source(
        "P: type = { public name: std::string = (); }\n"
        "f: (move p: P) = { x := p.name; y := p.name; }\n", options);
    CHECK_FALSE(result.has_errors());
    CHECK_FALSE(result.cpp1.empty());
    CHECK(member_copy_hints(result).size() == 1);
}

namespace {

// A deep copy of `input` that keeps each statement's id, as any copy of the IR does.
auto clone(const cppfrontier::statement& input) -> std::unique_ptr<cppfrontier::statement> {
    using cppfrontier::statement;
    const auto clone_list = [](const cppfrontier::statement_list& list) {
        auto result = cppfrontier::statement_list{};
        for (const auto& value : list) result.push_back(clone(*value));
        return result;
    };
    auto output = std::make_unique<statement>(input.kind);
    output->span = input.span;
    output->symbol = input.symbol;
    output->id = input.id;
    std::visit([&](const auto& form) {
        using form_type = std::remove_cvref_t<decltype(form)>;
        auto& copy = output->as<form_type>();
        if constexpr (std::is_same_v<form_type, statement::block_form>) {
            copy.parameters = form.parameters;
            copy.body = clone_list(form.body);
            for (const auto& handler : form.handlers) {
                copy.handlers.push_back({handler.caught, clone_list(handler.body)});
            }
        }
        else if constexpr (std::is_same_v<form_type, statement::if_form>) {
            copy.condition = form.condition;
            copy.body = clone_list(form.body);
            copy.else_body = clone_list(form.else_body);
            copy.has_else = form.has_else;
            copy.is_else_if = form.is_else_if;
            copy.is_constexpr = form.is_constexpr;
        }
        else if constexpr (std::is_same_v<form_type, statement::loop_form>) {
            copy.label = form.label;
            copy.condition = form.condition;
            copy.next = form.next;
            copy.range_parameter = form.range_parameter;
            copy.body = clone_list(form.body);
        }
        else if constexpr (std::is_same_v<form_type, statement::declaration_form>) {
            copy.text = form.text;
            copy.declaration = form.declaration;
        }
        else if constexpr (std::is_same_v<form_type, statement::destructuring_form>) {
            copy.bindings = form.bindings;
            copy.initializer = form.initializer;
        }
        else {
            copy = form;
        }
    }, input.form);
    return output;
}

auto clone_bodies(std::vector<cppfrontier::ir_declaration>& declarations) -> void {
    for (auto& declaration : declarations) {
        clone_bodies(declaration.members);
        if (!declaration.function_statements) continue;
        auto body = std::make_shared<cppfrontier::function_body>();
        body->span = declaration.function_statements->span;
        for (const auto& value : declaration.function_statements->statements) {
            body->statements.push_back(clone(*value));
        }
        declaration.function_statements = std::move(body);
    }
}

}

TEST_CASE("the initialization plan applies to a copy of the program",
          "[safety][last-use][deferred]") {
    const auto text = std::string{
        "f: (copy s: std::string, copy n: int) -> std::string = {\n"
        "    t: std::string;\n"
        "    t = s;\n"
        "    assert(n < 10, \"n\");\n"
        "    g := :() -> std::string = t$;\n"
        "    return g();\n"
        "}\n"
        "h: (copy x: std::string) -> std::string = x;\n"
        "point: type = {\n"
        "    name: std::string;\n"
        "    operator=: (out this, move that) = { name = that.name; }\n"
        "}\n"};
    auto result = cppfrontier::translator{}.translate_source(text);
    REQUIRE_FALSE(result.has_errors());
    const auto source = cppfrontier::source_text::from_bytes(text);
    const auto plan = cppfrontier::analyze_safety(result.program).plan;
    auto copy = result.program;
    clone_bodies(copy.declarations);
    // The copy refers to no statement of the original.
    result.program = {};
    const auto emitted = cppfrontier::emit_cpp1(copy, source, result.tokens, result.options, plan);
    CHECK(emitted.cpp1 == result.cpp1);
    for (const auto* expected : {"t.construct(CPP2_FORWARD(s));", "cmp_less(n,10)",
                                 "[_0 = cpp2::move(t.value())]", "return cpp2::move(g)();",
                                 "{ return x; }", "name{cpp2::move(that).name}"}) {
        INFO(expected);
        CHECK(emitted.cpp1.find(expected) != std::string::npos);
    }
}

TEST_CASE("pointer arithmetic on a member named through an object is diagnosed",
          "[safety][review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "g: int = 3;\n"
        "S: type = { public p: *int = g&; }\n"
        "main: () = { s: S = (); a := s.p + 1; _ = a; }\n");
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& issue) {
            return issue.code == "pointer-arithmetic" && issue.span.begin.line == 3;
        }));
}

TEST_CASE("contracts evaluated on entry cannot read out parameters or named return values",
          "[safety][initializers][hunt]") {
    const auto first = [](const std::string& source) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        return result.diagnostics.empty()
            ? std::string{}
            : std::to_string(result.diagnostics.front().span.begin.column) + ": " +
                result.diagnostics.front().message;
    };
    CHECK(first("f: (out x: int) pre(x > 0) = { x = 1; }\n") ==
          "21: local variable x is used before it was initialized");
    CHECK(first("f: (out x: int) post(x$ > 0) = { x = 1; }\n") ==
          "22: local variable x is used before it was initialized");
    CHECK(first("f: (n: int) -> (r: int) pre(r == n) = { r = n; }\n") ==
          "29: local variable r is used before it was initialized");
    CHECK(first("f: (n: int) -> (r: int) post(r$ == n) = { r = n; }\n") ==
          "30: local variable r is used before it was initialized");
    CHECK(first("f: (out x: int) pre(x > 0) = x = 1;\n") ==
          "21: local variable x is used before it was initialized");
    // A postcondition is checked on exit, and a capture of an initialized value is not a read.
    CHECK(first("f: (out x: int) post(x > 0) = { x = 1; }\n").empty());
    CHECK(first("f: (n: int) -> (r: int = 1) pre(r == 1) post(r$ == r) = { }\n").empty());
    CHECK(first("f: (out x: int) pre<unevaluated>(x > 0) = { x = 1; }\n").empty());
}

TEST_CASE("an expression body must initialize its out parameters before reading them",
          "[safety][initializers][hunt]") {
    const auto first = [](const std::string& source) {
        const auto result = cppfrontier::translator{}.translate_source(source);
        return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().message;
    };
    CHECK(first("f: (out x: int) = std::cout << x;\n") ==
          "local variable x is used before it was initialized");
    CHECK(first("f: (out x: int, y: int) = y + 1;\n") == "local variable x is never initialized");
    CHECK(first("g: (out v: int) -> bool = { v = 2; return true; }\n"
                "f: (out x: int, c: bool) = c && g(out x);\n")
              .starts_with("local variable x is never initialized (an 'out' argument"));
    CHECK(first("f: (out x: int) = x = 1;\n").empty());
    CHECK(first("g: (out v: int) = v = 2;\nf: (out x: int) = g(out x);\n").empty());
}

TEST_CASE("a mixed-sign comparison is reported at its left operand",
          "[safety][comparisons][hunt]") {
    const auto columns = [](const std::string& source) {
        auto result = std::string{};
        for (const auto& issue : cppfrontier::translator{}.translate_source(source).diagnostics) {
            if (issue.code != "mixed-sign-comparison") continue;
            result += std::to_string(issue.span.begin.line) + ":" +
                std::to_string(issue.span.begin.column) + " ";
        }
        return result;
    };
    CHECK(columns("f: (s: std::size_t) -> bool = {\n"
                  "    return s > 0 &&\n"
                  "        s < 10;\n"
                  "}\n") == "2:12 3:9 ");
    // In an interpolation, at the string.
    CHECK(columns("f: (s: std::size_t) = std::cout << \"(s > 0)$\";\n") == "1:36 ");
    CHECK(columns("f: (s: std::size_t) pre(s >= 1) = { }\n") == "1:25 ");
}

TEST_CASE("an uninitialized local is reported once, whichever path reports it",
          "[safety][initializers][hunt]") {
    const auto messages = [](const std::string& body) {
        auto result = std::string{};
        for (const auto& issue : cppfrontier::translator{}.translate_source(
                 "f: (n: int) -> int = { " + body + " }\n").diagnostics) {
            result += issue.message + "; ";
        }
        return result;
    };
    CHECK(messages("x: int; if n > 0 { std::cout << x; } std::cout << x; x = 1; return x;") ==
          "local variable x is used before it was initialized; ");
    CHECK(messages("x: int; while n > 0 { x = 1; } return x;") ==
          "local variable x cannot be initialized inside a loop; ");
    // A handler may be reached before or after the try block initializes it.
    CHECK(messages("x: int; try { x = 1; std::cout << x; } catch (_) { x = 2; } return x;") ==
          "local variable x cannot be initialized here, where it is initialized on some "
          "paths only; ");
    CHECK(messages("x: int; try { if n > 0 { x = 1; } else { x = 3; } } catch (_) { x = 2; } "
                   "return x;") ==
          "local variable x cannot be initialized here, where it is initialized on some "
          "paths only; ");
    // A last statement that throws undoes its initializations.
    CHECK(messages("x: int; try { x = 1; } catch (_) { x = 2; } return x;").empty());
    CHECK(messages("x: int; try { std::cout << n; x = n; } catch (_) { x = 2; } return x;")
              .empty());
    CHECK(messages("x: int; try { x = 1; } catch (_) { return 0; } return x;").empty());
}

TEST_CASE("pointer arithmetic is diagnosed with the pointer as the right operand of +",
          "[safety][pointers][hunt]") {
    const auto lines = [](const std::string& source) {
        auto result = std::string{};
        for (const auto& issue : cppfrontier::translator{}.translate_source(source).diagnostics) {
            if (issue.code == "pointer-arithmetic") {
                result += std::to_string(issue.span.begin.line) + " ";
            }
        }
        return result;
    };
    CHECK(lines("A: namespace = { x: int = 1; p: *int = x&; }\n"
                "S: type = {\n"
                "    public q: *int = A::x&;\n"
                "    f: (this) -> *int = 1 + q;\n"
                "    g: (this, s: S) -> *int = (2) + s.q;\n"
                "}\n"
                "main: () = {\n"
                "    n := 1;\n"
                "    a := n + A::p;\n"
                "    b := 1 + A::p*;\n"
                "    c := A::p + A::p;\n"
                "    _ = (a, b, c);\n"
                "}\n") == "4 5 9 11 ");
    CHECK(lines("f: (p: *int) -> int = 1 + g(p);\ng: (p: *int) -> int = p*;\n").empty());
    CHECK(lines("f: (p: *int) -> int = 1 + p*;\n").empty());
}

TEST_CASE("a contract group is a use of the names it mentions", "[safety][contracts][hunt]") {
    auto result = cppfrontier::translator{}.translate_source(
        "main: () = {\n"
        "    g := shared.new<cpp2::contract_group>();\n"
        "    h := g;\n"
        "    assert<g*>(h.get() != nullptr);\n"
        "}\n");
    INFO(result.cpp1);
    CHECK(result.diagnostics.empty());
    CHECK(result.cpp1.find("auto h {g};") != std::string::npos);
    CHECK(result.cpp1.find("if ((*cpp2::impl::assert_not_null(g)).is_active()") !=
          std::string::npos);
    result = cppfrontier::translator{}.translate_source(
        "main: () = { g: cpp2::contract_group; assert<g>(true); g = (); }\n");
    REQUIRE(!result.diagnostics.empty());
    CHECK(result.diagnostics.front().message ==
          "local variable g is used before it was initialized");
}

TEST_CASE("a capture of an uninitialized local is reported at its capture",
          "[safety][captures]") {
    for (const auto* body : {"    f := :() -> int = 1 + x$ * x$;\n",
                             "    f := :() -> int = { y := 2; return y + x$; };\n"}) {
        CAPTURE(body);
        const auto result = cppfrontier::translator{}.translate_source(
            std::string{"main: () = {\n    x: int;\n"} + body + "    x = 1;\n    _ = f();\n}\n");
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().message ==
              "local variable x is used before it was initialized");
        CHECK(result.diagnostics.front().span.begin.line == 3);
        CHECK(result.diagnostics.front().span.begin.column ==
              std::string_view{body}.find("x$") + 1);
    }
}

TEST_CASE("a returned copy parameter is left to Cpp1's implicit move",
          "[safety][last-use][hunt]") {
    // Where the return type is `decltype(auto)`, `cpp2::move(a)` would be a reference.
    const auto result = cppfrontier::translator{}.translate_source(
        "f: (copy a: i64) = a;\n"
        "g: (copy b: i64) -> forward _ = { return b; }\n"
        "h: (copy c: i64) = c + 1;\n"
        "k: (copy d: i64) -> i64 = { return d; }\n"
        "main: () = { std::cout << f(1) << g(2) << h(3) << k(4); }\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("parameter\"); return a; }") != std::string::npos);
    CHECK(result.cpp1.find("parameter\"); return b;") != std::string::npos);
    CHECK(result.cpp1.find("return cpp2::move(c) + 1;") != std::string::npos);
    CHECK(result.cpp1.find("return d;") != std::string::npos);
}

TEST_CASE("an out argument in a returned value initializes", "[safety][initializers][hunt]") {
    const auto messages = [](const std::string& body) {
        auto result = std::string{};
        for (const auto& issue : cppfrontier::translator{}.translate_source(
                 "set: (out a: int) -> int = { a = 1; return 1; }\n" + body).diagnostics) {
            result += issue.message + "; ";
        }
        return result;
    };
    CHECK(messages("f: () -> int = { z: int; return set(out z); }\n").empty());
    CHECK(messages("f: (out x: int) -> int = { return set(out x); }\n").empty());
    CHECK(messages("f: () -> int = { z: int; return set(out z) + set(out z); }\n") ==
          "z is passed as 'out' more than once in one expression; ");
    CHECK(messages("f: () -> int = { z: int; while true { return set(out z); } }\n") ==
          "local variable z cannot be initialized inside a loop; ");
}

TEST_CASE("a null pointer value in grouping parentheses is diagnosed",
          "[safety][pointers][hunt]") {
    const auto codes = [](const std::string& source) {
        auto result = std::string{};
        for (const auto& issue : cppfrontier::translator{}.translate_source(source).diagnostics) {
            result += std::to_string(issue.span.begin.line) + " " + issue.code + "; ";
        }
        return result;
    };
    CHECK(codes("main: () = {\n"
                "    a := 1;\n"
                "    p: *int = (nullptr);\n"
                "    q: *int = ((0));\n"
                "    r: *int = (a&);\n"
                "    r = (nullptr);\n"
                "    r = ();\n"
                "    r = (q);\n"
                "    _ = p;\n"
                "}\n") == "3 pointer-null-initialization; 4 pointer-null-initialization; "
                          "6 pointer-null-assignment; 7 pointer-null-assignment; ");
}

TEST_CASE("an uninitialized contract group or flag is reported at its name",
          "[safety][contracts][hunt]") {
    const auto at = [](const std::string& source) {
        auto result = std::string{};
        for (const auto& issue : cppfrontier::translator{}.translate_source(source).diagnostics) {
            result += std::to_string(issue.span.begin.line) + ":" +
                std::to_string(issue.span.begin.column) + " " + issue.code + "; ";
        }
        return result;
    };
    CHECK(at("main: () = {\n"
             "    flag: bool;\n"
             "    assert<bounds_safety, flag>(true);\n"
             "    flag = true;\n"
             "}\n") == "3:27 use-before-initialization; ");
    CHECK(at("f: (out g: cpp2::contract_group) pre<g>(true) = { g = (); }\n") ==
          "1:38 use-before-initialization; ");
}
