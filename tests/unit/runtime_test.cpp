#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

namespace {

struct runtime_case final {
    const char* name;
    std::filesystem::path source_path;
    std::string expected_output;
    int exit_status = 0;
};

}

// Sharded so that `ctest -j` runs the programs in parallel.
TEMPLATE_TEST_CASE("curated upstream programs translate and run", "[runtime][upstream]",
                   (std::integral_constant<int, 0>), (std::integral_constant<int, 1>)) {
    constexpr auto shard_count = 2;
    const auto test_directory = std::filesystem::path{CPPFRONTIER_UPSTREAM_DIR} / "regression-tests";
    const auto cases = std::vector<runtime_case>{
        {"pure2-hello.cpp2", test_directory / "pure2-hello.cpp2", "Hello [world]\n"},
        {"pure2-synthesize-rightshift-and-rightshifteq.cpp2",
            test_directory / "pure2-synthesize-rightshift-and-rightshifteq.cpp2", "4\n"},
        {"pure2-function-multiple-forward-arguments.cpp2",
            test_directory / "pure2-function-multiple-forward-arguments.cpp2", "abc\n"},
        {"pure2-bounds-safety-span.cpp2", test_directory / "pure2-bounds-safety-span.cpp2",
            ">> decorated\n>> hello\n>> world\n"},
        {"pure2-intro-example-three-loops.cpp2",
            test_directory / "pure2-intro-example-three-loops.cpp2",
            ">> hello\n>> big\n>> world\n  **  **  **\n"
            ">> [hello]\n>> [big]\n>> [world]\n>> end of program\n"},
        {"pure2-break-continue.cpp2", test_directory / "pure2-break-continue.cpp2",
            "while_continue_inner:\n  00 inner 01 02 inner outer "
            "10 inner 11 12 inner outer 20 inner 21 22 inner outer \n"
            "while_continue_outer:\n  00 inner 01 10 inner 11 20 inner 21 \n"
            "while_break_inner:\n  00 inner 01 outer 10 inner 11 outer "
            "20 inner 21 outer \n"
            "while_break_outer:\n  00 inner 01 \n"
            "\ndo_continue_inner:\n  00 inner 01 02 inner outer "
            "10 inner 11 12 inner outer 20 inner 21 22 inner outer \n"
            "do_continue_outer:\n  00 inner 01 10 inner 11 20 inner 21 \n"
            "do_break_inner:\n  00 inner 01 outer 10 inner 11 outer "
            "20 inner 21 outer \n"
            "do_break_outer:\n  00 inner 01 \n"
            "\nfor_continue_inner:\n  00 inner 01 02 inner outer "
            "10 inner 11 12 inner outer 20 inner 21 22 inner outer \n"
            "for_continue_outer:\n  00 inner 01 10 inner 11 20 inner 21 \n"
            "for_break_inner:\n  00 inner 01 outer 10 inner 11 outer "
            "20 inner 21 outer \n"
            "for_break_outer:\n  00 inner 01 "},
        {"mixed-hello.cpp2", test_directory / "mixed-hello.cpp2", "Hello [world]\n"},
    };

    auto index = 0;
    for (const auto& runtime_case : cases) {
        if (index++ % shard_count != TestType::value) continue;
        CAPTURE(runtime_case.name);
        auto source = test_support::read_file(runtime_case.source_path);
        if (source.find("#define CPP2_INCLUDE_STD Yes") == std::string::npos) {
            source.insert(0, "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n");
        }
        const auto translation = cppfrontier::translator{}.translate_source(source);

        REQUIRE_FALSE(translation.cpp1.empty());
        CHECK(std::none_of(translation.diagnostics.begin(), translation.diagnostics.end(),
            [](const auto& issue) { return issue.severity == cppfrontier::diagnostic_severity::error; }));

        const auto compiler = std::string{CPPFRONTIER_GXX};
        if (compiler.empty()) {
            SKIP("g++ is unavailable");
        }

        const auto scratch = test_support::scratch_directory("cppfrontier-runtime");
        std::filesystem::create_directories(scratch);
        const auto cpp1_path = scratch / (std::filesystem::path{runtime_case.name}.stem().string() + ".cpp");
        const auto executable_path = scratch / std::filesystem::path{runtime_case.name}.stem();
        test_support::write_file(cpp1_path, translation.cpp1);

        const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
            CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
            std::chrono::seconds{60});
        INFO("compiler output: " << compiler_result.output);
        REQUIRE_FALSE(compiler_result.timed_out);
        REQUIRE(compiler_result.exit_status == 0);

        const auto run_result = test_support::run_process(
            {executable_path.string()}, scratch, std::chrono::seconds{10});
        INFO("program output: " << run_result.output);
        INFO("program output size: " << run_result.output.size());
        INFO("expected output size: " << runtime_case.expected_output.size());
        REQUIRE_FALSE(run_result.timed_out);
        CHECK(run_result.exit_status == runtime_case.exit_status);
        CHECK(std::equal(runtime_case.expected_output.begin(), runtime_case.expected_output.end(),
            run_result.output.begin(), run_result.output.end()));
    }
}

