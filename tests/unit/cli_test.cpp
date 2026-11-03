#include <catch2/catch_all.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

#if CPPFRONTIER_POSIX_PROCESS

namespace {

struct cli_scratch final {
    std::filesystem::path directory;

    cli_scratch() {
        const auto pattern = (std::filesystem::temp_directory_path() / "cppfrontier-cli-XXXXXX").string();
        auto buffer = std::vector<char>{pattern.begin(), pattern.end()};
        buffer.push_back('\0');
        const auto created = ::mkdtemp(buffer.data());
        REQUIRE(created != nullptr);
        directory = created;
    }

    ~cli_scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    cli_scratch(const cli_scratch&) = delete;
    auto operator=(const cli_scratch&) -> cli_scratch& = delete;
};

auto run_cli(const cli_scratch& scratch, std::vector<std::string> arguments)
    -> test_support::process_result {
    arguments.insert(arguments.begin(), CPPFRONTIER_CLI);
    return test_support::run_process(arguments, scratch.directory, std::chrono::seconds{10});
}

}

TEST_CASE("CLI rejects malformed worker counts", "[cli]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    test_support::write_file(input, "main: () -> int = { return 0; }\n");

    for (const auto& value : {"2x", "-1", "0", "999999999999999999999"}) {
        CAPTURE(value);
        const auto result = run_cli(scratch, {"-j", value, input.string()});
        CHECK(result.exit_status == 1);
        CHECK(result.output.find("-j requires a positive integer") != std::string::npos);
    }
}

TEST_CASE("CLI rejects output paths that refer to the input file", "[cli]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    const auto source = std::string{"main: () -> int = { return 0; }\n"};
    test_support::write_file(input, source);

    const auto result = run_cli(scratch, {input.string(), "-o", input.string()});
    CHECK(result.exit_status == 1);
    CHECK(result.output.find("is the input file") != std::string::npos);
    CHECK(test_support::read_file(input) == source);
}

TEST_CASE("CLI writes the default output in the current directory", "[cli]") {
    const auto scratch = cli_scratch{};
    const auto sources = scratch.directory / "sources";
    std::filesystem::create_directory(sources);
    test_support::write_file(sources / "input.cpp2", "main: () -> int = { return 0; }\n");
    test_support::write_file(sources / "lib.h2", "f: () -> int = 1;\n");
    test_support::write_file(sources / "input", "main: () -> int = { return 0; }\n");

    const auto result = run_cli(scratch, {"-cwd", scratch.directory.string(),
        "sources/input.cpp2", "sources/lib.h2"});
    CHECK(result.exit_status == 0);
    CHECK(result.output == "sources/input.cpp2... ok (all Cpp2, passes safety checks)\n\n"
                           "sources/lib.h2... ok (all Cpp2, passes safety checks)\n\n");
    CHECK_FALSE(test_support::read_file(scratch.directory / "input.cpp").empty());
    const auto header = test_support::read_file(scratch.directory / "lib.h");
    CHECK(header.starts_with("#ifndef LIB_H_CPP2\n#define LIB_H_CPP2\n"));
    CHECK(header.ends_with("#endif\n"));

    const auto rejected = run_cli(scratch, {"-cwd", sources.string(), "input"});
    CHECK(rejected.exit_status == 1);
    CHECK(rejected.output.find("error: source filename must end with .cpp2 or .h2") !=
          std::string::npos);
}

TEST_CASE("CLI rejects two inputs with the same output path", "[cli][review]") {
    const auto scratch = cli_scratch{};
    for (const auto* directory : {"1", "2"}) {
        std::filesystem::create_directory(scratch.directory / ("d" + std::string{directory}));
        test_support::write_file(scratch.directory / ("d" + std::string{directory}) / "x.cpp2",
                                 std::string{"main: () -> int = "} + directory + ";\n");
    }
    const auto result = run_cli(scratch, {"-cwd", scratch.directory.string(), "d1/x.cpp2", "d2/x.cpp2"});
    CHECK(result.exit_status == 1);
    CHECK(result.output.find("d1/x.cpp2... ok") != std::string::npos);
    CHECK(result.output.find("d2/x.cpp2: error: output path x.cpp is also the output of d1/x.cpp2") !=
          std::string::npos);
    CHECK(test_support::read_file(scratch.directory / "x.cpp").find("return 1;") != std::string::npos);
}

