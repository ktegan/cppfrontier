#include <cppfrontier/json.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <ranges>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

using namespace cppfrontier;

namespace {

// Upstream cppfront's command line: each flag can be abbreviated to any
// prefix that is unique among the flags, and the part of a flag's name after
// a space names its value.
enum class flag_id : std::uint8_t {
    help, version, gen_build, verbose, internal_debug, colon_errors, debug, quiet, emit_info,
    clean_cpp1, line_paths, import_std, include_std, pure_cpp2, no_null_checks,
    no_div_zero_checks, no_subscript_checks, no_comparison_checks, output, cwd, no_exceptions,
    no_rtti, jobs, source_map,
};

struct flag final {
    int group = 0;
    std::string_view name;
    std::string_view description;
    flag_id id = flag_id::help;
    std::string_view synonym = {};
    std::size_t unique_prefix = 0;

    [[nodiscard]] auto takes_value() const -> bool {
        return name.find(' ') != std::string_view::npos;
    }

    // The name with the optional part of its spelling in brackets.
    [[nodiscard]] auto display_name() const -> std::string {
        const auto length = std::min(name.find(' '), name.size());
        auto result = std::string{name.substr(0, unique_prefix)};
        if (unique_prefix < length) {
            result += "[" + std::string{name.substr(unique_prefix, length - unique_prefix)} + "]";
        }
        return result + std::string{name.substr(length)};
    }
};

auto make_flags() -> std::vector<flag> {
    auto flags = std::vector<flag>{
        {0, "help", "Print help", flag_id::help, "?"},
        {0, "version", "Print version information", flag_id::version},
        {0, "_gen_build", "Generate build information", flag_id::gen_build},
        {9, "verbose", "Print verbose output and statistics", flag_id::verbose},
        {0, "_debug", "Generate internal debug data, implies -verbose", flag_id::internal_debug},
        {9, "format-colon-errors", "Emit ':line:col:' format for messages - lights up some tools",
         flag_id::colon_errors},
        {9, "debug", "Emit compiler debug output (not supported)", flag_id::debug},
        {9, "quiet", "Print only error output", flag_id::quiet},
        {8, "emit-cppfront-info", "Emit cppfrontier version in Cpp1 file", flag_id::emit_info},
        {8, "clean-cpp1", "Emit clean Cpp1 without #line directives", flag_id::clean_cpp1},
        {8, "line-paths", "Emit absolute paths in #line directives", flag_id::line_paths},
        {0, "import-std", "import all std:: via 'import std;' - ignored if -include-std is set",
         flag_id::import_std},
        {0, "include-std", "#include all std:: headers", flag_id::include_std},
        {0, "pure-cpp2", "Allow Cpp2 syntax only - also sets -import-std", flag_id::pure_cpp2},
        {2, "no-null-checks", "Disable null safety checks", flag_id::no_null_checks},
        {2, "no-div-zero-checks", "Disable integer division by zero checks",
         flag_id::no_div_zero_checks},
        {2, "no-subscript-checks", "Disable subscript safety checks", flag_id::no_subscript_checks},
        {2, "no-comparison-checks", "Disable mixed-sign comparison safety checks",
         flag_id::no_comparison_checks},
        {9, "output filename", "Output to 'filename' (can be 'stdout') - default is *.cpp/*.h",
         flag_id::output},
        {9, "cwd path", "Change current working directory to path", flag_id::cwd},
        {4, "fno-exceptions", "Disable C++ EH - failed 'as' for 'variant' will assert",
         flag_id::no_exceptions},
        {4, "fno-rtti", "Disable C++ RTTI - using 'as' for '*'/'std::any' will assert",
         flag_id::no_rtti},
        {9, "jobs count", "Translate up to 'count' files in parallel - default is the CPU count",
         flag_id::jobs},
        {9, "source-map filename", "Write a JSON map of output lines to source lines",
         flag_id::source_map},
    };
    for (auto first = flags.begin(); first != flags.end(); ++first) {
        for (auto second = first + 1; second != flags.end(); ++second) {
            auto shared = std::size_t{0};
            while (shared < first->name.size() && shared < second->name.size() &&
                   first->name[shared] != ' ' && second->name[shared] != ' ' &&
                   first->name[shared] == second->name[shared]) {
                ++shared;
            }
            first->unique_prefix = std::max(first->unique_prefix, shared + 1);
            second->unique_prefix = std::max(second->unique_prefix, shared + 1);
        }
    }
    return flags;
}

// The flag that `argument` (`-name` or `/name`) spells, matched as upstream:
// all of it but one trailing character, and at least the unique prefix, must
// begin the flag's name.
auto match_flag(const std::vector<flag>& flags, std::string_view argument) -> const flag* {
    if (argument.size() < 2 || (argument[0] != '-' && argument[0] != '/')) return nullptr;
    const auto text = argument.substr(1);
    for (const auto& candidate : flags) {
        const auto length = std::max(candidate.unique_prefix, argument.size() - 1);
        if (text.starts_with(candidate.name.substr(0, length)) ||
            (!candidate.synonym.empty() && text == candidate.synonym)) {
            return &candidate;
        }
    }
    return nullptr;
}

auto print_help(std::vector<flag> flags) -> void {
    std::sort(flags.begin(), flags.end(), [](const flag& left, const flag& right) {
        return left.group < right.group || (left.group == right.group && left.name < right.name);
    });
    auto width = std::size_t{0};
    for (const auto& value : flags) width = std::max(width, value.name.size());
    const auto label = [](int group) -> std::string_view {
        switch (group) {
        case 2: return "Additional dynamic safety check controls";
        case 4: return "Support for constrained target environments";
        case 8: return "Cpp1 file content options";
        case 9: return "Cppfront output options";
        default: return "";
        }
    };
    std::cout << "\nUsage: cppfrontier [options] file ...\n"
                 "\n  file                    source file(s) (can be 'stdin')\n"
                 "\nOptions: \n";
    auto group = -1;
    for (const auto& value : flags) {
        if (value.name.front() == '_') continue;
        if (value.group != group) {
            group = value.group;
            std::cout << '\n';
            if (!label(group).empty()) std::cout << label(group) << '\n';
        }
        auto name = value.display_name();
        if (!value.synonym.empty()) name += ", -" + std::string{value.synonym};
        std::cout << "  -" << std::setw(static_cast<int>(width + 3)) << std::left << name
                  << value.description << '\n';
    }
    std::cout << "\nLanguage experiments, off by default (not in cppfront)\n"
              << "  -" << std::setw(static_cast<int>(width + 3)) << std::left << "experiment=name"
              << "Enable experiment 'name' - 'list' lists them, 'all' enables all\n";
}

auto print_experiments() -> void {
    for (const auto& value : all_experiments) {
        std::cout << value.name << "\n    " << value.description << " (" << value.decision;
        for (const auto base : value.builds_on) {
            if (!base.empty()) std::cout << "; builds on " << base;
        }
        std::cout << ")\n";
    }
}

auto print_version() -> void {
    std::cout << "\ncppfrontier compiler v" CPPFRONTIER_VERSION
                 "\nA Cpp2 to Cpp1 translator with cppfront's command line\n";
}

struct settings final {
    translation_options translation;
    std::string output;
    std::string source_map;
    std::filesystem::path cwd;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    bool quiet = false;
    bool verbose = false;
    bool colon_errors = false;
    bool line_paths = false;
    bool help = false;
};

auto error(std::string_view message) -> int {
    std::cerr << "cppfrontier: error: " << message << '\n';
    return 1;
}

// Applies flag `id`, spelled `text`, with `argument` if it takes one; returns
// an exit status if the command line is invalid.
auto apply(settings& values, const std::vector<flag>& flags, flag_id id, std::string_view text,
           std::string_view argument) -> std::optional<int> {
    auto& options = values.translation;
    // `-include-std` wins over `-import-std` in either order.
    const auto import_std = [&] {
        if (options.standard_library != standard_library_mode::include_all) {
            options.standard_library = standard_library_mode::import_all;
        }
    };
    switch (id) {
    case flag_id::help: print_help(flags); values.help = true; break;
    case flag_id::version: print_version(); values.help = true; break;
    case flag_id::gen_build:
    case flag_id::internal_debug:
    case flag_id::debug:
        return error(std::string{text} + " is not supported");
    case flag_id::verbose: values.verbose = true; break;
    case flag_id::colon_errors: values.colon_errors = true; break;
    case flag_id::quiet: values.quiet = true; break;
    case flag_id::emit_info: options.emit_version_info = true; break;
    case flag_id::clean_cpp1: options.line_directives = false; break;
    case flag_id::line_paths: values.line_paths = true; break;
    case flag_id::import_std: import_std(); break;
    case flag_id::include_std: options.standard_library = standard_library_mode::include_all; break;
    case flag_id::pure_cpp2: options.pure_cpp2 = true; import_std(); break;
    case flag_id::no_null_checks: options.check_null_dereference = false; break;
    case flag_id::no_div_zero_checks: options.check_division_by_zero = false; break;
    case flag_id::no_subscript_checks: options.check_subscripts = false; break;
    case flag_id::no_comparison_checks: options.check_comparisons = false; break;
    case flag_id::no_exceptions: options.exceptions = false; break;
    case flag_id::no_rtti: options.rtti = false; break;
    case flag_id::output: values.output = argument; break;
    case flag_id::cwd: values.cwd = std::filesystem::path{std::string{argument}}; break;
    case flag_id::source_map: values.source_map = argument; break;
    case flag_id::jobs: {
        auto parsed_jobs = 0u;
        const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(),
                                            parsed_jobs);
        if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() ||
            parsed_jobs == 0) {
            return error(std::string{text} + " requires a positive integer");
        }
        values.jobs = parsed_jobs;
        break;
    }
    }
    return std::nullopt;
}

