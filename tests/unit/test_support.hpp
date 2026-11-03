#pragma once

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#define CPPFRONTIER_POSIX_PROCESS 1
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#else
#define CPPFRONTIER_POSIX_PROCESS 0
#endif

namespace test_support {

// A scratch directory unique to this test process, so concurrent test runs
// (`ctest -j`) do not share files.
inline auto scratch_directory(std::string_view name) -> std::filesystem::path {
#if CPPFRONTIER_POSIX_PROCESS
    const auto id = std::to_string(::getpid());
#else
    const auto id = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    return std::filesystem::temp_directory_path() / (std::string{name} + "-" + id);
}

inline auto read_file(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    auto buffer = std::ostringstream{};
    buffer << input.rdbuf();
    return buffer.str();
}

inline auto write_file(const std::filesystem::path& path, std::string_view text) -> void {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

struct process_result final {
    int exit_status = -1;
    bool timed_out = false;
    std::string output;
};

#if CPPFRONTIER_POSIX_PROCESS
struct temp_file final {
    std::filesystem::path path;

    temp_file(const std::filesystem::path& directory, const char* prefix) {
        const auto pattern = (directory / (std::string{prefix} + "-XXXXXX")).string();
        auto pattern_buffer = std::vector<char>{pattern.begin(), pattern.end()};
        pattern_buffer.push_back('\0');
        const auto descriptor = ::mkstemp(pattern_buffer.data());
        REQUIRE(descriptor != -1);
        ::close(descriptor);
        path = pattern_buffer.data();
    }

    ~temp_file() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    temp_file(const temp_file&) = delete;
    auto operator=(const temp_file&) -> temp_file& = delete;
};

inline auto launch(const std::vector<std::string>& arguments, const std::filesystem::path& output_path)
    -> ::pid_t {
    const auto output_descriptor = ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    REQUIRE(output_descriptor != -1);

    const auto child = ::fork();
    REQUIRE(child != -1);
    if (child == 0) {
        ::dup2(output_descriptor, STDOUT_FILENO);
        ::dup2(output_descriptor, STDERR_FILENO);
        ::close(output_descriptor);

        auto executable = std::vector<char*>{};
        executable.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            executable.push_back(const_cast<char*>(argument.c_str()));
        }
        executable.push_back(nullptr);
        // tools/strict-compile.sh --unit replaces the compiler with a wrapper that adds flags.
        const char* compiler = std::getenv("CPPFRONTIER_TEST_CXX");
        if (compiler && *compiler && arguments.front() == CPPFRONTIER_GXX) {
            executable.front() = const_cast<char*>(compiler);
        }
        ::execvp(executable.front(), executable.data());
        ::_exit(127);
    }

    ::close(output_descriptor);
    return child;
}

inline auto wait_for_process(::pid_t child, std::chrono::milliseconds timeout) -> std::pair<int, bool> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto status = 0;
    for (;;) {
        const auto waited = ::waitpid(child, &status, WNOHANG);
        REQUIRE(waited != -1);
        if (waited == child) return {status, false};
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            return {status, true};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
}

inline auto run_process(const std::vector<std::string>& arguments, const std::filesystem::path& scratch,
                        std::chrono::milliseconds timeout) -> process_result {
    const auto output_path = temp_file{scratch, "process-output"}.path;
    const auto child = launch(arguments, output_path);
    const auto [status, timed_out] = wait_for_process(child, timeout);
    auto result = process_result{};
    result.timed_out = timed_out;
    if (WIFEXITED(status)) result.exit_status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_status = 128 + WTERMSIG(status);
    result.output = read_file(output_path);
    return result;
}
#endif

}
