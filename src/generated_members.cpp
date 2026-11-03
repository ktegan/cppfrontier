#include "generated_members.hpp"

#include <cppfrontier/pipeline.hpp>

#include <algorithm>
#include <memory>
#include <optional>

#include "ir_walk.hpp"

namespace cppfrontier {

namespace {

// Maps a position in generated text to the line of the declaration it was
// generated from, keeping distinct positions distinct.
auto relocate(source_position& position, source_span origin) -> void {
    position.column += generated_column * position.line;
    position.line = origin.begin.line;
}

auto relocate(source_span& span, source_span origin) -> void {
    relocate(span.begin, origin);
    relocate(span.end, origin);
}

auto relocate(statement_list& statements, source_span origin) -> void {
    ir_detail::visit_statements(statements, [&](statement& value) {
        relocate(value.span, origin);
        if (auto* block = std::get_if<statement::block_form>(&value.form)) {
            for (auto& parameter : block->parameters) relocate(parameter.value.name_span, origin);
            for (auto& handler : block->handlers) relocate(handler.caught.name_span, origin);
        }
        if (auto* loop = std::get_if<statement::loop_form>(&value.form);
            loop && loop->range_parameter) {
            relocate(loop->range_parameter->name_span, origin);
        }
    });
}

auto relocate(ir_declaration& declaration, source_span origin) -> void {
    relocate(declaration.span, origin);
    for (auto& parameter : declaration.parameters) relocate(parameter.name_span, origin);
    for (auto& named : declaration.named_returns) relocate(named.value.name_span, origin);
    if (declaration.function_statements) {
        auto body = std::make_shared<function_body>();
        body->span = declaration.function_statements->span;
        // The statements were just built and are not shared yet.
        body->statements = std::move(
            const_cast<function_body&>(*declaration.function_statements).statements);
        relocate(body->statements, origin);
        declaration.function_statements = std::move(body);
    }
    for (auto& member : declaration.members) relocate(member, origin);
}

// Whether `text` is one expression, optionally followed by `;`.
auto is_one_expression(const parsed_expression& text) -> bool {
    const auto& tree = text.tree();
    const auto tokens = tree.tokens.size() -
        (!tree.tokens.empty() && tree.tokens.back().text == ";" ? 1 : 0);
    if (tree.root.kind != expression_kind::sequence) return tree.root.last == tokens;
    return tree.root.children.size() == 1 && tree.root.children.front().first == 0 &&
        tree.root.children.front().last == tokens;
}

// The first expression in `statements` that does not parse as one expression,
// as the parser accepts some invalid expressions.
auto invalid_expression(const statement_list& statements) -> std::optional<std::string> {
    auto result = std::optional<std::string>{};
    ir_detail::visit_statements(statements, [&](const statement& value) {
        const auto check = [&](const parsed_expression& text) {
            if (!result && !text.empty() && !is_one_expression(text)) result = text.source();
        };
        if (const auto* form = std::get_if<statement::expression_form>(&value.form)) {
            check(form->text);
        }
        else if (const auto* form = std::get_if<statement::declaration_form>(&value.form)) {
            if (form->declaration) check(form->text);
        }
        else if (const auto* form = std::get_if<statement::destructuring_form>(&value.form)) {
            check(form->initializer);
        }
        else if (const auto* form = std::get_if<statement::if_form>(&value.form)) {
            check(form->condition);
        }
        else if (const auto* form = std::get_if<statement::loop_form>(&value.form)) {
            check(form->condition);
        }
        else if (const auto* form = std::get_if<statement::block_form>(&value.form)) {
            for (const auto& parameter : form->parameters) {
                check(parameter.initializer);
            }
        }
    });
    return result;
}

}

auto parse_generated_members(std::string_view text, const ir_declaration& target,
                             ir_program& program, source_span origin) -> generated_members {
    const auto wrapper = std::string{"cppfrontier_generated: "} +
        (target.kind == declaration_kind::type ? "type" : "namespace") + " = {\n" +
        std::string{text} + "\n}\n";
    const auto source = source_text::from_bytes(wrapper);
    auto scratch = ir_program{};
    scratch.symbols = program.symbols;
    const auto first_symbol = scratch.symbols.size();
    const auto first_error = [](const std::vector<diagnostic>& diagnostics) {
        const auto found = std::find_if(diagnostics.begin(), diagnostics.end(),
            [](const diagnostic& issue) { return issue.severity == diagnostic_severity::error; });
        return found == diagnostics.end() ? std::string{} : found->message;
    };
    for (const auto& section : source.cpp2_sections()) {
        const auto tokens = tokenize(source, section.begin_line, section.end_line_exclusive);
        auto parsed = parse_cpp2(source, tokens);
        if (auto error = first_error(parsed.diagnostics); !error.empty()) return {{}, error};
        auto built = build_ir(parsed.tree, std::move(scratch));
        if (auto error = first_error(built.diagnostics); !error.empty()) return {{}, error};
        scratch = std::move(built.program);
    }
    if (scratch.declarations.size() != 1) return {{}, "expected one declaration"};
    auto& wrapped = scratch.declarations.front();
    for (const auto& member : wrapped.members) {
        if (!member.function_statements) continue;
        if (const auto expression = invalid_expression(member.function_statements->statements)) {
            return {{}, "'" + *expression + "' is not an expression"};
        }
    }
    for (auto index = first_symbol; index < scratch.symbols.size(); ++index) {
        auto& symbol = scratch.symbols[index];
        if (symbol.id == wrapped.symbol) symbol.name.clear();
        if (symbol.parent == wrapped.symbol) symbol.parent = target.symbol;
        relocate(symbol.span, origin);
        symbol.generated = true;
    }
    auto members = std::move(wrapped.members);
    for (auto& member : members) {
        member.is_generated = true;
        relocate(member, origin);
        // The span begins at the origin, so it contains the relocated positions.
        member.span.begin = origin.begin;
    }
    program.symbols = std::move(scratch.symbols);
    return {std::move(members), {}};
}

auto locate_generated_diagnostics(const ir_program& program,
                                  std::vector<diagnostic>& diagnostics) -> void {
    const auto key = [](source_position position) {
        return std::pair{position.line, position.column};
    };
    for (auto& value : diagnostics) {
        if (value.span.begin.column < generated_column) continue;
        const ir_declaration* origin = nullptr;
        ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& member) {
            if (origin == nullptr && member.is_generated &&
                member.span.begin.column < generated_column &&
                member.span.end.column >= generated_column &&
                key(member.span.begin) <= key(value.span.begin) &&
                key(value.span.begin) <= key(member.span.end)) {
                origin = &member;
            }
        });
        if (origin == nullptr) continue;
        value.span = {origin->span.begin, origin->span.begin};
        value.message += " (in '" + origin->name + "', generated by a metafunction)";
    }
}

}