TEST_CASE("expression bodies and copy parameters compile and run", "[runtime][expression-body]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "#include <string>\n"
        "sum: (a: int, b: int) = a + b;\n"
        "copy_text: (copy text: std::string) -> std::string = text;\n"
        "main: () -> int = {\n"
        "    std::cout << copy_text(\"hello\") << ' ' << sum(40, 2) << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-expression-body");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "expression-body.cpp";
    const auto executable_path = scratch / "expression-body";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "hello 42\n");
}

TEST_CASE("template aliases compile and run", "[runtime][aliases][templates]") {
    const auto source = std::string{
        "#include <vector>\n"
        "box: <T> type == std::vector<T>;\n"
        "main: () -> int = {\n"
        "    cards: box<int> = (1, 2, 3);\n"
        "    return cpp2::unchecked_narrow<int>(cards.size()) - 3;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find("template<typename T> using box = std::vector<T>;") !=
          std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-local-alias");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "local-alias.cpp";
    const auto executable_path = scratch / "local-alias";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
}

TEST_CASE("string interpolation compiles and preserves literal text",
          "[runtime][interpolation]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "main: () -> int = {\n"
        "    x := 5;\n"
        "    std::cout << \"a (x)$ b; (x)$(x)$; (x:04d)$; (x:)$; $5; \\\"(x)$\\\"\\n\";\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-interpolation");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "interpolation.cpp";
    const auto executable_path = scratch / "interpolation";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
        scratch, std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "a 5 b; 55; 0005; 5; $5; \"5\"\n");
}

TEST_CASE("move this operator= compiles and runs as a destructor",
          "[runtime][types][destructor]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "Widget: type = {\n"
        "    operator=: (move this) = { std::cout << \"destroyed\\n\"; }\n"
        "}\n"
        "main: () -> int = {\n"
        "    widget: Widget = ( );\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-destructor");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "destructor.cpp";
    const auto executable_path = scratch / "destructor";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
        scratch, std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "destroyed\n");
}

TEST_CASE("explicit this member access compiles and runs", "[runtime][types][this]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "Widget: type = {\n"
        "    value: int = 0;\n"
        "    set: (inout this, next: int) = { this.value = next; }\n"
        "    get: (this) -> int = this.value;\n"
        "}\n"
        "main: () -> int = {\n"
        "    widget: Widget = ( );\n"
        "    widget.set(42);\n"
        "    std::cout << widget.get() << '\\n';\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find("(*this).value") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-this");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "this.cpp";
    const auto executable_path = scratch / "this";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
        scratch, std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "42\n");
}

