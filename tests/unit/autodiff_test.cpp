#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

#include "test_support.hpp"

namespace {

auto run_program(const std::string& cpp1, const std::string& name) -> test_support::process_result {
    const auto scratch = test_support::scratch_directory("cppfrontier-autodiff");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / (name + ".cpp");
    const auto executable_path = scratch / name;
    test_support::write_file(cpp1_path, cpp1);
    auto result = test_support::run_process({CPPFRONTIER_GXX, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{120});
    if (!result.timed_out && result.exit_status == 0) {
        result = test_support::run_process({executable_path.string()}, scratch,
                                           std::chrono::seconds{10});
    }
    std::filesystem::remove_all(scratch);
    return result;
}

auto has_message(const cppfrontier::translation_result& result, const std::string& text) -> bool {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) {
            return diagnostic.code == "autodiff" &&
                diagnostic.message.find(text) != std::string::npos;
        });
}

const auto modes_program = std::string{
    "fwd: @autodiff type = {\n"
    "    g: (x: double, y: double) -> (r: double) = { r = x * y + sin(x); }\n"
    "}\n"
    "taylor: @autodiff<\"order=2\"> type = {\n"
    "    g: (x: double, y: double) -> (r: double) = { r = x * y + sin(x); }\n"
    "}\n"
    "rev: @autodiff<\"reverse\"> type = {\n"
    "    g: (x: double, y: double) -> (r: double) = { r = x * y + sin(x); }\n"
    "}\n"
    "main: () = {\n"
    "    a := fwd::g_d(2.0, 1.0, 3.0, 0.0);\n"
    "    std::cout << a.r << \" \" << a.r_d << \"\\n\";\n"
    "    b := taylor::g_d(2.0, 1.0, 3.0, 0.0);\n"
    "    std::cout << b.r << \" \" << b.r_d[1] << \" \" << b.r_d[2] << \"\\n\";\n"
    "    x_b := 0.0;\n"
    "    y_b := 0.0;\n"
    "    r_b := 1.0;\n"
    "    c := rev::g_b(2.0, x_b, 3.0, y_b, r_b);\n"
    "    std::cout << c << \" \" << x_b << \" \" << y_b << \" \" << r_b << \"\\n\";\n"
    "}\n"};

}

TEST_CASE("@autodiff generates forward, Taylor, and reverse derivatives", "[autodiff]") {
    const auto result = cppfrontier::translator{}.translate_source(modes_program);
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("#include \"cpp2taylor.h\"") != std::string::npos);
    CHECK(result.cpp1.find("#include \"cpp2ad_stack.h\"") != std::string::npos);
    CHECK(result.cpp1.find("static auto g_d(cpp2::impl::in<double> x, cpp2::impl::in<double> x_d, "
                           "cpp2::impl::in<double> y, cpp2::impl::in<double> y_d) -> g_d_ret") !=
          std::string::npos);
    CHECK(result.cpp1.find("cpp2::impl::in<cpp2::taylor<double, 2>> x_d") != std::string::npos);
    CHECK(result.cpp1.find("static auto g_b(cpp2::impl::in<double> x, double& x_b, "
                           "cpp2::impl::in<double> y, double& y_b, double& r_b)") !=
          std::string::npos);
}

TEST_CASE("@autodiff derivatives compute upstream's values", "[autodiff][runtime]") {
    const auto result = cppfrontier::translator{}.translate_source(modes_program);
    REQUIRE_FALSE(result.has_errors());
    const auto run = run_program(result.cpp1, "modes");
    INFO(run.output);
    REQUIRE(run.exit_status == 0);
    CHECK(run.output == "6.9093 2.58385\n6.9093 2.58385 -0.909297\n6.9093 2.58385 2 0\n");
}