TEST_CASE("CLI rejects unknown and ambiguous flags as upstream", "[cli]") {
    const auto scratch = cli_scratch{};
    const auto unknown = run_cli(scratch, {"--bad-option", "x.cpp2"});
    CHECK(unknown.exit_status == 1);
    CHECK(unknown.output == "--bad-option - unknown compiler flag name (try -help)\n");

    const auto ambiguous = run_cli(scratch, {"-ver", "x.cpp2"});
    CHECK(ambiguous.exit_status == 1);
    CHECK(ambiguous.output == "-ver - ambiguous compiler flag name, did you mean one of these?\n"
                              "  -vers[ion]\n  -verb[ose]\n");

    const auto unsupported = run_cli(scratch, {"-debug", "x.cpp2"});
    CHECK(unsupported.exit_status == 1);
    CHECK(unsupported.output.find("-debug is not supported") != std::string::npos);

    const auto missing = run_cli(scratch, {"x.cpp2", "-o"});
    CHECK(missing.exit_status == 1);
    CHECK(missing.output.find("missing argument to option -o") != std::string::npos);

    CHECK(run_cli(scratch, {}).exit_status == 1);
    CHECK(run_cli(scratch, {"-vers"}).exit_status == 0);
    CHECK(run_cli(scratch, {"-?"}).output.find("-o[utput] filename") != std::string::npos);
}

TEST_CASE("CLI reports diagnostics in upstream's formats", "[cli]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "bad.cpp2";
    test_support::write_file(input, "main: () = {\n    x := ;\n}\n");
    const auto name = input.string();

    const auto result = run_cli(scratch, {name, "-o", (scratch.directory / "bad.cpp").string()});
    CHECK(result.exit_status == 1);
    CHECK(result.output.starts_with(name + "...\n" + name + "(2,"));
    CHECK(result.output.find("): error: ") != std::string::npos);
    CHECK(result.output.ends_with("\n\n"));

    const auto colon = run_cli(scratch, {"-fo", "-q", name, "-o",
        (scratch.directory / "bad.cpp").string()});
    CHECK(colon.output.starts_with("\n" + name + ":2:"));
}

TEST_CASE("CLI prints warnings after the status line and succeeds", "[cli]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "warn.cpp2";
    const auto output = (scratch.directory / "warn.cpp").string();
    test_support::write_file(input,
        "P: type = { public name: std::string = (); }\n"
        "f: (move p: P) = { x := p.name; y := p.name; }\n");
    const auto name = input.string();
    const auto warning = name + "(2,20): warning: 'p.name' is copied, not moved";

    const auto result = run_cli(scratch, {name, "-o", output});
    CHECK(result.exit_status == 0);
    CHECK(result.output.starts_with(name + "... ok (all Cpp2, passes safety checks)\n" + warning));
    CHECK(result.output.ends_with("to move it\n\n"));
    CHECK_FALSE(test_support::read_file(output).empty());

    const auto pure = run_cli(scratch, {"-p", "-q", name, "-o", output});
    CHECK(pure.exit_status == 0);
    CHECK(pure.output.starts_with(warning));
}

