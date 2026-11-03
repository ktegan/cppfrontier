#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cppfrontier/json.hpp>
#include <cppfrontier/language_server.hpp>

// A Language Server Protocol adapter over `language_server`, speaking JSON-RPC
// 2.0 with full-document synchronization.
namespace cppfrontier::lsp {

// The next `Content-Length` framed message, or nothing at the end of `input`
// or on a malformed header.
[[nodiscard]] auto read_message(std::istream& input) -> std::optional<std::string>;
auto write_message(std::ostream& output, std::string_view content) -> void;

// The 0-based UTF-16 offset of 1-based byte column `column` in `line`. Bytes
// that are not valid UTF-8 count as one code unit each.
[[nodiscard]] auto utf16_offset(std::string_view line, std::uint32_t column) -> std::uint32_t;
// The 1-based byte column of 0-based UTF-16 offset `offset` in `line`. An
// offset inside a surrogate pair is the start of its character, and one past
// the end of the line is the end of the line.
[[nodiscard]] auto byte_column(std::string_view line, std::uint32_t offset) -> std::uint32_t;

class session final {
public:
    // The messages to send in reply to `message`: a response to a request,
    // and any notifications.
    auto handle(std::string_view message) -> std::vector<std::string>;
    [[nodiscard]] auto exited() const noexcept -> bool { return exit_code_.has_value(); }
    // 0 after `shutdown` then `exit`, else 1.
    [[nodiscard]] auto exit_code() const noexcept -> int { return exit_code_.value_or(1); }

private:
    struct document final {
        std::uint64_t version = 0;
        std::vector<std::string> lines;
        // Whether each line is ASCII, so that its byte and UTF-16 columns agree.
        std::vector<bool> ascii;
    };

    auto dispatch(const std::string& method, const json::value& params,
                  std::vector<std::string>& notifications) -> json::value;
    auto open(const std::string& uri, std::uint64_t version, std::string text,
              std::vector<std::string>& notifications) -> void;
    [[nodiscard]] auto to_lsp(const document& file, source_position position) const -> json::value;
    [[nodiscard]] auto to_lsp(const document& file, const source_span& span) const -> json::value;

    language_server server_;
    std::map<std::string, document> documents_;
    bool initialized_ = false;
    bool shutdown_ = false;
    std::optional<int> exit_code_;
};

// Serves `input` until `exit` or the end of input; returns the exit code.
auto run(std::istream& input, std::ostream& output) -> int;

}
