#pragma once

#include <cppfrontier/ast.hpp>

// Accessors that look up a statement field in whichever form holds it.
namespace cppfrontier::test {

inline auto text_of(const statement& value) -> const parsed_expression& {
    return *statement_text(value);
}

inline auto declaration_of(const statement& value)
    -> const std::optional<statement::declaration_data>& {
    return value.as<statement::declaration_form>().declaration;
}

inline auto if_of(const statement& value) -> const statement::if_form& {
    return value.as<statement::if_form>();
}

inline auto loop_of(const statement& value) -> const statement::loop_form& {
    return value.as<statement::loop_form>();
}

inline auto condition_of(const statement& value) -> const parsed_expression& {
    return value.is<statement::if_form>() ? if_of(value).condition : loop_of(value).condition;
}

inline auto body_of(const statement& value) -> const statement_list& {
    if (value.is<statement::if_form>()) return if_of(value).body;
    if (value.is<statement::loop_form>()) return loop_of(value).body;
    return value.as<statement::block_form>().body;
}

inline auto label_of(const statement& value) -> const std::string& {
    return value.is<statement::jump_form>() ? value.as<statement::jump_form>().label
                                             : loop_of(value).label;
}

inline auto handlers_of(const statement& value) -> const std::vector<statement::handler>& {
    return value.as<statement::block_form>().handlers;
}

inline auto using_name_of(const statement& value) -> const std::string& {
    return value.as<statement::using_form>().name;
}

}