TEST_CASE("CLI applies upstream's output flags", "[cli][options]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    const auto output = scratch.directory / "output.cpp";
    test_support::write_file(input, "f: () -> bool = 1 < 2 <= 3;\n");
    const auto translate = [&](std::vector<std::string> flags) {
        flags.insert(flags.end(), {"-q", input.string(), "-o", output.string()});
        const auto result = run_cli(scratch, flags);
        CHECK(result.exit_status == 0);
        CHECK(result.output.empty());
        return test_support::read_file(output);
    };

    const auto plain = translate({});
    CHECK(plain.find("#line 1 \"" + input.string() + "\"") != std::string::npos);
    CHECK(plain.find("cpp2::impl::cmp_less(_0,_1)") != std::string::npos);
    CHECK(plain.find("CPP2_INCLUDE_STD") == std::string::npos);
    CHECK(translate({"-clean-cpp1"}).find("#line") == std::string::npos);
    CHECK(translate({"-no-comparison-checks"}).find("_0<_1 && _1<=_2") != std::string::npos);
    CHECK(translate({"-include-std"}).find("#define CPP2_INCLUDE_STD         Yes\n") !=
          std::string::npos);
    CHECK(translate({"-im"}).find("#define CPP2_IMPORT_STD          Yes\n") != std::string::npos);
    CHECK(translate({"-im", "-in"}).find("CPP2_IMPORT_STD") == std::string::npos);
    CHECK(translate({"-emit-cppfront-info"}).starts_with("// Generated by cppfrontier "));

    const auto relative = run_cli(scratch, {"-q", "-cwd", scratch.directory.string(),
        "-line-paths", "input.cpp2", "-o", "output.cpp"});
    CHECK(relative.exit_status == 0);
    CHECK(test_support::read_file(output).find("#line 1 \"" +
        std::filesystem::absolute(input).string() + "\"") != std::string::npos);

    const auto to_stdout = run_cli(scratch, {input.string(), "-o", "stdout"});
    CHECK(to_stdout.exit_status == 0);
    CHECK(to_stdout.output.find("auto f() -> bool") != std::string::npos);
    CHECK(to_stdout.output.find("... ok") == std::string::npos);
}

TEST_CASE("CLI -pure-cpp2 rejects Cpp1 lines", "[cli][options]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    const auto output = (scratch.directory / "out.cpp").string();
    test_support::write_file(input, "// comment\nmain: () = { }\n\nint f();\n");
    const auto result = run_cli(scratch, {"-p", input.string(), "-o", output});
    CHECK(result.exit_status == 1);
    CHECK(result.output.find(input.string() +
        "(4,1): error: pure-cpp2 switch disables Cpp1 syntax") != std::string::npos);

    // As upstream, a leading `#include` is Cpp1 syntax only in a file with Cpp2 code.
    test_support::write_file(input, "#include <vector>\nmain: () = { }\n");
    CHECK(run_cli(scratch, {"-p", input.string(), "-o", output}).output.find(
        "(1,1): error: pure-cpp2 switch disables Cpp1 syntax") != std::string::npos);
    test_support::write_file(input, "#include <vector>\n");
    CHECK(run_cli(scratch, {"-p", input.string(), "-o", output}).output.find(
        "(1,1): error: pure-cpp2 switch disables the preprocessor") != std::string::npos);

    test_support::write_file(input, "main: () = { }\n");
    const auto pure = run_cli(scratch, {"-p", input.string(), "-o", output});
    CHECK(pure.exit_status == 0);
    CHECK(test_support::read_file(output).find("#define CPP2_IMPORT_STD") != std::string::npos);
}

TEST_CASE("CLI -pure-cpp2 rejects a .h2 input, whatever the output name", "[cli][options][review]") {
    const auto scratch = cli_scratch{};
    const auto source = scratch.directory / "input.cpp2";
    const auto header = scratch.directory / "lib.h2";
    test_support::write_file(source, "main: () = { }\n");
    test_support::write_file(header, "f: () -> int = 1;\n");
    CHECK(run_cli(scratch, {"-p", source.string(), "-o", (scratch.directory / "bench").string()})
              .exit_status == 0);
    const auto rejected = run_cli(scratch, {"-p", header.string(), "-o",
                                            (scratch.directory / "lib.cpp").string()});
    CHECK(rejected.exit_status == 1);
    CHECK(rejected.output.find("-pure-cpp2 is not supported for headers") != std::string::npos);
}