TEST_CASE("range expressions compile and run with negative bounds", "[runtime][loops][range]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "main: () -> int = {\n"
        "    for -2 ..< 2 do (value) { std::cout << value; }\n"
        "    std::cout << '\\n';\n"
        "    for 1 ..= 3 do (value) { std::cout << value; }\n"
        "    std::cout << '\\n';\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find("cpp2::range(-2,2)") != std::string::npos);
    CHECK(translation.cpp1.find("cpp2::range(1,3,true)") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-range-expressions");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "range-expressions.cpp";
    const auto executable_path = scratch / "range-expressions";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
        scratch, std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "-2-101\n123\n");
}

TEST_CASE("typed out parameters compile and run", "[runtime][parameters][out]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "fill: (out value: int) = { value = 42; }\n"
        "main: () -> int = {\n"
        "    value: int = 0;\n"
        "    fill(out value);\n"
        "    std::cout << value << '\\n';\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find("cpp2::impl::out<int> value") != std::string::npos);
    CHECK(translation.cpp1.find("value.construct(std::type_identity_t<int>{42})") !=
          std::string::npos);
    CHECK(translation.cpp1.find("cpp2::impl::out(&value)") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-out");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "out.cpp";
    const auto executable_path = scratch / "out";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
        scratch, std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "42\n");
}

TEST_CASE("deferred local initialization narrows literal assignments",
          "[runtime][initializers][deferred]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "main: () -> int = {\n"
        "    small: u8;\n"
        "    small = 'a';\n"
        "    medium: i16;\n"
        "    medium = 3;\n"
        "    std::cout << (small as int) << ':' << medium << '\\n';\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find("cpp2::impl::deferred_init<unsigned char> small") !=
          std::string::npos);
    CHECK(translation.cpp1.find("small.construct(std::type_identity_t<unsigned char>{'a'})") !=
          std::string::npos);
    CHECK(translation.cpp1.find("medium.construct(std::type_identity_t<short>{3})") !=
          std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-deferred");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "deferred.cpp";
    const auto executable_path = scratch / "deferred";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()},
        scratch, std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "97:3\n");
}

TEST_CASE("try and catch handlers compile and run", "[runtime][exceptions]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "#include <stdexcept>\n"
        "main: () -> int = {\n"
        "    try {\n"
        "        throw(42);\n"
        "    } catch (i: int) {\n"
        "        std::cout << \"int \" << i << '\\n';\n"
        "    } catch (_) {\n"
        "        std::cout << \"other\\n\";\n"
        "    }\n"
        "    try {\n"
        "        throw(std::runtime_error(\"boom\"));\n"
        "    } catch (error: std::runtime_error) {\n"
        "        std::cout << error.what() << '\\n';\n"
        "    }\n"
        "    try {\n"
        "        throw(1.5);\n"
        "    } catch (_) {\n"
        "        std::cout << \"other\\n\";\n"
        "    }\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-exceptions");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "exceptions.cpp";
    const auto executable_path = scratch / "exceptions";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "int 42\nboom\nother\n");
}

TEST_CASE("function-local statics persist across calls", "[runtime][locals][static]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "next_value: () -> int = {\n"
        "    static counter: int = 0;\n"
        "    static deduced := 0;\n"
        "    ordinary := 0;\n"
        "    counter += 1;\n"
        "    deduced += 1;\n"
        "    ordinary += 1;\n"
        "    return counter * 100 + deduced * 10 + ordinary;\n"
        "}\n"
        "main: () -> int = {\n"
        "    first := next_value();\n"
        "    second := next_value();\n"
        "    std::cout << first << ' ' << second << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-local-static");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "local-static.cpp";
    const auto executable_path = scratch / "local-static";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "111 221\n");
}

TEST_CASE("using declarations compile and run", "[runtime][using][namespaces]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "values: namespace = { answer: int = 42; }\n"
        "main: () -> int = {\n"
        "    using values::answer;\n"
        "    std::cout << answer << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-using");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "using.cpp";
    const auto executable_path = scratch / "using";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "42\n");
}

TEST_CASE("template parameters compile and run", "[runtime][templates]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "f1: <T, U>           (t: T, u: U) = t + u;\n"
        "f2: <T: type, U: type> (t: T, u: U) = t + u;\n"
        "f3: <T: _, U: _>       ()         = T + U;\n"
        "f4: <T: i8, U: i16>    ()         = T + U;\n"
        "main: () -> int = {\n"
        "    std::cout << f1(1, 1) << ' ' << f2(2, 2) << ' '\n"
        "              << f3<3, 3>() << ' ' << f4<4, 4>() << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-templates");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "templates.cpp";
    const auto executable_path = scratch / "templates";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "2 4 6 8\n");
}