TEST_CASE("@autodiff differentiates called functions and used types", "[autodiff][runtime]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "n: namespace = {\n"
        "    outer: (x: double) -> double = { return x * x; }\n"
        "    point: type = {\n"
        "        public a: double = 0.0;\n"
        "        scale: (this, s: double) -> double = { return s * a; }\n"
        "    }\n"
        "    t: @autodiff type = {\n"
        "        f: (x: double) -> (r: double) = {\n"
        "            p: point = ();\n"
        "            p.a = x;\n"
        "            r = x * outer(x) + p.scale(x);\n"
        "        }\n"
        "    }\n"
        "}\n"
        "main: () = {\n"
        "    v := n::t::f_d(3.0, 1.0);\n"
        "    std::cout << v.r << \" \" << v.r_d << \"\\n\";\n"
        "}\n");
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    CHECK(result.cpp1.find("struct point_d {") != std::string::npos);
    CHECK(result.cpp1.find("outer_d(") != std::string::npos);
    CHECK(result.cpp1.find("scale_d(") != std::string::npos);
    const auto run = run_program(result.cpp1, "uses");
    INFO(run.output);
    REQUIRE(run.exit_status == 0);
    CHECK(run.output == "36 33\n");
}

TEST_CASE("@autodiff diagnoses unsupported arguments and statements", "[autodiff][diagnostics]") {
    const auto unknown = cppfrontier::translator{}.translate_source(
        "t: @autodiff<\"sideways\"> type = { f: (x: double) -> (r: double) = { r = x; } }\n");
    CHECK(has_message(unknown, "AD: Unknown argument: \"sideways\""));
    const auto order = cppfrontier::translator{}.translate_source(
        "t: @autodiff<\"order=x\"> type = { f: (x: double) -> (r: double) = { r = x; } }\n");
    CHECK(has_message(order, "AD: Could not parse derivative order: x"));
    const auto loop = cppfrontier::translator{}.translate_source(
        "t: @autodiff<\"reverse\"> type = {\n"
        "    f: (x: double) -> (r: double) = { r = x; while r < 10.0 { r = r * x; } }\n"
        "}\n");
    CHECK(has_message(loop, "reverse mode for while, do while or for"));
    const auto compound = cppfrontier::translator{}.translate_source(
        "t: @autodiff type = { f: (x: double) -> (r: double) = { r = x; r += x; } }\n");
    CHECK(has_message(compound, "Compound assignments are not yet handled"));
}

TEST_CASE("@autodiff reports diagnostics in derivatives at their origin", "[autodiff][review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "ad: @autodiff type = {\n"
        "    f: (x: double, y: double) -> (r: double) = {\n"
        "        r = x;\n"
        "        i: int;\n"
        "        r = r * y;\n"
        "    }\n"
        "}\n");
    REQUIRE(result.has_errors());
    auto generated = 0;
    for (const auto& diagnostic : result.diagnostics) {
        INFO(diagnostic.span.begin.line << ":" << diagnostic.span.begin.column << ": "
             << diagnostic.message);
        CHECK(diagnostic.span.begin.column < 100);
        if (diagnostic.message.find("generated by a metafunction") != std::string::npos) {
            ++generated;
            CHECK(diagnostic.span.begin.line == 2);
            CHECK(diagnostic.span.begin.column == 5);
        }
    }
    CHECK(generated == 1);
}

TEST_CASE("@autodiff rejects derivatives with incomplete expressions", "[autodiff][review]") {
    for (const auto* function : {"f: (x: int) -> (r: int) = { r = x * 2; }"}) {
        const auto result = cppfrontier::translator{}.translate_source(
            std::string{"ad: @autodiff type = {\n    "} + function + "\n}\n");
        INFO(function);
        CHECK(result.has_errors());
        CHECK(has_message(result, "AD: Generated code is not valid Cpp2"));
    }
}

