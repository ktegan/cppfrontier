#include <cppfrontier/language_server.hpp>
#include <cppfrontier/translator.hpp>

#include <exception>
#include <string>
#include <string_view>

namespace cppfrontier {

namespace {

auto internal_error(std::string_view what) -> translation_result {
    auto result = translation_result{};
    result.diagnostics.push_back({"internal-error", diagnostic_severity::error, {},
                                  "internal error: " + std::string{what}});
    return result;
}

// The translation of `source`, or an `internal-error` diagnostic for an exception.
auto translate(const std::string& source, const translation_options& options)
    -> translation_result {
    try {
        return translator{}.translate_source(source, options);
    }
    catch (const std::exception& error) {
        return internal_error(error.what());
    }
    catch (...) {
        return internal_error("unknown exception");
    }
}

// Where the source declares `value`: for a generated symbol, the source
// declaration of the same name in the same scope that it replaces, such as an
// `@enum` enumerator or a `@union` alternative, else the nearest enclosing
// source declaration, such as the type a metafunction generated it in.
auto source_declaration(const ir_program& program, const symbol& value) -> const symbol& {
    if (!value.generated) return value;
    for (const auto& candidate : program.symbols) {
        if (!candidate.generated && candidate.parent == value.parent &&
            candidate.name == value.name) {
            return candidate;
        }
    }
    for (const auto* scope = find_symbol(program, value.parent); scope != nullptr;
         scope = find_symbol(program, scope->parent)) {
        if (!scope->generated) return *scope;
    }
    return value;
}

}

auto language_server::open_document(const document_request& request) -> document_symbols {
    auto translated = translate(request.source, options_);
    auto& document = documents_[request.uri];
    document.version = request.version;
    document.program = std::move(translated.program);
    document.diagnostics = std::move(translated.diagnostics);
    document.symbols = document_symbols{.version = request.version, .symbols = {}};
    // The names the source declares: not block scopes, function expressions,
    // or what a metafunction or the translator generates.
    for (const auto& value : document.program.symbols) {
        if (value.id != invalid_symbol && !value.generated && !value.name.empty() &&
            !value.name.starts_with(generated_name_prefix)) {
            document.symbols.symbols.push_back({value.name, value.id, value.kind, value.span});
        }
    }
    return document.symbols;
}

auto language_server::close_document(const std::string& uri) -> void {
    documents_.erase(uri);
}

auto language_server::symbols(const std::string& uri) const -> const document_symbols& {
    static const auto empty = document_symbols{};
    const auto found = documents_.find(uri);
    return found != documents_.end() ? found->second.symbols : empty;
}

auto language_server::diagnostics(const std::string& uri, std::uint64_t version) const
    -> std::vector<diagnostic> {
    const auto found = documents_.find(uri);
    if (found == documents_.end() || found->second.version != version) return {};
    return found->second.diagnostics;
}

auto language_server::goto_definition(const std::string& uri, std::uint64_t version,
                                      source_position position) const -> definition_response {
    const auto found = documents_.find(uri);
    if (found == documents_.end() || found->second.version != version) return {};
    const auto& program = found->second.program;
    for (const auto& reference : program.references) {
        if (position.line < reference.span.begin.line || position.line > reference.span.end.line) continue;
        if (position.line == reference.span.begin.line && position.column < reference.span.begin.column) continue;
        if (position.line == reference.span.end.line && position.column > reference.span.end.column) continue;
        if (const auto* symbol = find_symbol(program, reference.symbol)) {
            return {true, source_declaration(program, *symbol).span};
        }
    }
    return {};
}

}