TEST_CASE("CLI writes a source map", "[cli][source-map]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    const auto output = scratch.directory / "output.cpp";
    const auto map = scratch.directory / "output.map";
    test_support::write_file(input, "\nmain: () -> int = {\n    return 0;\n}\n");
    const auto result = run_cli(scratch, {"-q", "-clean-cpp1", input.string(), "-o",
        output.string(), "-source-map", map.string()});
    REQUIRE(result.exit_status == 0);
    const auto text = test_support::read_file(map);
    CHECK(text.starts_with("{\"version\":1,\"source\":\"" + input.string() +
                           "\",\"output\":\"" + output.string() + "\",\"lines\":["));
    // The definition's lines map to its source lines.
    CHECK(text.ends_with(",2,3,4]}\n"));
    const auto cpp1 = test_support::read_file(output);
    CHECK(std::count(text.begin(), text.end(), ',') ==
          std::count(cpp1.begin(), cpp1.end(), '\n') + 2);
}

TEST_CASE("CLI rejects a source map path that is the input or the output", "[cli][source-map][hunt]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    const auto output = scratch.directory / "input.cpp";
    const auto source = std::string{"main: () -> int = { return 0; }\n"};
    test_support::write_file(input, source);

    auto result = run_cli(scratch, {input.string(), "-source-map", input.string()});
    CHECK(result.exit_status == 1);
    CHECK(result.output.find("source map path " + input.string() + " is the input file") !=
          std::string::npos);
    CHECK(test_support::read_file(input) == source);

    result = run_cli(scratch, {input.string(), "-o", output.string(), "-source-map",
                               (scratch.directory / "." / "input.cpp").string()});
    CHECK(result.exit_status == 1);
    CHECK(result.output.find("is also the output path") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(output));
}

TEST_CASE("CLI reads the arguments after -- as input files", "[cli][hunt]") {
    const auto scratch = cli_scratch{};
    test_support::write_file(scratch.directory / "-input.cpp2", "main: () -> int = { return 0; }\n");

    auto result = run_cli(scratch, {"-cwd", scratch.directory.string(), "--", "-input.cpp2"});
    CHECK(result.exit_status == 0);
    CHECK(std::filesystem::exists(scratch.directory / "-input.cpp"));
    result = run_cli(scratch, {"-cwd", scratch.directory.string(), "-input.cpp2"});
    CHECK(result.exit_status == 1);
    CHECK(result.output.find("unknown compiler flag name") != std::string::npos);
}

TEST_CASE("CLI can disable integer division checks", "[cli][division][options]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    const auto output = scratch.directory / "output.cpp";
    test_support::write_file(input,
        "main: () -> int = { divisor := 0; return 12 / divisor; }\n");

    const auto checked = run_cli(scratch, {input.string(), "-o", output.string()});
    REQUIRE(checked.exit_status == 0);
    CHECK(test_support::read_file(output).find("CPP2_ASSERT_NOT_ZERO") != std::string::npos);

    const auto unchecked = run_cli(scratch,
        {"-no-div-zero-checks", input.string(), "-o", output.string()});
    REQUIRE(unchecked.exit_status == 0);
    CHECK(test_support::read_file(output).find("CPP2_ASSERT_NOT_ZERO") == std::string::npos);
}

TEST_CASE("CLI can disable subscript and null-dereference checks", "[cli][options][mixed]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    const auto output = scratch.directory / "output.cpp";
    test_support::write_file(input, "f: (v: std::vector<int>, p: *int) -> int = v[1] + p*;\n");

    const auto checked = run_cli(scratch, {input.string(), "-o", output.string()});
    REQUIRE(checked.exit_status == 0);
    CHECK(test_support::read_file(output).find(
        "CPP2_ASSERT_IN_BOUNDS_LITERAL(v, 1) + *cpp2::impl::assert_not_null(p)") != std::string::npos);

    const auto unchecked = run_cli(scratch,
        {"-no-s", "-no-null-checks", input.string(), "-o", output.string()});
    REQUIRE(unchecked.exit_status == 0);
    CHECK(test_support::read_file(output).find("v[1] + *p") != std::string::npos);
}

