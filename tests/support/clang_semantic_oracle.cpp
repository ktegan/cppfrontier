#include "clang_semantic_oracle.hpp"

#include <clang-c/Index.h>

#include <algorithm>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace cppfrontier::test {
namespace {

auto text(CXString value) -> std::string {
    const auto* data = clang_getCString(value);
    auto result = data ? std::string{data} : std::string{};
    clang_disposeString(value);
    return result;
}

auto kind_name(CXCursorKind kind) -> std::string {
    switch (kind) {
    case CXCursor_CompoundStmt: return "CompoundStmt";
    case CXCursor_DeclStmt: return "DeclStmt";
    case CXCursor_NullStmt: return "NullStmt";
    case CXCursor_LabelStmt: return "LabelStmt";
    case CXCursor_CaseStmt: return "CaseStmt";
    case CXCursor_DefaultStmt: return "DefaultStmt";
    case CXCursor_ReturnStmt: return "ReturnStmt";
    case CXCursor_IfStmt: return "IfStmt";
    case CXCursor_ForStmt: return "ForStmt";
    case CXCursor_CXXForRangeStmt: return "CXXForRangeStmt";
    case CXCursor_WhileStmt: return "WhileStmt";
    case CXCursor_DoStmt: return "DoStmt";
    case CXCursor_BreakStmt: return "BreakStmt";
    case CXCursor_ContinueStmt: return "ContinueStmt";
    case CXCursor_SwitchStmt: return "SwitchStmt";
    case CXCursor_GotoStmt: return "GotoStmt";
    case CXCursor_IndirectGotoStmt: return "IndirectGotoStmt";
    case CXCursor_CXXTryStmt: return "CXXTryStmt";
    case CXCursor_CXXCatchStmt: return "CXXCatchStmt";
    case CXCursor_CXXThrowExpr: return "CXXThrowExpr";
    default: return text(clang_getCursorKindSpelling(kind));
    }
}

auto access_name(CXCursor cursor) -> std::string {
    switch (clang_getCXXAccessSpecifier(cursor)) {
    case CX_CXXPublic: return "public";
    case CX_CXXProtected: return "protected";
    case CX_CXXPrivate: return "private";
    default: return {};
    }
}

auto is_entity(CXCursorKind kind) -> bool {
    switch (kind) {
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
    case CXCursor_ConversionFunction:
    case CXCursor_VarDecl:
    case CXCursor_ParmDecl:
    case CXCursor_FieldDecl:
    case CXCursor_StructDecl:
    case CXCursor_ClassDecl:
    case CXCursor_UnionDecl:
    case CXCursor_EnumDecl:
    case CXCursor_EnumConstantDecl:
    case CXCursor_TypedefDecl:
    case CXCursor_TypeAliasDecl:
    case CXCursor_Namespace:
    case CXCursor_NamespaceAlias:
    case CXCursor_ClassTemplate:
    case CXCursor_FunctionTemplate:
    case CXCursor_TypeAliasTemplateDecl:
    case CXCursor_UsingDirective:
    case CXCursor_UsingDeclaration:
        return true;
    default:
        return false;
    }
}

auto is_statement(CXCursorKind kind) -> bool {
    switch (kind) {
    case CXCursor_CompoundStmt:
    case CXCursor_DeclStmt:
    case CXCursor_NullStmt:
    case CXCursor_LabelStmt:
    case CXCursor_CaseStmt:
    case CXCursor_DefaultStmt:
    case CXCursor_ReturnStmt:
    case CXCursor_IfStmt:
    case CXCursor_ForStmt:
    case CXCursor_CXXForRangeStmt:
    case CXCursor_WhileStmt:
    case CXCursor_DoStmt:
    case CXCursor_BreakStmt:
    case CXCursor_ContinueStmt:
    case CXCursor_SwitchStmt:
    case CXCursor_GotoStmt:
    case CXCursor_IndirectGotoStmt:
    case CXCursor_CXXTryStmt:
    case CXCursor_CXXCatchStmt:
    case CXCursor_CXXThrowExpr:
        return true;
    default:
        return false;
    }
}

auto belongs_to_file(CXCursor cursor, CXFile file) -> bool {
    auto found = CXFile{};
    unsigned line = 0;
    unsigned column = 0;
    unsigned offset = 0;
    clang_getSpellingLocation(clang_getCursorLocation(cursor), &found, &line, &column, &offset);
    return found == file;
}

struct context final {
    CXTranslationUnit unit = {};
    CXFile file = {};
};

auto collect_entity(const context& state, CXCursor cursor) -> semantic_entity;

auto collect_statement(const context& state, CXCursor cursor, semantic_statement& statement) -> void {
    const auto kind = clang_getCursorKind(cursor);
    statement.kind = kind_name(kind);
    statement.label = text(clang_getCursorSpelling(cursor));
    if (kind == CXCursor_DeclStmt) {
        clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) {
            auto& declarations = *static_cast<std::vector<std::string>*>(data);
            if (clang_getCursorKind(child) == CXCursor_VarDecl) {
                declarations.push_back(text(clang_getCursorSpelling(child)));
            }
            return CXChildVisit_Continue;
        }, &statement.declarations);
    }
    struct nested_state final {
        const context* state = nullptr;
        semantic_statement* statement = nullptr;
    } nested{&state, &statement};
    clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) {
        auto& nested = *static_cast<nested_state*>(data);
        const auto kind = clang_getCursorKind(child);
        if (kind == CXCursor_NullStmt) return CXChildVisit_Continue;
        if (is_statement(kind) && belongs_to_file(child, nested.state->file)) {
            auto nested_statement = semantic_statement{};
            collect_statement(*nested.state, child, nested_statement);
            nested.statement->children.push_back(std::move(nested_statement));
            return CXChildVisit_Continue;
        }
        return CXChildVisit_Recurse;
    }, &nested);
}

