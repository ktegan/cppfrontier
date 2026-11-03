#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <cppfrontier/ir.hpp>
#include <cppfrontier/options.hpp>

namespace cppfrontier {

struct document_symbol final {
    std::string name;
    symbol_id id = invalid_symbol;
    declaration_kind kind = declaration_kind::object;
    source_span span;
};

struct document_symbols final {
    std::uint64_t version = 0;
    std::vector<document_symbol> symbols;
};

struct definition_response final {
    bool found = false;
    source_span span;
};

struct document_request final {
    std::string uri;
    std::uint64_t version = 0;
    std::string source;
};

class language_server final {
public:
    language_server() = default;
    // Documents are translated with `options`, such as its experiments.
    explicit language_server(translation_options options) : options_{std::move(options)} {}

    auto open_document(const document_request& request) -> document_symbols;
    auto close_document(const std::string& uri) -> void;
    [[nodiscard]] auto symbols(const std::string& uri) const -> const document_symbols&;
    [[nodiscard]] auto diagnostics(const std::string& uri, std::uint64_t version) const
        -> std::vector<diagnostic>;
    [[nodiscard]] auto goto_definition(const std::string& uri, std::uint64_t version,
                                       source_position position) const -> definition_response;

private:
    struct document final {
        std::uint64_t version = 0;
        ir_program program;
        document_symbols symbols;
        std::vector<diagnostic> diagnostics;
    };
    translation_options options_;
    std::map<std::string, document> documents_;
};
}
