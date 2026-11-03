#pragma once

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cppfrontier::test {

inline auto add_pure_cpp2_preamble(std::string source, std::string_view file_name) -> std::string {
    if (!file_name.starts_with("pure2-")) return source;
    if (source.find("CPP2_INCLUDE_STD") != std::string::npos) return source;
    return "#define CPP2_INCLUDE_STD Yes\n#include \"cpp2util.h\"\n" + source;
}

struct upstream_translation final {
    bool available = false;
    bool succeeded = false;
    std::string output;
};

inline auto translate_with_upstream(std::string_view file_name,
                                    const std::filesystem::path& scratch)
    -> upstream_translation {
    const auto binary = std::filesystem::path{CPPFRONTIER_UPSTREAM_BIN};
    if (!std::filesystem::exists(binary)) return {};

    const auto source = std::filesystem::path{CPPFRONTIER_UPSTREAM_DIR} /
        "regression-tests" / file_name;
    if (!std::filesystem::exists(source)) return {};

    auto arguments = std::vector<std::string>{binary.string(), source.string(), "-o", "stdout"};
    if (file_name.starts_with("pure2-")) {
        arguments.insert(arguments.end(), {"-p", "-include-std"});
    }
    const auto result = ::test_support::run_process(
        arguments, scratch, std::chrono::seconds{90});
    return {true, result.exit_status == 0 && !result.timed_out, result.output};
}

}