TEST_CASE("CLI propagates restricted-runtime options", "[cli][options][exceptions][rtti]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    const auto output = scratch.directory / "output.cpp";
    test_support::write_file(input, "main: () -> int = { return 0; }\n");

    const auto result = run_cli(scratch,
        {"-fno-exceptions", "-fno-rtti", input.string(), "-o", output.string()});
    REQUIRE(result.exit_status == 0);
    const auto cpp1 = test_support::read_file(output);
    const auto exceptions = cpp1.find("#define CPP2_NO_EXCEPTIONS");
    const auto rtti = cpp1.find("#define CPP2_NO_RTTI");
    const auto include = cpp1.find("#include \"cpp2util.h\"");
    REQUIRE(exceptions != std::string::npos);
    REQUIRE(rtti != std::string::npos);
    REQUIRE(include != std::string::npos);
    CHECK(exceptions < include);
    CHECK(rtti < include);
}

TEST_CASE("CLI writes to a device such as /dev/null", "[cli]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    test_support::write_file(input, "main: () -> int = { return 0; }\n");

    const auto result = run_cli(scratch, {input.string(), "-o", "/dev/null"});
    CHECK(result.exit_status == 0);
    CHECK(result.output.find("could not open") == std::string::npos);
}

TEST_CASE("CLI writes through a symbolic link and leaves other files alone", "[cli]") {
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    test_support::write_file(input, "main: () -> int = { return 0; }\n");
    const auto target = scratch.directory / "target.cpp";
    const auto link = scratch.directory / "link.cpp";
    test_support::write_file(target, "old\n");
    std::filesystem::create_symlink("target.cpp", link);
    test_support::write_file(scratch.directory / "link.cpp.tmp", "keep\n");
    test_support::write_file(scratch.directory / "target.cpp.tmp", "keep\n");

    const auto result = run_cli(scratch, {input.string(), "-o", link.string()});
    CHECK(result.exit_status == 0);
    CHECK(std::filesystem::is_symlink(link));
    CHECK(test_support::read_file(target).find("auto main() -> int") != std::string::npos);
    CHECK(test_support::read_file(scratch.directory / "link.cpp.tmp") == "keep\n");
    CHECK(test_support::read_file(scratch.directory / "target.cpp.tmp") == "keep\n");
    CHECK(std::count_if(std::filesystem::directory_iterator{scratch.directory},
                        std::filesystem::directory_iterator{}, [](const auto& entry) {
                            return entry.path().extension() == ".tmp";
                        }) == 2);
}

TEST_CASE("CLI keeps an output file's mode and links, and writes it in a read-only directory",
          "[cli][review]") {
    namespace fs = std::filesystem;
    const auto scratch = cli_scratch{};
    const auto input = scratch.directory / "input.cpp2";
    test_support::write_file(input, "main: () -> int = { return 0; }\n");
    const auto written = [](const fs::path& path) {
        return test_support::read_file(path).find("auto main() -> int") != std::string::npos;
    };

    const auto executable = scratch.directory / "mode.cpp";
    test_support::write_file(executable, "old\n");
    fs::permissions(executable, fs::perms::owner_all | fs::perms::group_read);
    CHECK(run_cli(scratch, {input.string(), "-o", executable.string()}).exit_status == 0);
    CHECK(written(executable));
    CHECK(fs::status(executable).permissions() == (fs::perms::owner_all | fs::perms::group_read));

    const auto linked = scratch.directory / "linked.cpp";
    const auto other = scratch.directory / "other.cpp";
    test_support::write_file(linked, "old\n");
    fs::create_hard_link(linked, other);
    CHECK(run_cli(scratch, {input.string(), "-o", linked.string()}).exit_status == 0);
    CHECK(written(other));

    const auto locked = scratch.directory / "locked";
    fs::create_directory(locked);
    const auto inside = locked / "out.cpp";
    test_support::write_file(inside, "old\n");
    fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
    const auto result = run_cli(scratch, {input.string(), "-o", inside.string()});
    fs::permissions(locked, fs::perms::owner_all);
    CHECK(result.exit_status == 0);
    CHECK(written(inside));
}

#endif