auto read_file(const std::filesystem::path& path, std::string& data) -> bool {
    auto input = std::ifstream(path, std::ios::binary);
    if (!input) return false;
    auto buffer = std::ostringstream{};
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) return false;
    data = buffer.str();
    return true;
}

auto write_in_place(const std::filesystem::path& path, const std::string& data) -> bool {
    auto output = std::ofstream(path, std::ios::binary | std::ios::trunc);
    output << data;
    output.close();
    return static_cast<bool>(output);
}

// Replaces a regular file through a rename of a uniquely named temporary in
// its directory, following symbolic links to it, and keeps an existing file's
// permissions. Writes the file in place when it is not a regular file, such as
// /dev/null, has other hard links, or the temporary cannot be created.
auto write_atomic(const std::filesystem::path& path, const std::string& data) -> bool {
    auto target = path;
    auto error = std::error_code{};
    for (auto depth = 0; depth < 40 && std::filesystem::is_symlink(target, error); ++depth) {
        const auto link = std::filesystem::read_symlink(target, error);
        if (error) break;
        target = link.is_absolute() ? link : target.parent_path() / link;
    }
    const auto status = std::filesystem::status(target, error);
    const auto existing = !error && std::filesystem::exists(status);
    if (existing && (!std::filesystem::is_regular_file(status) ||
                     std::filesystem::hard_link_count(target, error) > 1)) {
        return write_in_place(target, data);
    }
    auto random = std::random_device{};
    auto temporary = target;
    do {
        temporary = target;
        temporary += "." + std::to_string(random()) + std::to_string(random()) + ".tmp";
    } while (std::filesystem::exists(temporary, error));
    auto output = std::ofstream(temporary, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) return write_in_place(target, data);
    output << data;
    output.close();
    if (output) {
        if (existing) std::filesystem::permissions(temporary, status.permissions(), error);
        std::filesystem::rename(temporary, target, error);
        if (!error) return true;
    }
    std::filesystem::remove(temporary, error);
    return false;
}

