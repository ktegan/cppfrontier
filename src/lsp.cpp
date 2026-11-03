#include <cppfrontier/lsp.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <istream>
#include <limits>
#include <ostream>

namespace cppfrontier::lsp {

namespace {

constexpr auto max_message_size = std::size_t{1} << 28;

// The byte length and UTF-16 length of the character starting `text`; a byte
// that does not start a valid UTF-8 sequence is one of each.
auto next_character(std::string_view text) -> std::pair<std::size_t, std::uint32_t> {
    const auto lead = static_cast<unsigned char>(text[0]);
    const auto length = lead < 0x80 ? 1u : (lead >> 5) == 0x6 ? 2u : (lead >> 4) == 0xE ? 3u
        : (lead >> 3) == 0x1E ? 4u : 0u;
    if (length <= 1 || length > text.size()) return {1, 1};
    auto code_point = static_cast<std::uint32_t>(lead & (0x7F >> length));
    for (auto index = std::size_t{1}; index < length; ++index) {
        const auto next = static_cast<unsigned char>(text[index]);
        if ((next & 0xC0) != 0x80) return {1, 1};
        code_point = (code_point << 6) | (next & 0x3F);
    }
    constexpr std::uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    if (code_point < minimum[length] || code_point > 0x10FFFF ||
        (code_point >= 0xD800 && code_point < 0xE000)) {
        return {1, 1};
    }
    return {length, code_point >= 0x10000 ? 2u : 1u};
}

auto lower(std::string_view text) -> std::string {
    auto result = std::string{text};
    for (auto& value : result) value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    return result;
}

auto response(const json::value& id, json::value result) -> std::string {
    return json::serialize(json::object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

auto error_response(const json::value& id, int code, std::string_view message) -> std::string {
    return json::serialize(json::object{{"jsonrpc", "2.0"}, {"id", id},
        {"error", json::object{{"code", code}, {"message", message}}}});
}

auto notification(std::string_view method, json::value params) -> std::string {
    return json::serialize(json::object{{"jsonrpc", "2.0"}, {"method", method},
                                        {"params", std::move(params)}});
}

// LSP `SymbolKind` values.
auto symbol_kind(declaration_kind kind) -> int {
    switch (kind) {
    case declaration_kind::function: return 12;
    case declaration_kind::object: return 13;
    case declaration_kind::namespace_node: return 3;
    case declaration_kind::type: return 5;
    case declaration_kind::concept_declaration: return 11;
    case declaration_kind::using_declaration: return 2;
    }
    return 13;
}

auto split_lines(std::string_view text) -> std::vector<std::string> {
    auto lines = std::vector<std::string>{};
    for (;;) {
        const auto newline = text.find('\n');
        lines.emplace_back(text.substr(0, newline));
        if (newline == std::string_view::npos) break;
        text.remove_prefix(newline + 1);
    }
    return lines;
}

// Invalid request parameters.
struct invalid_params final {
    std::string message = "Invalid params";
};
struct unknown_method final {};

auto member(const json::value& value, std::string_view key) -> const json::value& {
    const auto* found = value.find(key);
    if (found == nullptr) throw invalid_params{};
    return *found;
}

auto string_member(const json::value& value, std::string_view key) -> const std::string& {
    const auto& found = member(value, key);
    if (!found.is_string()) throw invalid_params{};
    return found.as_string();
}

auto unsigned_member(const json::value& value, std::string_view key) -> std::uint64_t {
    const auto& found = member(value, key);
    if (!found.is_number() || found.as_number() < 0 || found.as_number() > 9.0e15 ||
        std::floor(found.as_number()) != found.as_number()) {
        throw invalid_params{};
    }
    return static_cast<std::uint64_t>(found.as_number());
}

}

auto read_message(std::istream& input) -> std::optional<std::string> {
    auto length = std::optional<std::size_t>{};
    auto header = std::string{};
    auto any_header = false;
    while (std::getline(input, header)) {
        if (header.ends_with('\r')) header.pop_back();
        if (header.empty()) {
            if (!any_header) continue;
            break;
        }
        any_header = true;
        const auto colon = header.find(':');
        if (colon == std::string::npos) return std::nullopt;
        if (lower(std::string_view{header}.substr(0, colon)) != "content-length") continue;
        auto value = std::string_view{header}.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        auto parsed = std::size_t{0};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
            parsed > max_message_size) {
            return std::nullopt;
        }
        length = parsed;
    }
    if (!length || !input) return std::nullopt;
    auto content = std::string(*length, '\0');
    input.read(content.data(), static_cast<std::streamsize>(*length));
    if (static_cast<std::size_t>(input.gcount()) != *length) return std::nullopt;
    return content;
}

auto write_message(std::ostream& output, std::string_view content) -> void {
    output << "Content-Length: " << content.size() << "\r\n\r\n" << content;
    output.flush();
}

auto utf16_offset(std::string_view line, std::uint32_t column) -> std::uint32_t {
    const auto end = std::min<std::size_t>(column == 0 ? 0 : column - 1, line.size());
    auto offset = std::uint32_t{0};
    for (auto position = std::size_t{0}; position < end;) {
        const auto [bytes, units] = next_character(line.substr(position));
        if (position + bytes > end) break;
        position += bytes;
        offset += units;
    }
    return offset;
}

auto byte_column(std::string_view line, std::uint32_t offset) -> std::uint32_t {
    auto position = std::size_t{0};
    for (auto units = std::uint32_t{0}; position < line.size();) {
        const auto [bytes, width] = next_character(line.substr(position));
        if (units + width > offset) break;
        units += width;
        position += bytes;
    }
    return static_cast<std::uint32_t>(position + 1);
}

auto session::to_lsp(const document& file, source_position position) const -> json::value {
    const auto line = position.line == 0 ? 0 : position.line - 1;
    const auto text = line < file.lines.size() ? std::string_view{file.lines[line]}
                                               : std::string_view{};
    const auto character = line < file.ascii.size() && file.ascii[line]
        ? std::min<std::uint32_t>(position.column == 0 ? 0 : position.column - 1,
                                  static_cast<std::uint32_t>(text.size()))
        : utf16_offset(text, position.column);
    return json::object{{"line", line}, {"character", character}};
}

auto session::to_lsp(const document& file, const source_span& span) const -> json::value {
    auto end = span.end;
    if (end.line < span.begin.line ||
        (end.line == span.begin.line && end.column < span.begin.column)) {
        end = span.begin;
    }
    return json::object{{"start", to_lsp(file, span.begin)}, {"end", to_lsp(file, end)}};
}

auto session::open(const std::string& uri, std::uint64_t version, std::string text,
                   std::vector<std::string>& notifications) -> void {
    auto& file = documents_[uri];
    file.version = version;
    file.lines = split_lines(text);
    file.ascii.clear();
    for (const auto& line : file.lines) {
        file.ascii.push_back(std::ranges::all_of(line,
            [](char value) { return static_cast<unsigned char>(value) < 0x80; }));
    }
    server_.open_document({uri, version, std::move(text)});
    auto diagnostics = json::array{};
    for (const auto& issue : server_.diagnostics(uri, version)) {
        const auto severity = issue.severity == diagnostic_severity::error ? 1
            : issue.severity == diagnostic_severity::warning ? 2 : 3;
        diagnostics.push_back(json::object{{"range", to_lsp(file, issue.span)},
            {"severity", severity}, {"code", issue.code}, {"source", "cppfrontier"},
            {"message", issue.message}});
    }
    notifications.push_back(notification("textDocument/publishDiagnostics",
        json::object{{"uri", uri}, {"version", version}, {"diagnostics", std::move(diagnostics)}}));
}

auto session::dispatch(const std::string& method, const json::value& params,
                       std::vector<std::string>& notifications) -> json::value {
    if (method == "initialize") {
        // `initializationOptions: {experiments: ["name", ...]}` enables experiments.
        auto options = translation_options{};
        const auto* settings = params.find("initializationOptions");
        if (const auto* names = settings != nullptr ? settings->find("experiments") : nullptr) {
            if (!names->is_array()) throw invalid_params{};
            for (const auto& name : names->as_array()) {
                if (!name.is_string()) throw invalid_params{};
                if (!options.experiments.enable(name.as_string())) {
                    throw invalid_params{"Invalid params: unknown experiment '" +
                                         name.as_string() + "'"};
                }
            }
        }
        server_ = language_server{std::move(options)};
        initialized_ = true;
        return json::object{
            {"capabilities", json::object{{"positionEncoding", "utf-16"},
                {"textDocumentSync", json::object{{"openClose", true}, {"change", 1}}},
                {"documentSymbolProvider", true}, {"definitionProvider", true}}},
            {"serverInfo", json::object{{"name", "cppfrontier"}, {"version", CPPFRONTIER_VERSION}}}};
    }
    if (method == "shutdown") {
        shutdown_ = true;
        return {};
    }
    if (method == "exit") {
        exit_code_ = shutdown_ ? 0 : 1;
        return {};
    }
    if (method == "initialized") return {};
    constexpr std::string_view document_methods[] = {"textDocument/didOpen",
        "textDocument/didChange", "textDocument/didClose", "textDocument/documentSymbol",
        "textDocument/definition"};
    if (std::ranges::find(document_methods, method) == std::end(document_methods)) {
        throw unknown_method{};
    }
    const auto& text_document = member(params, "textDocument");
    const auto& uri = string_member(text_document, "uri");
    if (method == "textDocument/didOpen") {
        open(uri, unsigned_member(text_document, "version"), string_member(text_document, "text"),
             notifications);
        return {};
    }
    if (method == "textDocument/didChange") {
        // With full synchronization, the last change holds the whole text; an
        // incremental change, which the server does not advertise, has a range.
        const auto& changes = member(params, "contentChanges");
        if (!changes.is_array() || changes.as_array().empty()) throw invalid_params{};
        for (const auto& change : changes.as_array()) {
            if (!change.is_object() || change.find("range") != nullptr) throw invalid_params{};
        }
        const auto version = unsigned_member(text_document, "version");
        const auto& text = string_member(changes.as_array().back(), "text");
        // A change to a document that is not open, or older than its text, is ignored.
        const auto found = documents_.find(uri);
        if (found == documents_.end() || version <= found->second.version) return {};
        open(uri, version, text, notifications);
        return {};
    }
    if (method == "textDocument/didClose") {
        server_.close_document(uri);
        documents_.erase(uri);
        notifications.push_back(notification("textDocument/publishDiagnostics",
            json::object{{"uri", uri}, {"diagnostics", json::array{}}}));
        return {};
    }
    const auto found = documents_.find(uri);
    if (method == "textDocument/documentSymbol") {
        auto symbols = json::array{};
        if (found == documents_.end()) return symbols;
        for (const auto& symbol : server_.symbols(uri).symbols) {
            symbols.push_back(json::object{{"name", symbol.name},
                {"kind", symbol_kind(symbol.kind)},
                {"location", json::object{{"uri", uri},
                    {"range", to_lsp(found->second, symbol.span)}}}});
        }
        return symbols;
    }
    if (method == "textDocument/definition") {
        const auto& position = member(params, "position");
        const auto line = unsigned_member(position, "line");
        const auto character = unsigned_member(position, "character");
        if (found == documents_.end() || line >= found->second.lines.size()) return {};
        const auto& file = found->second;
        // A character past the line's end is its end.
        const auto column = byte_column(file.lines[line], static_cast<std::uint32_t>(
            std::min<std::uint64_t>(character, std::numeric_limits<std::uint32_t>::max())));
        const auto definition = server_.goto_definition(uri, file.version,
            {static_cast<std::uint32_t>(line + 1), column});
        if (!definition.found) return {};
        return json::object{{"uri", uri}, {"range", to_lsp(file, definition.span)}};
    }
    return {};
}

auto session::handle(std::string_view message) -> std::vector<std::string> {
    const auto parsed = json::parse(message);
    if (!parsed) return {error_response({}, -32700, "Parse error")};
    // Batches are not supported.
    if (!parsed->is_object()) return {error_response({}, -32600, "Invalid request")};
    const auto* id = parsed->find("id");
    const auto* method = parsed->find("method");
    // Responses to server requests are ignored; the server sends none.
    if (method == nullptr && id != nullptr && parsed->find("jsonrpc") != nullptr &&
        (parsed->find("result") != nullptr || parsed->find("error") != nullptr)) {
        return {};
    }
    if (method == nullptr || !method->is_string() ||
        (id != nullptr && !id->is_number() && !id->is_string())) {
        return {error_response(id != nullptr ? *id : json::value{}, -32600, "Invalid request")};
    }
    const auto& name = method->as_string();
    const auto request = id != nullptr;
    if (!initialized_ && name != "initialize" && name != "exit") {
        if (!request) return {};
        return {error_response(*id, -32002, "Server not initialized")};
    }
    // `initialize` is sent once; another would discard the open documents.
    if (initialized_ && name == "initialize") {
        if (!request) return {};
        return {error_response(*id, -32600, "Invalid request: the server is already initialized")};
    }
    if (shutdown_ && name != "exit") {
        if (!request) return {};
        return {error_response(*id, -32600, "Invalid request: the server is shut down")};
    }
    const auto* params = parsed->find("params");
    const auto empty = json::value{};
    auto replies = std::vector<std::string>{};
    try {
        auto result = dispatch(name, params != nullptr ? *params : empty, replies);
        if (request) replies.push_back(response(*id, std::move(result)));
    }
    catch (const invalid_params& failure) {
        if (request) replies.push_back(error_response(*id, -32602, failure.message));
    }
    catch (const unknown_method&) {
        if (request) replies.push_back(error_response(*id, -32601, "Method not found: " + name));
    }
    return replies;
}

auto run(std::istream& input, std::ostream& output) -> int {
    auto server = session{};
    while (const auto message = read_message(input)) {
        for (const auto& reply : server.handle(*message)) write_message(output, reply);
        if (server.exited()) break;
    }
    return server.exit_code();
}

}