TEST_CASE("allocator-qualified allocation compiles and runs", "[runtime][allocation]") {
    const auto source = std::string{
        "#include \"cpp2util.h\"\n"
        "#include <iostream>\n"
        "#include <memory>\n"
        "main: () -> int = {\n"
        "    up := unique.new<int>(20);\n"
        "    sp := cpp2::shared.new<int>(22);\n"
        "    std::cout << up* + sp* << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-allocation");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "allocation.cpp";
    const auto executable_path = scratch / "allocation";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "42\n");
}

TEST_CASE("reference-forced parameters and forwarding returns compile and run", "[runtime][parameters][returns]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "minimum: (in_ref left: int, in_ref right: int) -> forward_ref _ = {\n"
        "    if right < left { return right; }\n"
        "    return left;\n"
        "}\n"
        "main: () -> int = {\n"
        "    left := 41;\n"
        "    right := 42;\n"
        "    std::cout << minimum(left, right) << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-reference-forced");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "reference-forced.cpp";
    const auto executable_path = scratch / "reference-forced";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "41\n");
}

TEST_CASE("range for next runs after continue and skips after break", "[runtime][loops][next]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "#include <vector>\n"
        "main: () -> int = {\n"
        "    values: std::vector = (1, 2, 3);\n"
        "    count := 0;\n"
        "    for values next count += 10 do (value) {\n"
        "        if value == 2 { continue; }\n"
        "        if value == 3 { break; }\n"
        "        count += 1;\n"
        "    }\n"
        "    std::cout << count << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find("for (auto const& value : CPP2_FORWARD(values))") != std::string::npos);
    CHECK(translation.cpp1.find("if (__cpp2_for_break_") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-range-next");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "range-next.cpp";
    const auto executable_path = scratch / "range-next";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "21\n");
}

TEST_CASE("range for parameter passing styles control mutation", "[runtime][loops][parameters]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "#include <vector>\n"
        "main: () -> int = {\n"
        "    values: std::vector = (1, 2, 3);\n"
        "    for values do (inout value) { value *= 2; }\n"
        "    total := 0;\n"
        "    for values do (copy value) { total += value; }\n"
        "    std::cout << values[0] << values[1] << values[2] << ' '\n"
        "              << total << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find("for (auto& value : values)") != std::string::npos);
    CHECK(translation.cpp1.find("for (auto value : values)") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-range-parameters");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "range-parameters.cpp";
    const auto executable_path = scratch / "range-parameters";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "246 12\n");
}

TEST_CASE("unbraced range-for bodies execute and honor next", "[runtime][loops][upstream-1426]") {
    const auto source = std::string{
        "#include <iostream>\n"
        "#include <vector>\n"
        "main: () -> int = {\n"
        "    values: std::vector = (0, 1, 2, 3);\n"
        "    count := 0;\n"
        "    for values next count += 10 do (value)\n"
        "    if value % 2 == 0 { count += value; }\n"
        "    std::cout << count << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-unbraced-range");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "unbraced-range.cpp";
    const auto executable_path = scratch / "unbraced-range";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "42\n");
}

TEST_CASE("postfix pointer dereference preserves increment semantics",
          "[runtime][expressions][pointers]") {
    const auto source = std::string{
        "#define CPP2_INCLUDE_STD Yes\n"
        "#include \"cpp2util.h\"\n"
        "#include <iostream>\n"
        "main: () -> int = {\n"
        "    value := new<int>(0);\n"
        "    value*++;\n"
        "    std::cout << value* << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find("++*cpp2::impl::assert_not_null(value)") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-pointer-increment");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "pointer-increment.cpp";
    const auto executable_path = scratch / "pointer-increment";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "1\n");
}

TEST_CASE("postfix address-of creates a usable pointer",
          "[runtime][expressions][address-of]") {
    const auto source = std::string{
        "read: (value: *int) -> int = value*;\n"
        "main: () -> int = {\n"
        "    value := 4;\n"
        "    pointer := value&;\n"
        "    pointer* += 1;\n"
        "    if read(pointer) == 5 { return 0; } return 1;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find("&(value)") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-address-of");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "address-of.cpp";
    const auto executable_path = scratch / "address-of";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
}

