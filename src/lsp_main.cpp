#include <cppfrontier/lsp.hpp>

#include <cstring>
#include <iostream>

// A Language Server Protocol server on standard input and output.
auto main(int argc, char** argv) -> int {
    if (argc > 1) {
        const auto help = std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-help") == 0;
        (help ? std::cout : std::cerr) << "usage: cppfrontier-lsp\n"
            "Serves the Language Server Protocol over standard input and output.\n";
        return help ? 0 : 1;
    }
    std::ios::sync_with_stdio(false);
    return cppfrontier::lsp::run(std::cin, std::cout);
}