TEST_CASE("@autodiff reverse mode differentiates literals, copies, locals, and branches",
          "[autodiff][runtime][review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "ad: @autodiff<\"reverse\"> type = {\n"
        "    lit: (x: double, y: double) -> (r: double) = { r = 2.0 * x * y + 1.0; }\n"
        "    copy: (x: double, y: double) -> (r: double) = { r = x; r = r * r; }\n"
        "    locals: (x: double, y: double) -> (r: double) = {\n"
        "        t := x * y; u := t * t; n: int = 3; r = u / x * n;\n"
        "    }\n"
        "    branch: (x: double, y: double) -> (r: double) = {\n"
        "        r = x * y;\n"
        "        if x > y { r = y * y; }\n"
        "        else if x > 0.0 { s := r * x; r = s * y; if y > 2.5 { r = r * x; } }\n"
        "    }\n"
        "}\n"
        "show: (name: std::string, v: double, x_b: double, y_b: double) = {\n"
        "    std::cout << name << \" \" << v << \" \" << x_b << \" \" << y_b << \"\\n\";\n"
        "}\n"
        "main: () = {\n"
        "    x_b := 0.0; y_b := 0.0; r_b := 1.0;\n"
        "    v := ad::lit_b(2.0, x_b, 3.0, y_b, r_b); show(\"lit\", v, x_b, y_b);\n"
        "    x_b = 0.0; y_b = 0.0; r_b = 1.0;\n"
        "    v = ad::copy_b(2.0, x_b, 3.0, y_b, r_b); show(\"copy\", v, x_b, y_b);\n"
        "    x_b = 0.0; y_b = 0.0; r_b = 1.0;\n"
        "    v = ad::locals_b(2.0, x_b, 3.0, y_b, r_b); show(\"locals\", v, x_b, y_b);\n"
        "    x_b = 0.0; y_b = 0.0; r_b = 1.0;\n"
        "    v = ad::branch_b(3.0, x_b, 2.0, y_b, r_b); show(\"then\", v, x_b, y_b);\n"
        "    x_b = 0.0; y_b = 0.0; r_b = 1.0;\n"
        "    v = ad::branch_b(2.0, x_b, 3.0, y_b, r_b); show(\"else\", v, x_b, y_b);\n"
        "    _ = r_b;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    const auto run = run_program(result.cpp1, "reverse");
    INFO(run.output);
    REQUIRE(run.exit_status == 0);
    CHECK(run.output == "lit 13 6 4\ncopy 4 4 0\nlocals 54 27 36\nthen 4 0 4\n"
                        "else 72 108 48\n");
}

TEST_CASE("@autodiff forward mode returns from a branch", "[autodiff][runtime][review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "ad: @autodiff type = {\n"
        "    f: (x: double, y: double) -> double = { if x > 0.0 { return x * x; } return y; }\n"
        "}\n"
        "main: () = {\n"
        "    a := ad::f_d(2.0, 1.0, 3.0, 0.5); std::cout << a.r << \" \" << a.r_d << \"\\n\";\n"
        "    b := ad::f_d(-2.0, 1.0, 3.0, 0.5); std::cout << b.r << \" \" << b.r_d << \"\\n\";\n"
        "}\n");
    REQUIRE_FALSE(result.has_errors());
    const auto run = run_program(result.cpp1, "early");
    INFO(run.output);
    REQUIRE(run.exit_status == 0);
    CHECK(run.output == "4 4\n3 0.5\n");
}

TEST_CASE("@autodiff diagnoses reverse control flow and the return name r",
          "[autodiff][diagnostics][review]") {
    const auto translate = [](const std::string& mode, const std::string& function) {
        return cppfrontier::translator{}.translate_source(
            "ad: @autodiff" + mode + " type = {\n    " + function + "\n}\n");
    };
    CHECK(has_message(translate("<\"reverse\">",
        "f: (x: double) -> double = { if x > 0.0 { return x * x; } return x; }"),
        "reverse mode for a return before the end"));
    CHECK(has_message(translate("<\"reverse\">",
        "f: (x: double) -> (r: double) = { r = x; return; r = x * x; }"),
        "reverse mode for a return before the end"));
    CHECK(has_message(translate("<\"reverse\">",
        "f: (x: double, v: std::vector<double>) -> (r: double) = "
        "{ r = x; for v do (e) { if e > x { break; } } }"),
        "reverse mode for break or continue"));
    CHECK(has_message(translate("<\"reverse\">",
        "f: (x: double) -> (r: double) = { r = x * (-x); }"),
        "reverse mode for prefix operators"));
    CHECK(has_message(translate("<\"reverse\">",
        "f: (x: double, v: std::vector<double>) -> (r: double) = "
        "{ r = x; for v do (e) { r = r * e; } }"),
        "reverse mode for while, do while or for"));
    CHECK(has_message(translate("<\"reverse\">",
        "p: type = { public a: double = 0.0; }\n    "
        "f: (x: double, y: double) -> (r: double) = { t: p = (); t.a = x; r = t.a * y; }"),
        "reverse mode for member access: t.a"));
    for (const auto* function : {"g: (x: double) -> double = { r := x * x; return r + x; }",
                                 "g: (r: double) -> double = { return r * r; }"}) {
        INFO(function);
        CHECK(has_message(translate("", function),
                          "cannot declare `r`, the name of its differentiated return value"));
    }
}

