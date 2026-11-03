#pragma once

#include <compare>
#include <string>
#include <vector>

namespace cppfrontier::test {

struct semantic_statement final {
    std::string kind;
    std::string label;
    std::vector<std::string> declarations;
    std::vector<semantic_statement> children;

    friend auto operator==(const semantic_statement&, const semantic_statement&) -> bool = default;

    [[nodiscard]] auto is_empty_block() const noexcept -> bool {
        return kind == "CompoundStmt" && declarations.empty() && children.empty();
    }
};

struct semantic_entity final {
    std::string kind;
    std::string name;
    std::string type;
    std::string canonical_type;
    std::string access;
    bool definition = false;
    std::vector<semantic_statement> statements;
    std::vector<semantic_entity> children;

    friend auto operator==(const semantic_entity& left, const semantic_entity& right) -> bool {
        return left.kind == right.kind && left.name == right.name &&
            (left.type == right.type || left.canonical_type == right.canonical_type) &&
            left.access == right.access && left.definition == right.definition &&
            left.statements == right.statements && left.children == right.children;
    }
};

struct semantic_program final {
    std::vector<semantic_entity> entities;

    friend auto operator==(const semantic_program&, const semantic_program&) -> bool = default;
};

[[nodiscard]] auto read_semantic_program(const std::string& path,
                                         const std::string& include_path) -> semantic_program;

}