auto same_file(const std::filesystem::path& left, const std::filesystem::path& right) -> bool {
    auto error = std::error_code{};
    const auto equivalent = std::filesystem::equivalent(left, right, error);
    if (!error) return equivalent;
    if (std::filesystem::exists(left, error) && !error &&
        std::filesystem::exists(right, error) && !error) {
        return false;
    }
    error.clear();
    const auto left_absolute = std::filesystem::absolute(left, error);
    if (error) return false;
    const auto right_absolute = std::filesystem::absolute(right, error);
    if (error) return false;
    return left_absolute.lexically_normal() == right_absolute.lexically_normal();
}

auto with_thousands(std::uint64_t value) -> std::string {
    auto result = std::to_string(value);
    for (auto position = result.size(); position > 3; position -= 3) {
        result.insert(position - 3, 1, ',');
    }
    return result;
}

auto header_guard(std::string_view output) -> std::string {
    auto result = std::string{output};
    for (auto& value : result) {
        value = std::isalnum(static_cast<unsigned char>(value))
            ? static_cast<char>(std::toupper(static_cast<unsigned char>(value))) : '_';
    }
    return result + "_CPP2";
}

// `{"version":1,"source":...,"output":...,"lines":[...]}`, where element `i`
// of `lines` is the source line of output line `i + 1`, or null.
auto source_map_json(const translation_result& result, std::string_view output) -> std::string {
    const auto lines = static_cast<std::size_t>(
        std::count(result.cpp1.begin(), result.cpp1.end(), '\n') +
        (!result.cpp1.empty() && !result.cpp1.ends_with('\n') ? 1 : 0));
    auto mapped = json::array(lines);
    for (const auto& [generated, original] : result.source_map) {
        if (generated.line >= 1 && generated.line <= lines) {
            mapped[generated.line - 1] = original.line;
        }
    }
    return json::serialize(json::object{{"version", 1},
        {"source", result.options.source_path}, {"output", output},
        {"lines", std::move(mapped)}}) + "\n";
}

