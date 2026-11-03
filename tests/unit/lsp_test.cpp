#include <catch2/catch_all.hpp>

#include <cppfrontier/json.hpp>
#include <cppfrontier/lsp.hpp>
#include "test_support.hpp"

#include <chrono>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace json = cppfrontier::json;
namespace lsp = cppfrontier::lsp;

namespace {

auto frame(std::string_view content) -> std::string {
    auto output = std::ostringstream{};
    lsp::write_message(output, content);
    return output.str();
}

auto parse(const std::string& text) -> json::value {
    auto parsed = json::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

auto request(int id, std::string_view method, json::value params) -> std::string {
    return json::serialize(json::object{{"jsonrpc", "2.0"}, {"id", id}, {"method", method},
                                        {"params", std::move(params)}});
}

auto notify(std::string_view method, json::value params) -> std::string {
    return json::serialize(json::object{{"jsonrpc", "2.0"}, {"method", method},
                                        {"params", std::move(params)}});
}

auto position(int line, int character) -> json::value {
    return json::object{{"line", line}, {"character", character}};
}

auto range_of(const json::value& range) -> std::vector<double> {
    const auto& start = *range.find("start");
    const auto& end = *range.find("end");
    return {start.find("line")->as_number(), start.find("character")->as_number(),
            end.find("line")->as_number(), end.find("character")->as_number()};
}

}

TEST_CASE("JSON values round-trip", "[lsp][json]") {
    const auto text = std::string{R"({"a":[1,-2.5,1e3,true,false,null],"b":"q\"\\\n\u00e9\ud83d\ude00\u0001"})"};
    const auto value = parse(text);
    CHECK(value.find("a")->as_array().size() == 6);
    CHECK(value.find("a")->as_array()[2].as_number() == 1000);
    CHECK(value.find("b")->as_string() == "q\"\\\n\xC3\xA9\xF0\x9F\x98\x80\x01");
    CHECK(json::serialize(value) ==
          "{\"a\":[1,-2.5,1e3,true,false,null],\"b\":\"q\\\"\\\\\\n\xC3\xA9\xF0\x9F\x98\x80\\u0001\"}");
    CHECK(parse(json::serialize(value)) == value);
    // An unpaired surrogate and invalid UTF-8 become U+FFFD.
    CHECK(parse(R"("\ud800x")").as_string() == "\xEF\xBF\xBDx");
    CHECK(json::serialize(json::value{std::string{"\xFF"}}) == "\"\xEF\xBF\xBD\"");

    for (const auto* invalid : {"", "{", "[1,]", "01", "1.", "\"\\x\"", "{\"a\" 1}", "[1] 2",
                                "nul", "\"\n\""}) {
        CAPTURE(invalid);
        CHECK_FALSE(json::parse(invalid).has_value());
    }
    CHECK_FALSE(json::parse(std::string(300, '[') + std::string(300, ']')).has_value());
}

TEST_CASE("JSON numbers keep their text and objects parse in linear time", "[lsp][json][review]") {
    const auto big = parse(R"([9007199254740993,1e400,-1e400,1e-400,0.10])");
    CHECK(json::serialize(big) == "[9007199254740993,1e400,-1e400,1e-400,0.10]");
    CHECK(big.as_array()[1].as_number() == HUGE_VAL);
    CHECK(big.as_array()[2].as_number() == -HUGE_VAL);
    CHECK(big.as_array()[3].as_number() == 0);
    CHECK(parse(R"({"a":1,"b":2,"a":3})") == json::value{json::object{{"a", 3}, {"b", 2}}});

    auto text = std::string{"{"};
    for (auto index = 0; index < 100000; ++index) {
        text += (index == 0 ? "\"k" : ",\"k") + std::to_string(index) + "\":0";
    }
    text += "}";
    const auto start = std::chrono::steady_clock::now();
    CHECK(parse(text).as_object().size() == 100000);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{5});
}