auto collect_statements(const context& state, CXCursor cursor,
                        std::vector<semantic_statement>& statements) -> void {
    struct visitor_state final {
        const context* state = nullptr;
        std::vector<semantic_statement>* statements = nullptr;
    } visitor_value{&state, &statements};
    clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) {
        auto& visitor = *static_cast<visitor_state*>(data);
        const auto kind = clang_getCursorKind(child);
        if (kind == CXCursor_NullStmt) return CXChildVisit_Continue;
        if (is_statement(kind) && belongs_to_file(child, visitor.state->file) &&
            kind != CXCursor_CompoundStmt) {
            auto statement = semantic_statement{};
            collect_statement(*visitor.state, child, statement);
            visitor.statements->push_back(std::move(statement));
            return CXChildVisit_Continue;
        }
        return CXChildVisit_Recurse;
    }, &visitor_value);
}

auto simplify(std::vector<semantic_statement>& statements) -> void {
    for (auto& statement : statements) simplify(statement.children);
    std::erase_if(statements, [](const semantic_statement& statement) {
        return statement.is_empty_block();
    });
    for (auto& statement : statements) {
        while (statement.kind == "CompoundStmt" && statement.declarations.empty() &&
               statement.children.size() == 1 &&
               statement.children.front().kind == "CompoundStmt") {
            statement = std::move(statement.children.front());
        }
    }
}

auto collect_children(const context& state, CXCursor cursor, semantic_entity& entity) -> void {
    struct visitor_state final {
        const context* state = nullptr;
        semantic_entity* entity = nullptr;
    } visitor_value{&state, &entity};
    clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) {
        auto& visitor = *static_cast<visitor_state*>(data);
        const auto kind = clang_getCursorKind(child);
        if (is_entity(kind)) {
            if (belongs_to_file(child, visitor.state->file)) {
                visitor.entity->children.push_back(
                    collect_entity(*visitor.state, child));
            }
            return CXChildVisit_Continue;
        }
        return CXChildVisit_Recurse;
    }, &visitor_value);
}

auto collect_entity(const context& state, CXCursor cursor) -> semantic_entity {
    auto entity = semantic_entity{};
    entity.kind = kind_name(clang_getCursorKind(cursor));
    entity.name = text(clang_getCursorSpelling(cursor));
    entity.type = text(clang_getTypeSpelling(clang_getCursorType(cursor)));
    entity.canonical_type =
        text(clang_getTypeSpelling(clang_getCanonicalType(clang_getCursorType(cursor))));
    entity.access = access_name(cursor);
    entity.definition = clang_isCursorDefinition(cursor) != 0;
    collect_children(state, cursor, entity);
    collect_statements(state, cursor, entity.statements);
    simplify(entity.statements);
    return entity;
}

auto sort_entities(std::vector<semantic_entity>& entities) -> void {
    std::stable_sort(entities.begin(), entities.end(),
        [](const semantic_entity& left, const semantic_entity& right) {
            return std::tie(left.kind, left.name, left.type, left.canonical_type,
                            left.access, left.definition) <
                std::tie(right.kind, right.name, right.type, right.canonical_type,
                         right.access, right.definition);
        });
    for (auto& entity : entities) sort_entities(entity.children);
}

auto normalize(semantic_program& program) -> void {
    sort_entities(program.entities);
}

}

auto read_semantic_program(const std::string& path, const std::string& include_path)
    -> semantic_program {
    auto index = clang_createIndex(0, 0);
    const auto include_argument = std::string{"-I"} + include_path;
    const char* arguments[] = {"-std=c++23", "-Wno-everything", include_argument.c_str()};
    auto unit = clang_parseTranslationUnit(index, path.c_str(), arguments, 3, nullptr, 0, 0);
    if (!unit) {
        clang_disposeIndex(index);
        return {};
    }
    const auto state = context{unit, clang_getFile(unit, path.c_str())};
    auto result = semantic_program{};
    struct top_state final {
        const context* state = nullptr;
        semantic_program* program = nullptr;
    } state_data{&state, &result};
    clang_visitChildren(clang_getTranslationUnitCursor(unit), [](CXCursor cursor, CXCursor,
                                                                  CXClientData data) {
        auto& top = *static_cast<top_state*>(data);
        if (is_entity(clang_getCursorKind(cursor)) && belongs_to_file(cursor, top.state->file)) {
            top.program->entities.push_back(collect_entity(*top.state, cursor));
            return CXChildVisit_Continue;
        }
        return CXChildVisit_Continue;
    }, &state_data);
    normalize(result);
    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);
    return result;
}

}