TEST_CASE("main args receives the Cpp1 command line", "[runtime][main][parameters]") {
    const auto source = std::string{
        "#define CPP2_INCLUDE_STD Yes\n"
        "#include \"cpp2util.h\"\n"
        "#include <iostream>\n"
        "main: (args) -> int = {\n"
        "    std::cout << args.ssize() << ':' << args[0] << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find("auto main(int const argc_, char** argv_) -> int") !=
          std::string::npos);
    CHECK(translation.cpp1.find("auto const args = cpp2::make_args(argc_, argv_);") !=
          std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-main-args");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "main_args.cpp";
    const auto executable_path = scratch / "main_args";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiler_result = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiler_result.output);
    REQUIRE_FALSE(compiler_result.timed_out);
    REQUIRE(compiler_result.exit_status == 0);

    const auto run_result = test_support::run_process(
        {executable_path.string(), "first", "second"}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run_result.output);
    REQUIRE_FALSE(run_result.timed_out);
    CHECK(run_result.exit_status == 0);
    CHECK(run_result.output == "3:" + executable_path.string() + "\n");
}

TEST_CASE("function types compile and run in documented forms",
          "[runtime][types][functions]") {
    const auto source = std::string{
        "decorate_int: (i: i32) -> std::string = \"--> (i)$ <--\";\n"
        "main: () -> int = {\n"
        "    pf1: std::function< (i: i32) -> std::string > = decorate_int&;\n"
        "    pf2: * (i: i32) -> std::string = decorate_int&;\n"
        "    std::cout << pf1(123) << '\\n' << pf2(456) << '\\n';\n"
        "    return 0;\n"
        "}\n"};
    const auto translation = cppfrontier::translator{}.translate_source(source);
    INFO(translation.cpp1);
    for (const auto& diagnostic : translation.diagnostics) {
        INFO(diagnostic.code << ": " << diagnostic.message);
    }
    REQUIRE_FALSE(translation.has_errors());
    CHECK(translation.cpp1.find(
        "std::function<std::string(cpp2::impl::in<int> i)> pf1") != std::string::npos);
    CHECK(translation.cpp1.find(
        "std::string(*pf2)(cpp2::impl::in<int> i)") != std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-function-types");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "function_types.cpp";
    const auto executable_path = scratch / "function_types";
    test_support::write_file(cpp1_path, translation.cpp1);

    const auto compiled = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiled.output);
    REQUIRE_FALSE(compiled.timed_out);
    REQUIRE(compiled.exit_status == 0);

    const auto run = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run.output);
    REQUIRE_FALSE(run.timed_out);
    CHECK(run.exit_status == 0);
    CHECK(run.output == "--> 123 <--\n--> 456 <--\n");
}