TEST_CASE("LSP session echoes a request id unchanged", "[lsp][review]") {
    auto server = lsp::session{};
    for (const auto* id : {"9007199254740993", "1e400", "\"x\""}) {
        CAPTURE(id);
        const auto replies = server.handle(std::string{R"({"jsonrpc":"2.0","id":)"} + id +
                                           R"(,"method":"textDocument/hover"})");
        REQUIRE(replies.size() == 1);
        CHECK(replies[0].find(std::string{"\"id\":"} + id + ",") != std::string::npos);
    }
}

TEST_CASE("LSP messages use Content-Length framing", "[lsp]") {
    const auto content = std::string{"{\"jsonrpc\":\"2.0\",\"method\":\"x\",\"params\":\"\xC3\xA9\"}"};
    CHECK(frame(content) == "Content-Length: " + std::to_string(content.size()) + "\r\n\r\n" + content);

    auto input = std::istringstream{frame(content) +
        "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\ncontent-length: 2\r\n\r\n{}" +
        frame("[]")};
    CHECK(lsp::read_message(input) == content);
    CHECK(lsp::read_message(input) == "{}");
    CHECK(lsp::read_message(input) == "[]");
    CHECK_FALSE(lsp::read_message(input).has_value());

    for (const auto* invalid : {"Content-Length: 10\r\n\r\n{}", "Content-Length: x\r\n\r\n",
                                "Content-Type: text\r\n\r\n{}", "garbage\r\n\r\n"}) {
        CAPTURE(invalid);
        auto stream = std::istringstream{invalid};
        CHECK_FALSE(lsp::read_message(stream).has_value());
    }
}

TEST_CASE("LSP positions convert between bytes and UTF-16", "[lsp]") {
    // "aé😀b": a is 1 byte, é is 2, 😀 is 4 bytes and a surrogate pair.
    const auto line = std::string_view{"a\xC3\xA9\xF0\x9F\x98\x80" "b"};
    CHECK(lsp::utf16_offset(line, 1) == 0);
    CHECK(lsp::utf16_offset(line, 2) == 1);
    CHECK(lsp::utf16_offset(line, 4) == 2);
    CHECK(lsp::utf16_offset(line, 8) == 4);
    CHECK(lsp::utf16_offset(line, 9) == 5);
    CHECK(lsp::utf16_offset(line, 100) == 5);
    // A column inside a character is at its start.
    CHECK(lsp::utf16_offset(line, 3) == 1);
    CHECK(lsp::utf16_offset(line, 6) == 2);

    CHECK(lsp::byte_column(line, 0) == 1);
    CHECK(lsp::byte_column(line, 1) == 2);
    CHECK(lsp::byte_column(line, 2) == 4);
    CHECK(lsp::byte_column(line, 3) == 4);
    CHECK(lsp::byte_column(line, 4) == 8);
    CHECK(lsp::byte_column(line, 5) == 9);
    CHECK(lsp::byte_column(line, 50) == 9);

    // Invalid UTF-8 counts one code unit per byte.
    const auto invalid = std::string_view{"\xFF\xC3x"};
    CHECK(lsp::utf16_offset(invalid, 4) == 3);
    CHECK(lsp::byte_column(invalid, 2) == 3);
}

