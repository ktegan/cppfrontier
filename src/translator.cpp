#include <cppfrontier/pipeline.hpp>
#include <cppfrontier/translator.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <thread>

#include "autodiff_metafunction.hpp"
#include "cpp1_expression_renderer.hpp"
#include "generated_members.hpp"
#include "ir_walk.hpp"
#include "scope_model.hpp"

namespace cppfrontier {

translation_result translator::translate_source(const std::string& source_data) const {
    return translate_source(source_data, translation_options{});
}

namespace {

// The position `offset` bytes into `text`, which is written at `from`.
auto advance(source_position from, std::string_view text, std::size_t offset) -> source_position {
    for (std::size_t index = 0; index < offset; ++index) {
        if (text[index] == '\n') from = {from.line + 1, 1};
        else ++from.column;
    }
    return from;
}

// The word that ends `text` and its offset, ignoring trailing whitespace.
auto last_word(std::string_view text) -> std::pair<std::string_view, std::size_t> {
    const auto is_word = [](char value) {
        return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
    };
    auto end = text.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    auto begin = end;
    while (begin > 0 && is_word(text[begin - 1])) --begin;
    return {text.substr(begin, end - begin), begin};
}

// Where the access and `virtual` a metafunction gave a written `member` go in
// its source, with their text.
auto changed_specifiers(const source_text& source, const ir_declaration& member)
    -> std::vector<std::pair<source_position, std::string>> {
    auto result = std::vector<std::pair<source_position, std::string>>{};
    if (member.is_generated) return result;
    const auto written = source.text_between(member.span.begin, member.span.end);
    if (member.access != access_kind::default_access && !written.starts_with("public") &&
        !written.starts_with("protected") && !written.starts_with("private")) {
        result.emplace_back(member.span.begin,
                            member.access == access_kind::public_access      ? "public "
                                : member.access == access_kind::protected_access ? "protected "
                                                                                 : "private ");
    }
    if (member.kind != declaration_kind::function || member.parameters.empty() ||
        member.parameters.front().this_specifier == virtual_specifier::none) {
        return result;
    }
    const auto before = source.text_between(member.span.begin,
                                            member.parameters.front().name_span.begin);
    auto [word, offset] = last_word(before);
    if (word == "in" || word == "inout" || word == "out" || word == "move" || word == "copy" ||
        word == "forward") {
        word = last_word(std::string_view{before}.substr(0, offset)).first;
    }
    else {
        offset = before.size();
    }
    if (word != "virtual" && word != "override" && word != "final") {
        result.emplace_back(advance(member.span.begin, before, offset), "virtual ");
    }
    return result;
}

// The source of `declaration` with the members `@autodiff` generated for it,
// and the access and `virtual` that metafunctions gave its written members.
auto printed_source(const source_text& source, const ir_declaration& declaration) -> std::string {
    auto inserted = std::vector<std::pair<source_position, std::string>>{};
    for (const auto& member : declaration.generated_members) {
        if (member.before) inserted.emplace_back(*member.before, member.text + "\n");
    }
    const auto key = [](source_position position) {
        return std::pair{position.line, position.column};
    };
    for (const auto& member : declaration.members) {
        // A generated member is written where its type is.
        if (key(member.span.begin) <= key(declaration.span.begin)) continue;
        for (auto& specifier : changed_specifiers(source, member)) {
            inserted.push_back(std::move(specifier));
        }
    }
    std::stable_sort(inserted.begin(), inserted.end(), [&](const auto& left, const auto& right) {
        return key(left.first) < key(right.first);
    });
    auto result = std::string{};
    auto from = declaration.span.begin;
    for (const auto& [position, text] : inserted) {
        result += source.text_between(from, position) + text;
        from = position;
    }
    const auto rest = source.text_between(from, declaration.span.end);
    const auto close = rest.rfind('}');
    if (close == std::string::npos) return result + rest;
    result += rest.substr(0, close);
    for (const auto& member : declaration.generated_members) {
        if (!member.before) result += "\n" + member.text + "\n";
    }
    return result + rest.substr(close);
}

// As upstream's `-pure-cpp2`, the first line that is neither Cpp2, blank, a
// comment, an import, nor an `#include` of a `.h2` file. As upstream, the
// `#include` lines that begin a file with Cpp2 code are reported as Cpp1 syntax.
auto pure_cpp2_diagnostics(const source_text& source) -> std::vector<diagnostic> {
    auto leading = !source.cpp2_sections().empty();
    for (std::size_t index = 0; index < source.lines().size(); ++index) {
        const auto& line = source.lines()[index];
        const auto position = source_position{static_cast<std::uint32_t>(index + 1), 1};
        const auto h2_include = line.text.find("#include") != std::string::npos &&
            line.text.ends_with(".h2\"");
        if (line.category == source_category::preprocessor && leading &&
            line.text.find("#include") != std::string::npos && !h2_include) {
            return {{"pure-cpp2", diagnostic_severity::error, {position, position},
                     "pure-cpp2 switch disables Cpp1 syntax"}};
        }
        if (line.category != source_category::empty && line.category != source_category::comment) {
            leading = false;
        }
        if (line.category == source_category::preprocessor && !h2_include) {
            return {{"pure-cpp2", diagnostic_severity::error, {position, position},
                "pure-cpp2 switch disables the preprocessor, including #include (except of .h2 "
                "files) - use import instead (note: 'import std;' is implicit in -pure-cpp2)"}};
        }
        if (line.category == source_category::cpp1 &&
            line.text.find_first_not_of(" \t\r") != std::string::npos) {
            return {{"pure-cpp2", diagnostic_severity::error, {position, position},
                     "pure-cpp2 switch disables Cpp1 syntax"}};
        }
    }
    return {};
}

auto contains(const source_span& outer, const source_position& inner) -> bool {
    const auto before = [](const source_position& left, const source_position& right) {
        return left.line != right.line ? left.line < right.line : left.column < right.column;
    };
    return !before(inner, outer.begin) && !before(outer.end, inner);
}

// Keeps the first error reported for each statement, the tokens between two
// `;`, `{` or `}`, among the syntax diagnostics from `first` on: the later
// errors of a statement follow from its first.
auto keep_first_error_per_statement(const token_list& tokens,
                                    std::vector<diagnostic>& diagnostics, std::size_t first)
    -> void {
    const auto key = [](source_position at) { return std::pair{at.line, at.column}; };
    auto delimiters = std::vector<std::pair<std::uint32_t, std::uint32_t>>{};
    for (const auto& value : tokens) {
        if (value.text == ";" || value.text == "{" || value.text == "}") {
            delimiters.push_back(key(value.span.begin));
        }
    }
    std::sort(delimiters.begin(), delimiters.end());
    auto reported = std::vector<bool>(delimiters.size() + 1);
    diagnostics.erase(std::remove_if(
        diagnostics.begin() + static_cast<std::ptrdiff_t>(first), diagnostics.end(),
        [&](const diagnostic& value) {
            if (value.severity != diagnostic_severity::error) return false;
            const auto statement = static_cast<std::size_t>(std::lower_bound(
                delimiters.begin(), delimiters.end(), key(value.span.begin)) - delimiters.begin());
            if (reported[statement]) return true;
            reported[statement] = true;
            return false;
        }), diagnostics.end());
}

}

translation_result translator::translate_source(const std::string& source_data,
                                                translation_options options) const {
    auto result = translation_result{};
    result.options = std::move(options);
    const auto source = source_text::from_bytes(source_data, result.options.experiments);

    result.diagnostics = validate_preprocessor(source);
    auto braces = validate_cpp1_braces(source);
    result.diagnostics.insert(result.diagnostics.end(), braces.begin(), braces.end());
    const auto source_checks = result.diagnostics.size();
    auto all_tokens = token_list{};
    auto program = ir_program{};
    for (const auto& section : source.cpp2_sections()) {
        auto tokens = tokenize(source, section.begin_line, section.end_line_exclusive,
                               &result.diagnostics);
        for (const auto& value : tokens) {
            const auto interpolated = value.kind == token_kind::string_literal &&
                value.text.find(")$") != std::string::npos;
            if ((value.kind == token_kind::identifier || interpolated) &&
                (interpolated ? value.text.find(generated_name_prefix) != std::string::npos
                              : value.text.starts_with(generated_name_prefix))) {
                result.diagnostics.push_back({"reserved-name", diagnostic_severity::error,
                    value.span, "names beginning with `" + std::string{generated_name_prefix} +
                    "` are reserved for the translator"});
            }
        }
        auto extracted = extract_function_expressions(std::move(tokens),
                                                      program.function_expressions.size());
        auto section_tokens = std::move(extracted.tokens);
        result.diagnostics.insert(result.diagnostics.end(), extracted.diagnostics.begin(),
                                  extracted.diagnostics.end());
        auto validation_issues = validate_cpp2(section_tokens, result.options.experiments);
        result.diagnostics.insert(result.diagnostics.end(), validation_issues.begin(),
                                  validation_issues.end());
        all_tokens.insert(all_tokens.end(), section_tokens.begin(), section_tokens.end());
        auto parsed = parse_cpp2(source, section_tokens,
                                 parse_options{{}, std::move(extracted.replacements),
                                               result.options.experiments});
        // An `as name` binding is not a conversion.
        std::erase_if(result.diagnostics, [&](const diagnostic& value) {
            return value.code == "mixed-is-as" &&
                std::any_of(parsed.binding_operators.begin(), parsed.binding_operators.end(),
                            [&](const source_span& at) {
                                return at.begin.line == value.span.begin.line &&
                                    at.begin.column == value.span.begin.column;
                            });
        });
        result.diagnostics.insert(result.diagnostics.end(), parsed.diagnostics.begin(), parsed.diagnostics.end());
        auto built = build_ir(parsed.tree, std::move(program));
        program = std::move(built.program);
        program.function_expressions.insert(program.function_expressions.end(),
                                            extracted.expressions.begin(),
                                            extracted.expressions.end());
        result.diagnostics.insert(result.diagnostics.end(), built.diagnostics.begin(), built.diagnostics.end());
    }
    result.tokens = std::move(all_tokens);
    keep_first_error_per_statement(result.tokens, result.diagnostics, source_checks);
    // With `patterns`, a missing `is _` is checked after reflection.
    const auto parsed_without_errors = std::none_of(result.diagnostics.begin(),
        result.diagnostics.end(), [&](const diagnostic& value) {
            return value.severity == diagnostic_severity::error &&
                !(result.options.experiments.enabled("patterns") &&
                  value.code == "invalid-inspect" && value.message == inspect_wildcard_message);
        });
    const auto reserve = [&](const std::string& name, source_span span) {
        if (is_cpp2_support_name(name, result.options.experiments)) {
            result.diagnostics.push_back({"reserved-support-name", diagnostic_severity::error,
                span, "'" + name + "' is a Cpp2 support name (cpp2::" + name +
                ") and cannot be declared"});
        }
    };
    for (const auto& declared : program.symbols) reserve(declared.name, declared.span);
    // Template parameters have no symbol.
    const auto reserve_template_parameters = [&](const auto& self,
                                                 const ir_declaration& declaration) -> void {
        for (const auto& parameter : declaration.template_parameters) {
            reserve(parameter.name, declaration.span);
        }
        for (const auto& member : declaration.members) self(self, member);
        if (!declaration.function_statements) return;
        ir_detail::visit_statements(declaration.function_statements->statements,
                                    [&](const statement& value) {
            const auto* form = std::get_if<statement::declaration_form>(&value.form);
            if (form == nullptr || !form->declaration) return;
            for (const auto& parameter : form->declaration->template_parameters) {
                reserve(parameter.name, value.span);
            }
        });
    };
    for (const auto& declaration : program.declarations) {
        reserve_template_parameters(reserve_template_parameters, declaration);
    }
    auto recovered = std::vector<source_span>{};
    for (const auto& declaration : program.declarations) {
        if (std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [&](const diagnostic& value) {
                            return value.severity == diagnostic_severity::error &&
                                contains(declaration.span, value.span.begin);
                        })) {
            recovered.push_back(declaration.span);
        }
    }
    auto reflected = apply_reflection(std::move(program), result.options.experiments);
    ir_detail::visit_declarations(reflected.program.declarations,
                                  [&](const ir_declaration& declaration) {
        const auto& metaclasses = declaration.metaclasses;
        if (parsed_without_errors &&
            std::find(metaclasses.begin(), metaclasses.end(), "print") != metaclasses.end()) {
            const auto text = printed_source(source, declaration);
            const auto printed = pretty_print_declaration(text);
            result.printed += printed ? *printed + "\n" : "\n" + text + "\n";
        }
    });
    // Metafunctions and the flow analyses see the declarations that recovery
    // from a syntax error left, so they report nothing on a declaration with one.
    const auto in_recovered = [&](const diagnostic& value) {
        return std::any_of(recovered.begin(), recovered.end(), [&](const source_span& span) {
            return contains(span, value.span.begin);
        });
    };
    std::erase_if(reflected.diagnostics, in_recovered);
    result.diagnostics.insert(result.diagnostics.end(), reflected.diagnostics.begin(),
                              reflected.diagnostics.end());
    result.program = std::move(reflected.program);
    const auto scopes = scope_model{result.program};
    result.program.references = resolve_symbols(result.program, scopes, result.tokens);
    auto constraint_issues = check_constraints(result.program, scopes);
    result.diagnostics.insert(result.diagnostics.end(), constraint_issues.begin(),
                              constraint_issues.end());
    auto safety = analyze_safety(result.program, scopes, result.options.check_comparisons,
                                 result.options.experiments);
    std::erase_if(safety.diagnostics, in_recovered);
    result.diagnostics.insert(result.diagnostics.end(), safety.diagnostics.begin(),
                              safety.diagnostics.end());
    auto pointer_issues = check_pointer_safety(result.program, result.tokens);
    result.diagnostics.insert(result.diagnostics.end(), pointer_issues.begin(),
                              pointer_issues.end());
    if (result.options.experiments.enabled("patterns")) {
        check_inspect_patterns(result.program, scopes, result.tokens, result.options.experiments,
                               result.diagnostics);
    }
    if (result.options.experiments.enabled("try-operator")) {
        auto try_issues = check_try_operator(result.program, result.tokens);
        result.diagnostics.insert(result.diagnostics.end(), try_issues.begin(), try_issues.end());
    }
    if (result.options.experiments.enabled("exclusivity")) {
        auto exclusivity_issues = check_exclusivity(result.program, scopes, result.tokens);
        result.diagnostics.insert(result.diagnostics.end(), exclusivity_issues.begin(),
                                  exclusivity_issues.end());
    }
    // Text the lexer could not read leaves the tokens around it incomplete, so
    // after a lexical error only the lexical errors are reported.
    const auto lexical = [](const diagnostic& value) { return value.code == "lexer"; };
    if (std::any_of(result.diagnostics.begin(), result.diagnostics.end(), lexical)) {
        result.diagnostics.erase(
            std::remove_if(result.diagnostics.begin() + static_cast<std::ptrdiff_t>(source_checks),
                           result.diagnostics.end(),
                           [&](const diagnostic& value) { return !lexical(value); }),
            result.diagnostics.end());
    }
    locate_generated_diagnostics(result.program, result.diagnostics);
    if (result.options.pure_cpp2 && !result.has_errors()) {
        const auto pure = pure_cpp2_diagnostics(source);
        result.diagnostics.insert(result.diagnostics.end(), pure.begin(), pure.end());
    }
    std::sort(result.diagnostics.begin(), result.diagnostics.end(), compare_by_source);