TEST_CASE("integer division and modulo are checked at run time",
          "[runtime][expressions][division][safety]") {
    const auto translated = cppfrontier::translator{}.translate_source(
        "main: () -> int = {\n"
        "    zero := 0;\n"
        "    seven := 7;\n"
        "    return seven / zero;\n"
        "}\n");
    INFO(translated.cpp1);
    REQUIRE_FALSE(translated.has_errors());
    CHECK(translated.cpp1.find("CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(cpp2::move(seven)),cpp2::move(zero))") !=
          std::string::npos);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-division-check");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "division_check.cpp";
    const auto executable_path = scratch / "division_check";
    test_support::write_file(cpp1_path, translated.cpp1);

    const auto compiled = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << compiled.output);
    REQUIRE_FALSE(compiled.timed_out);
    REQUIRE(compiled.exit_status == 0);

    const auto run = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run.output);
    REQUIRE_FALSE(run.timed_out);
    CHECK(run.exit_status != 0);

    const auto unchecked = cppfrontier::translator{}.translate_source(
        "main: () -> int = {\n"
        "    divisor := 3;\n"
        "    value := 12;\n"
        "    value /= divisor;\n"
        "    value %= 5;\n"
        "    return value;\n"
        "}\n");
    CHECK(unchecked.cpp1.find("value /= CPP2_ASSERT_NOT_ZERO") != std::string::npos);
    CHECK(unchecked.cpp1.find("value %= CPP2_ASSERT_NOT_ZERO_LITERAL") != std::string::npos);

    const auto trusted = cppfrontier::translator{}.translate_source(
        "main: () -> int = {\n"
        "    divisor := 1;\n"
        "    value := unchecked_div(1, divisor);\n"
        "    return value / 1;\n"
        "}\n");
    INFO(trusted.cpp1);
    REQUIRE_FALSE(trusted.has_errors());
    CHECK(trusted.cpp1.find("cpp2::unchecked_div(1, cpp2::move(divisor))") != std::string::npos);
    CHECK(trusted.cpp1.find("CPP2_ASSERT_NOT_ZERO_LITERAL(CPP2_TYPEOF(cpp2::move(value)),1)") !=
          std::string::npos);
    test_support::write_file(cpp1_path, trusted.cpp1);
    const auto trusted_compiled = test_support::run_process({compiler, "-std=c++23", "-I",
        CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(), cpp1_path.string()}, scratch,
        std::chrono::seconds{60});
    INFO("compiler output: " << trusted_compiled.output);
    REQUIRE_FALSE(trusted_compiled.timed_out);
    REQUIRE(trusted_compiled.exit_status == 0);
    const auto trusted_run = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << trusted_run.output);
    REQUIRE_FALSE(trusted_run.timed_out);
    CHECK(trusted_run.exit_status == 1);

    auto options = cppfrontier::translation_options{};
    options.check_division_by_zero = false;
    const auto disabled = cppfrontier::translator{}.translate_source(
        "main: () -> int = { divisor := 3; return 12 / divisor; }\n", options);
    REQUIRE_FALSE(disabled.has_errors());
    CHECK(disabled.cpp1.find("CPP2_ASSERT_NOT_ZERO") == std::string::npos);
}

TEST_CASE("restricted runtime options compile with matching compiler flags",
          "[runtime][options][exceptions][rtti]") {
    auto options = cppfrontier::translation_options{};
    options.exceptions = false;
    options.rtti = false;
    const auto translated = cppfrontier::translator{}.translate_source(
        "main: () -> int = { return 0; }\n", options);
    INFO(translated.cpp1);
    REQUIRE_FALSE(translated.has_errors());

    const auto exceptions = translated.cpp1.find("#define CPP2_NO_EXCEPTIONS");
    const auto rtti = translated.cpp1.find("#define CPP2_NO_RTTI");
    const auto support_header = translated.cpp1.find("#include \"cpp2util.h\"");
    REQUIRE(exceptions != std::string::npos);
    REQUIRE(rtti != std::string::npos);
    REQUIRE(support_header != std::string::npos);
    CHECK(exceptions < support_header);
    CHECK(rtti < support_header);

    const auto compiler = std::string{CPPFRONTIER_GXX};
    if (compiler.empty()) SKIP("g++ is unavailable");

    const auto scratch = test_support::scratch_directory("cppfrontier-restricted-runtime");
    std::filesystem::create_directories(scratch);
    const auto cpp1_path = scratch / "restricted_runtime.cpp";
    const auto executable_path = scratch / "restricted_runtime";
    test_support::write_file(cpp1_path, translated.cpp1);

    const auto compiled = test_support::run_process({compiler, "-std=c++23", "-fno-exceptions",
        "-fno-rtti", "-I", CPPFRONTIER_RUNTIME_INCLUDE, "-o", executable_path.string(),
        cpp1_path.string()}, scratch, std::chrono::seconds{60});
    INFO("compiler output: " << compiled.output);
    REQUIRE_FALSE(compiled.timed_out);
    REQUIRE(compiled.exit_status == 0);

    const auto run = test_support::run_process(
        {executable_path.string()}, scratch, std::chrono::seconds{10});
    INFO("program output: " << run.output);
    REQUIRE_FALSE(run.timed_out);
    CHECK(run.exit_status == 0);
}