TEST_CASE("LSP session serves a scripted editing session", "[lsp]") {
    auto server = lsp::session{};
    const auto uri = std::string{"file:///work/example.cpp2"};

    auto replies = server.handle(request(1, "textDocument/documentSymbol",
        json::object{{"textDocument", json::object{{"uri", uri}}}}));
    REQUIRE(replies.size() == 1);
    CHECK(parse(replies[0]).find("error")->find("code")->as_number() == -32002);

    replies = server.handle(request(2, "initialize", json::object{{"capabilities", json::object{}}}));
    REQUIRE(replies.size() == 1);
    const auto capabilities = *parse(replies[0]).find("result")->find("capabilities");
    CHECK(capabilities.find("textDocumentSync")->find("change")->as_number() == 1);
    CHECK(capabilities.find("definitionProvider")->as_bool());
    CHECK(server.handle(notify("initialized", json::object{})).empty());

    // "é" before the declarations moves UTF-16 columns away from byte columns.
    const auto text = std::string{"/* \xC3\xA9 */ x: int = 1;\nf: () -> int = x;\n"};
    replies = server.handle(notify("textDocument/didOpen", json::object{{"textDocument",
        json::object{{"uri", uri}, {"languageId", "cpp2"}, {"version", 1}, {"text", text}}}}));
    REQUIRE(replies.size() == 1);
    auto published = parse(replies[0]);
    CHECK(published.find("method")->as_string() == "textDocument/publishDiagnostics");
    CHECK(published.find("params")->find("diagnostics")->as_array().empty());

    replies = server.handle(request(3, "textDocument/documentSymbol",
        json::object{{"textDocument", json::object{{"uri", uri}}}}));
    REQUIRE(replies.size() == 1);
    const auto symbols = parse(replies[0]).find("result")->as_array();
    REQUIRE(symbols.size() == 2);
    CHECK(symbols[0].find("name")->as_string() == "x");
    CHECK(symbols[0].find("kind")->as_number() == 13);
    CHECK(symbols[1].find("name")->as_string() == "f");
    CHECK(symbols[1].find("kind")->as_number() == 12);
    const auto x_range = range_of(*symbols[0].find("location")->find("range"));
    CHECK(x_range[0] == 0);
    CHECK(x_range[1] == 8);

    replies = server.handle(request(4, "textDocument/definition", json::object{
        {"textDocument", json::object{{"uri", uri}}}, {"position", position(1, 15)}}));
    REQUIRE(replies.size() == 1);
    const auto definition = *parse(replies[0]).find("result");
    CHECK(definition.find("uri")->as_string() == uri);
    CHECK(range_of(*definition.find("range"))[0] == 0);
    CHECK(range_of(*definition.find("range"))[1] == 8);

    replies = server.handle(request(5, "textDocument/definition", json::object{
        {"textDocument", json::object{{"uri", uri}}}, {"position", position(1, 2)}}));
    CHECK(parse(replies.at(0)).find("result")->is_null());

    replies = server.handle(notify("textDocument/didChange", json::object{
        {"textDocument", json::object{{"uri", uri}, {"version", 2}}},
        {"contentChanges", json::array{json::object{{"text", "x: int = ;\n"}}}}}));
    REQUIRE(replies.size() == 1);
    published = parse(replies[0]);
    CHECK(published.find("params")->find("version")->as_number() == 2);
    const auto& diagnostics = published.find("params")->find("diagnostics")->as_array();
    REQUIRE_FALSE(diagnostics.empty());
    CHECK(diagnostics[0].find("severity")->as_number() == 1);
    CHECK(range_of(*diagnostics[0].find("range"))[0] == 0);

    replies = server.handle(notify("textDocument/didChange", json::object{
        {"textDocument", json::object{{"uri", uri}, {"version", 3}}},
        {"contentChanges", json::array{json::object{{"text",
            "P: type = { public s: std::string = (); }\n"
            "f: (move p: P) = { x := p.s; y := p.s; }\n"}}}}}));
    REQUIRE(replies.size() == 1);
    const auto warned = parse(replies[0]);
    const auto& warnings = warned.find("params")->find("diagnostics")->as_array();
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("severity")->as_number() == 2);
    CHECK(warnings[0].find("code")->as_string() == "member-copied-not-moved");

    replies = server.handle(notify("textDocument/didClose", json::object{
        {"textDocument", json::object{{"uri", uri}}}}));
    REQUIRE(replies.size() == 1);
    CHECK(parse(replies[0]).find("params")->find("diagnostics")->as_array().empty());
    replies = server.handle(request(6, "textDocument/documentSymbol",
        json::object{{"textDocument", json::object{{"uri", uri}}}}));
    CHECK(parse(replies.at(0)).find("result")->as_array().empty());

    CHECK(parse(server.handle(request(7, "workspace/symbol", json::object{})).at(0))
              .find("error")->find("code")->as_number() == -32601);
    CHECK(parse(server.handle(request(8, "textDocument/definition", json::object{})).at(0))
              .find("error")->find("code")->as_number() == -32602);
    CHECK(parse(server.handle("{not json").at(0)).find("error")->find("code")->as_number() == -32700);
    CHECK(parse(server.handle("{\"jsonrpc\":\"2.0\",\"id\":9}").at(0))
              .find("error")->find("code")->as_number() == -32600);
    CHECK(server.handle(notify("$/cancelRequest", json::object{{"id", 1}})).empty());

    replies = server.handle(request(10, "shutdown", {}));
    CHECK(parse(replies.at(0)).find("result")->is_null());
    CHECK(parse(server.handle(request(11, "textDocument/documentSymbol", json::object{})).at(0))
              .find("error")->find("code")->as_number() == -32600);
    CHECK_FALSE(server.exited());
    CHECK(server.handle(notify("exit", {})).empty());
    CHECK(server.exited());
    CHECK(server.exit_code() == 0);
}