TEST_CASE("@autodiff does not parse a derivative it could not generate", "[autodiff][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "ad: @autodiff type = {\n"
        "    f: (v: std::vector<double>, x: double) -> (r: double) = { r = v[0] * x; }\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    CHECK(has_message(result, "AD: Unknown operator for postfix expression. op: [ expr: v[0]"));
    CHECK_FALSE(has_message(result, "Generated code is not valid"));
}

TEST_CASE("@autodiff reverse mode diagnoses member functions", "[autodiff][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "ns: namespace = {\n"
        "pt: @autodiff<\"reverse\"> type = {\n"
        "    sq: (this, x: double) -> (r: double) = { r = x * x; }\n"
        "}\n"
        "}\n");
    CHECK(has_message(result, "AD: Alpha limitation: reverse mode for member functions: sq"));
    CHECK_FALSE(has_message(result, "Generated code is not valid"));
}

TEST_CASE("@autodiff reverse mode diagnoses calls without a return value", "[autodiff][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "ad: @autodiff<\"reverse\"> type = {\n"
        "    g: (x: double, inout y: double) = { y = x * y; }\n"
        "    f: (x: double) -> (r: double) = { y: double = 2.0; g(x, y); r = y; }\n"
        "}\n");
    CHECK(has_message(result,
                      "AD: Alpha limitation: reverse mode for calls without a return value: g(x, y)"));
    CHECK_FALSE(has_message(result, "Generated code is not valid"));
}

TEST_CASE("@autodiff diagnoses reverse mode with a higher order", "[autodiff][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "ad: @autodiff<\"reverse\", \"order=2\"> type = {\n"
        "    f: (x: double) -> (r: double) = { r = x * x; }\n"
        "}\n");
    CHECK(has_message(result, "AD: Alpha limitation: reverse mode with \"order=2\""));
    CHECK_FALSE(has_message(result, "Generated code is not valid"));
}

TEST_CASE("@autodiff diagnoses a deduced return type", "[autodiff][hunt]") {
    for (const auto* options : {"", "<\"reverse\">", "<\"order=2\">"}) {
        const auto result = cppfrontier::translator{}.translate_source(
            std::string{"ad: @autodiff"} + options + " type = {\n"
            "    h: (x: double) -> _ = { return x * x; }\n"
            "}\n");
        INFO(options);
        CHECK(has_message(result,
                          "AD: Alpha limitation: a deduced return type, declare the return type of h"));
        CHECK_FALSE(has_message(result, "Generated code is not valid"));
    }
}

TEST_CASE("@autodiff differentiates a declared forward return as a value",
          "[autodiff][runtime][hunt]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "ad: @autodiff type = {\n"
        "    f: (inout x: double) -> forward double = { return x; }\n"
        "}\n"
        "main: () = {\n"
        "    x := 3.0; x_d := 2.0;\n"
        "    std::cout << ad::f_d(x, x_d).r_d << \" \" << x << \"\\n\";\n"
        "    _ = x_d;\n"
        "}\n");
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE_FALSE(result.has_errors());
    const auto run = run_program(result.cpp1, "forward_return");
    INFO(run.output);
    REQUIRE(run.exit_status == 0);
    CHECK(run.output == "2 3\n");
}

TEST_CASE("@autodiff diagnoses a function template", "[autodiff][hunt]") {
    for (const auto* options : {"", "<\"reverse\">"}) {
        const auto result = cppfrontier::translator{}.translate_source(
            std::string{"ad: @autodiff"} + options + " type = {\n"
            "    f: <T: type> (x: T) -> T = { return x * x; }\n"
            "}\n");
        INFO(options);
        CHECK(has_message(result, "AD: Alpha limitation: a function template: f"));
        CHECK_FALSE(has_message(result, "Generated code is not valid"));
    }
}
