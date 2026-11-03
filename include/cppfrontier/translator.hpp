#pragma once

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <cppfrontier/ir.hpp>
#include <cppfrontier/options.hpp>

namespace cppfrontier {

struct translation_result final {
    translation_options options;
    std::string cpp1;
    // The source of each `@print` type, written to stdout at translation time.
    std::string printed;
    token_list tokens;
    std::vector<diagnostic> diagnostics;
    std::vector<std::pair<source_position, source_position>> source_map;
    ir_program program;

    [[nodiscard]] auto has_errors() const noexcept -> bool {
        return std::any_of(diagnostics.begin(), diagnostics.end(), [](const diagnostic& value) {
            return value.severity == diagnostic_severity::error;
        });
    }
};

class translator final {
public:
    [[nodiscard]] translation_result translate_source(const std::string& source) const;
    [[nodiscard]] translation_result translate_source(const std::string& source, translation_options options) const;
};

struct file_request final {
    std::uint64_t index = 0;
    std::string source;
    translation_options options;
};

struct file_result final {
    std::uint64_t index = 0;
    translation_result translation;
    bool completed = false;
};

class parallel_translator final {
public:
    explicit parallel_translator(unsigned worker_count = 0) noexcept;
    // Stops the batches in progress; a cancel that stops no request stops the
    // next batch instead. Requests not yet started are returned with
    // `completed` false.
    void request_cancel();
    // A request whose translation throws completes with an `internal-error` diagnostic.
    [[nodiscard]] std::vector<file_result> translate_files(const std::vector<file_request>& requests);

private:
    struct batch;

    unsigned worker_count_;
    std::mutex mutex_;
    std::vector<batch*> running_;
    // A cancel for the next batch.
    bool pending_cancel_ = false;
    // Whether the current cancel has stopped a request of a running batch.
    bool cancel_effective_ = false;
};
}