TEST_CASE("LSP session rejects batches, fractions and unsupported changes", "[lsp]") {
    auto server = lsp::session{};
    const auto uri = std::string{"file:///work/example.cpp2"};
    const auto code = [](const std::vector<std::string>& replies) {
        return parse(replies.at(0)).find("error")->find("code")->as_number();
    };
    const auto change = [&](int version, json::value value) {
        return notify("textDocument/didChange", json::object{
            {"textDocument", json::object{{"uri", uri}, {"version", version}}},
            {"contentChanges", json::array{std::move(value)}}});
    };
    static_cast<void>(server.handle(request(1, "initialize", json::object{})));

    CHECK(code(server.handle("[" + request(2, "shutdown", {}) + "]")) == -32600);
    CHECK_FALSE(server.exited());

    CHECK(server.handle(change(1, json::object{{"text", "x: int = ;\n"}})).empty());
    CHECK(parse(server.handle(request(3, "textDocument/documentSymbol",
        json::object{{"textDocument", json::object{{"uri", uri}}}})).at(0))
              .find("result")->as_array().empty());

    CHECK(server.handle(notify("textDocument/didOpen", json::object{{"textDocument",
        json::object{{"uri", uri}, {"version", 2}, {"text", "x: int = 1;\n"}}}})).size() == 1);
    CHECK(server.handle(change(1, json::object{{"text", "y: int = 1;\n"}})).empty());
    const auto range = json::object{{"start", position(0, 0)}, {"end", position(0, 1)}};
    CHECK(code(server.handle(request(4, "textDocument/didChange", json::object{
        {"textDocument", json::object{{"uri", uri}, {"version", 3}}},
        {"contentChanges", json::array{json::object{{"range", range}, {"text", "z"}}}}}))) ==
          -32602);
    const auto symbols = parse(server.handle(request(5, "textDocument/documentSymbol",
        json::object{{"textDocument", json::object{{"uri", uri}}}})).at(0)).find("result")->as_array();
    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].find("name")->as_string() == "x");

    CHECK(code(server.handle(request(6, "textDocument/definition", json::object{
        {"textDocument", json::object{{"uri", uri}}},
        {"position", json::object{{"line", 0.5}, {"character", 0}}}}))) == -32602);
}

TEST_CASE("LSP session rejects a second initialize and clamps a large character",
          "[lsp][review]") {
    auto server = lsp::session{};
    const auto uri = std::string{"file:///work/example.cpp2"};
    static_cast<void>(server.handle(request(1, "initialize", json::object{})));
    CHECK(server.handle(notify("textDocument/didOpen", json::object{{"textDocument",
        json::object{{"uri", uri}, {"version", 1},
                     {"text", "x: int = 1;\nf: () -> int = x;\n"}}}})).size() == 1);

    const auto again = parse(server.handle(request(2, "initialize", json::object{})).at(0));
    CHECK(again.find("error")->find("code")->as_number() == -32600);
    const auto symbols = parse(server.handle(request(3, "textDocument/documentSymbol",
        json::object{{"textDocument", json::object{{"uri", uri}}}})).at(0));
    CHECK(symbols.find("result")->as_array().size() == 2);

    const auto definition = [&](int id, double character) {
        return *parse(server.handle(request(id, "textDocument/definition", json::object{
            {"textDocument", json::object{{"uri", uri}}},
            {"position", json::object{{"line", 1}, {"character", character}}}})).at(0))
            .find("result");
    };
    CHECK(definition(4, 15).is_object());
    // 2^32 + 15 is past the line's end, not character 15.
    CHECK(definition(5, 4294967311.0).is_null());
}