struct input_file final {
    std::string name;
    std::string source;
    std::string output;
    std::string failure;
};

auto report_flag(const std::vector<flag>& flags, std::string_view argument) -> int {
    auto candidates = std::vector<std::string>{};
    for (const auto& value : flags) {
        if (value.name.starts_with(argument.substr(1))) candidates.push_back(value.display_name());
    }
    if (candidates.empty()) {
        std::cerr << argument << " - unknown compiler flag name (try " << argument.front()
                  << "help)\n";
        return 1;
    }
    std::cerr << argument << " - ambiguous compiler flag name, did you mean one of these?\n";
    for (const auto& candidate : candidates) {
        std::cerr << "  " << argument.front() << candidate << '\n';
    }
    return 1;
}

auto print_statistics(std::ostream& out, const source_text& source,
                      std::chrono::milliseconds elapsed) -> void {
    // As upstream, a file with Cpp2 counts only its preprocessor lines as Cpp1.
    auto cpp1_lines = std::uint64_t{0};
    auto cpp2_lines = std::uint64_t{0};
    for (const auto& line : source.lines()) {
        if (line.category != source_category::cpp2 &&
            (!source.has_cpp2() || line.category == source_category::preprocessor)) {
            ++cpp1_lines;
        }
        else {
            ++cpp2_lines;
        }
    }
    const auto total = cpp1_lines + cpp2_lines;
    const auto width = static_cast<int>(with_thousands(total).size());
    out << "   Cpp1  " << std::right << std::setw(width) << with_thousands(cpp1_lines) << " line"
        << (cpp1_lines != 1 ? "s" : "");
    out << "\n   Cpp2  " << std::right << std::setw(width) << with_thousands(cpp2_lines) << " line"
        << (cpp2_lines != 1 ? "s" : "");
    if (total > 0) {
        out << " (";
        if (cpp1_lines == 0) out << 100;
        else if (cpp2_lines / cpp1_lines > 25) {
            out << std::setprecision(3)
                << 100.0 * static_cast<double>(cpp2_lines) / static_cast<double>(total);
        }
        else {
            out << 100 * cpp2_lines / total;
        }
        out << "%)";
    }
    out << "\n   Time  " << with_thousands(static_cast<std::uint64_t>(elapsed.count()))
        << " ms";
}

}