    if (!result.has_errors()) {
        auto emitted = emit_cpp1(result.program, scopes, source, result.tokens, result.options,
                                 safety.plan);
        result.cpp1 = std::move(emitted.cpp1);
        result.source_map = std::move(emitted.source_map);
    }
    return result;
}

namespace {

auto internal_error(std::string_view what) -> translation_result {
    auto result = translation_result{};
    result.diagnostics.push_back({"internal-error", diagnostic_severity::error, {},
                                  "internal error: " + std::string{what}});
    return result;
}

}

struct parallel_translator::batch final {
    std::atomic<bool> cancelled{false};
};

parallel_translator::parallel_translator(unsigned worker_count) noexcept
    : worker_count_{worker_count ? worker_count : std::max(1u, std::thread::hardware_concurrency())} {}

void parallel_translator::request_cancel() {
    const auto lock = std::lock_guard{mutex_};
    if (running_.empty()) pending_cancel_ = true;
    for (auto* running : running_) running->cancelled.store(true, std::memory_order_relaxed);
    cancel_effective_ = false;
}

std::vector<file_result> parallel_translator::translate_files(const std::vector<file_request>& requests) {
    auto results = std::vector<file_result>(requests.size());
    for (std::size_t index = 0; index < requests.size(); ++index) {
        results[index].index = requests[index].index;
    }
    auto state = batch{};
    {
        const auto lock = std::lock_guard{mutex_};
        state.cancelled.store(std::exchange(pending_cancel_, false), std::memory_order_relaxed);
        running_.push_back(&state);
    }
    const auto local_translator = translator{};
    auto next = std::atomic<std::size_t>{0};
    auto futures = std::vector<std::future<void>>{};
    const auto task_count = std::min<std::size_t>(worker_count_, requests.size());
    futures.reserve(task_count);

    const auto translate = [&](std::size_t index) {
        try {
            return local_translator.translate_source(requests[index].source,
                                                     requests[index].options);
        }
        catch (const std::exception& error) {
            return internal_error(error.what());
        }
        catch (...) {
            return internal_error("unknown exception");
        }
    };
    for (std::size_t worker = 0; worker < task_count; ++worker) {
        futures.emplace_back(std::async(std::launch::async, [&] {
            while (!state.cancelled.load(std::memory_order_relaxed)) {
                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= requests.size()) break;
                results[index].translation = translate(index);
                results[index].completed = true;
            }
        }));
    }
    for (auto& future : futures) future.get();

    const auto lock = std::lock_guard{mutex_};
    std::erase(running_, &state);
    if (state.cancelled.load(std::memory_order_relaxed)) {
        const auto stopped = std::any_of(results.begin(), results.end(),
            [](const file_result& result) { return !result.completed; });
        cancel_effective_ = cancel_effective_ || stopped;
        if (running_.empty()) {
            pending_cancel_ = !cancel_effective_;
            cancel_effective_ = false;
        }
    }
    return results;
}

}