TEST_CASE("LSP exit without shutdown fails", "[lsp]") {
    auto server = lsp::session{};
    server.handle(request(1, "initialize", json::object{}));
    server.handle(notify("exit", {}));
    CHECK(server.exited());
    CHECK(server.exit_code() == 1);
}

#if CPPFRONTIER_POSIX_PROCESS
TEST_CASE("cppfrontier-lsp serves standard input", "[lsp][cli]") {
    const auto scratch = test_support::temp_file{std::filesystem::temp_directory_path(), "cppfrontier-lsp"};
    const auto uri = std::string{"file:///a.cpp2"};
    test_support::write_file(scratch.path,
        frame(request(1, "initialize", json::object{})) +
        frame(notify("initialized", json::object{})) +
        frame(notify("textDocument/didOpen", json::object{{"textDocument", json::object{
            {"uri", uri}, {"languageId", "cpp2"}, {"version", 1}, {"text", "main: () = { }\n"}}}})) +
        frame(request(2, "textDocument/documentSymbol",
                      json::object{{"textDocument", json::object{{"uri", uri}}}})) +
        frame(request(3, "shutdown", {})) + frame(notify("exit", {})));

    const auto result = test_support::run_process(
        {"sh", "-c", "exec \"$0\" < \"$1\"", CPPFRONTIER_LSP, scratch.path.string()},
        std::filesystem::temp_directory_path(), std::chrono::seconds{10});
    CHECK_FALSE(result.timed_out);
    CHECK(result.exit_status == 0);

    auto output = std::istringstream{result.output};
    auto messages = std::vector<json::value>{};
    while (const auto message = lsp::read_message(output)) messages.push_back(parse(*message));
    REQUIRE(messages.size() == 4);
    CHECK(messages[0].find("id")->as_number() == 1);
    CHECK(messages[1].find("method")->as_string() == "textDocument/publishDiagnostics");
    CHECK(messages[2].find("result")->as_array().at(0).find("name")->as_string() == "main");
    CHECK(messages[3].find("id")->as_number() == 3);
}

TEST_CASE("cppfrontier-lsp fails when input ends without exit", "[lsp][cli]") {
    const auto scratch = test_support::temp_file{std::filesystem::temp_directory_path(), "cppfrontier-lsp"};
    test_support::write_file(scratch.path, frame(request(1, "initialize", json::object{})));
    const auto result = test_support::run_process(
        {"sh", "-c", "exec \"$0\" < \"$1\"", CPPFRONTIER_LSP, scratch.path.string()},
        std::filesystem::temp_directory_path(), std::chrono::seconds{10});
    CHECK_FALSE(result.timed_out);
    CHECK(result.exit_status == 1);
}
#endif

TEST_CASE("LSP symbols of one long line take linear time", "[lsp][hunt]") {
    auto server = lsp::session{};
    const auto uri = std::string{"file:///work/long.cpp2"};
    auto text = std::string{};
    for (auto index = 0; index < 6000; ++index) {
        text += "a" + std::to_string(index) + ": int = 0; ";
    }
    static_cast<void>(server.handle(request(1, "initialize", json::object{})));
    static_cast<void>(server.handle(notify("textDocument/didOpen", json::object{{"textDocument",
        json::object{{"uri", uri}, {"version", 1}, {"text", text + "\n"}}}})));

    const auto start = std::chrono::steady_clock::now();
    const auto symbols = parse(server.handle(request(2, "textDocument/documentSymbol",
        json::object{{"textDocument", json::object{{"uri", uri}}}})).at(0));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{5});
    const auto& result = symbols.find("result")->as_array();
    REQUIRE(result.size() == 6000);
    CHECK(range_of(*result.front().find("location")->find("range")) ==
          std::vector<double>{0, 0, 0, 12});
    CHECK(range_of(*result.back().find("location")->find("range")) ==
          std::vector<double>{0, 94874, 0, 94890});
}