int main(int argc, char** argv) {
    const auto flags = make_flags();
    auto values = settings{};
    auto arguments = std::vector<std::string>{};
    // The arguments after `--` are input files, even those that look like flags.
    auto files_after = std::size_t{0};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--") {
            for (++index; index < argc; ++index, ++files_after) arguments.emplace_back(argv[index]);
            break;
        }
        // `-experiment=name` is matched in full, so it shortens no upstream flag's prefix.
        if (argument.starts_with("-experiment=")) {
            const auto name = argument.substr(argument.find('=') + 1);
            if (name == "list") {
                print_experiments();
                values.help = true;
            }
            else if (!values.translation.experiments.enable(name)) {
                return error("unknown experiment '" + std::string{name} +
                             "' (try -experiment=list)");
            }
            continue;
        }
        // Spellings from earlier cppfrontier versions.
        const auto* matched = argument == "--help" ? match_flag(flags, "-help")
            : argument == "--out" ? match_flag(flags, "-output")
            : match_flag(flags, argument);
        if (matched == nullptr) {
            arguments.emplace_back(argument);
            continue;
        }
        auto value = std::string_view{};
        if (matched->takes_value()) {
            if (index + 1 >= argc) {
                if (matched->id == flag_id::jobs) {
                    return error(std::string{argument} + " requires a positive integer");
                }
                return error("missing argument to option " + std::string{argument} +
                             " (try -help)");
            }
            value = argv[++index];
        }
        if (const auto status = apply(values, flags, matched->id, argument, value)) return *status;
    }
    if (values.help) return 0;

    for (const auto& argument : arguments | std::views::take(arguments.size() - files_after)) {
        if (argument.starts_with('-') ||
            (argument.starts_with('/') && argument.find('.') == std::string::npos)) {
            return report_flag(flags, argument);
        }
    }
    if (arguments.empty()) return error("no input files (try -help)");
    if (!values.output.empty() && values.output != "stdout" && arguments.size() != 1) {
        return error("-output requires exactly one input file, or 'stdout'");
    }
    if (!values.source_map.empty() && arguments.size() != 1) {
        return error("-source-map requires exactly one input file");
    }
    if (!values.cwd.empty()) {
        auto failure = std::error_code{};
        std::filesystem::current_path(values.cwd, failure);
        if (failure) return error("cannot change directory to " + values.cwd.string());
    }

    // As upstream, `dir/x.cpp2` is written to `x.cpp` and `dir/x.h2` to `x.h`
    // in the current directory, and `stdin` to stdout. Two inputs cannot share
    // an output file.
    auto inputs = std::vector<input_file>{};
    for (const auto& argument : arguments) {
        auto& input = inputs.emplace_back();
        input.name = argument;
        if (argument == "stdin") {
            auto buffer = std::ostringstream{};
            buffer << std::cin.rdbuf();
            input.source = buffer.str();
            input.output = values.output.empty() ? "stdout" : values.output;
            continue;
        }
        if (!argument.ends_with(".cpp2") && !argument.ends_with(".h2")) {
            input.failure = "source filename must end with .cpp2 or .h2: " + argument;
            continue;
        }
        input.output = !values.output.empty() ? values.output
            : std::filesystem::path{argument.substr(0, argument.size() - 1)}.filename().string();
        const auto earlier = std::find_if(inputs.begin(), inputs.end() - 1,
            [&](const input_file& other) { return other.output == input.output; });
        if (input.output != "stdout" && earlier != inputs.end() - 1) {
            input.failure = "output path " + input.output + " is also the output of " +
                earlier->name;
        }
        else if (values.translation.pure_cpp2 && argument.ends_with(".h2")) {
            input.failure = "-pure-cpp2 is not supported for headers";
        }
        else if (!read_file(argument, input.source)) {
            input.failure = "file not found: " + argument;
        }
    }

    // The source map, of the one input, replaces neither it nor its output.
    if (auto& input = inputs.front(); !values.source_map.empty() && input.failure.empty()) {
        if (input.name != "stdin" && same_file(values.source_map, input.name)) {
            input.failure = "source map path " + values.source_map + " is the input file";
        }
        else if (input.output != "stdout" && same_file(values.source_map, input.output)) {
            input.failure = "source map path " + values.source_map + " is also the output path";
        }
    }

    auto requests = std::vector<file_request>{};
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto& input = inputs[index];
        if (!input.failure.empty()) continue;
        auto options = values.translation;
        options.source_path = values.line_paths && input.name != "stdin"
            ? std::filesystem::absolute(input.name).string() : input.name;
        if (input.output.ends_with('h')) options.header_guard = header_guard(input.output);
        requests.push_back({index, input.source, std::move(options)});
    }
    // Verbose output reports each file's own translation time.
    auto results = std::vector<file_result>{};
    auto elapsed = std::vector<std::chrono::milliseconds>(inputs.size());
    if (values.verbose) {
        for (const auto& request : requests) {
            const auto start = std::chrono::steady_clock::now();
            results.push_back(std::move(parallel_translator{1}.translate_files({request}).front()));
            elapsed[request.index] = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
        }
    }
    else {
        results = parallel_translator{values.jobs}.translate_files(requests);
    }

    // Status lines go to stdout, unless the output does.
    auto& out = values.output == "stdout" ? std::cerr : std::cout;
    auto status = 0;
    auto next_result = results.begin();
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        auto& input = inputs[index];
        if (!values.quiet && input.name != "stdin" && values.output != "stdout") {
            out << input.name << "...";
        }
        const auto* result = next_result != results.end() && next_result->index == index
            ? &*next_result++ : nullptr;
        if (result != nullptr && !result->completed) input.failure = "was not translated";
        auto issues = std::vector<std::string>{};
        // Printed with the errors, or after the status line when there are none.
        auto warnings = std::vector<std::string>{};
        if (!input.failure.empty()) issues.push_back(input.name + ": error: " + input.failure);
        if (result != nullptr && result->completed) {
            const auto& translation = result->translation;
            std::cout << translation.printed;
            for (const auto& issue : translation.diagnostics) {
                auto text = input.name;
                if (issue.span.begin.line > 0) {
                    text += values.colon_errors
                        ? ":" + std::to_string(issue.span.begin.line) + ":" +
                            std::to_string(issue.span.begin.column)
                        : "(" + std::to_string(issue.span.begin.line) + "," +
                            std::to_string(issue.span.begin.column) + ")";
                }
                text += ": " + std::string{severity_name(issue.severity)} + ": " + issue.message;
                if (translation.has_errors()) issues.push_back(std::move(text));
                else warnings.push_back(std::move(text));
            }
            if (!translation.has_errors()) {
                if (input.output == "stdout") {
                    std::cout << translation.cpp1;
                }
                else if (same_file(input.output, input.name)) {
                    issues.push_back(input.name + ": error: output path " + input.output +
                                     " is the input file");
                }
                else if (!write_atomic(input.output, translation.cpp1)) {
                    issues.push_back(input.name + ": error: could not open output file " +
                                     input.output);
                }
                if (issues.empty() && !values.source_map.empty() &&
                    !write_atomic(values.source_map, source_map_json(translation, input.output))) {
                    issues.push_back(input.name + ": error: could not open source map file " +
                                     values.source_map);
                }
            }
        }
        if (!issues.empty()) {
            std::cerr << '\n';
            for (const auto& issue : warnings) std::cerr << issue << '\n';
            for (const auto& issue : issues) std::cerr << issue << '\n';
            std::cerr << '\n';
            status = 1;
            continue;
        }
        if (values.quiet || values.output == "stdout") {
            for (const auto& issue : warnings) std::cerr << issue << '\n';
            continue;
        }

        const auto source = source_text::from_bytes(input.source);
        const auto has_cpp1 = std::any_of(source.lines().begin(), source.lines().end(),
            [](const source_line& line) {
                return line.category == source_category::cpp1 ||
                    line.category == source_category::preprocessor;
            });
        out << (!has_cpp1 ? " ok (all Cpp2, passes safety checks)\n"
                : source.has_cpp2() ? " ok (mixed Cpp1/Cpp2, Cpp2 code passes safety checks)\n"
                : " ok (all Cpp1)\n");
        if (values.verbose) print_statistics(out, source, elapsed[index]);
        out.flush();
        for (const auto& issue : warnings) std::cerr << issue << '\n';
        out << '\n';
    }
    return status;
}
