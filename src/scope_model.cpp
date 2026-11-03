#include "scope_model.hpp"

#include <algorithm>
#include <iterator>

namespace cppfrontier {

namespace {

auto before(const source_position& left, const source_position& right) -> bool {
    return left.line != right.line ? left.line < right.line : left.column < right.column;
}

auto contains(const source_span& outer, const source_position& inner) -> bool {
    return !before(inner, outer.begin) && !before(outer.end, inner);
}

auto is_function_expression(const ir_declaration& declaration) -> bool {
    return is_function_expression_name(declaration.name);
}

// `N::f<T>` -> {"N", "f"}; a leading `::` gives an empty first part.
auto qualified_parts(std::string_view name) -> std::vector<std::string_view> {
    auto parts = std::vector<std::string_view>{};
    auto depth = 0;
    auto begin = std::size_t{0};
    for (auto index = std::size_t{0}; index < name.size(); ++index) {
        if (name[index] == '<') ++depth;
        else if (name[index] == '>') --depth;
        else if (depth == 0 && name.compare(index, 2, "::") == 0) {
            parts.push_back(name.substr(begin, index - begin));
            begin = index + 2;
            ++index;
        }
    }
    parts.push_back(name.substr(begin));
    for (auto& part : parts) {
        part = part.substr(0, part.find('<'));
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
    }
    return parts;
}

auto entry_kind_of(declaration_kind kind) -> scope_model::entry_kind {
    switch (kind) {
    case declaration_kind::function: return scope_model::entry_kind::function;
    case declaration_kind::namespace_node: return scope_model::entry_kind::namespace_node;
    case declaration_kind::type: return scope_model::entry_kind::type;
    case declaration_kind::concept_declaration: return scope_model::entry_kind::concept_declaration;
    case declaration_kind::using_declaration: return scope_model::entry_kind::using_name;
    case declaration_kind::object: break;
    }
    return scope_model::entry_kind::object;
}

constexpr auto none = scope_point::none;

}

scope_model::scope_model(const ir_program& program) {
    static_cast<void>(add_scope(scope_kind::namespace_scope, none, none, {}, false));
    for (const auto& value : program.symbols) {
        if (value.id != invalid_symbol && value.parent != invalid_symbol) {
            parameter_symbols_[{value.name, value.span.begin.line, value.span.begin.column}]
                .push_back(&value);
        }
    }
    auto function_expressions = std::vector<const ir_declaration*>{};
    for (const auto& declaration : program.declarations) {
        if (is_function_expression(declaration)) function_expressions.push_back(&declaration);
        else add_declaration(declaration, 0, false);
    }
    // Enclosing expressions first, so a nested one is placed inside its parent.
    std::stable_sort(function_expressions.begin(), function_expressions.end(),
        [](const ir_declaration* left, const ir_declaration* right) {
            if (before(left->span.begin, right->span.begin)) return true;
            if (before(right->span.begin, left->span.begin)) return false;
            return before(right->span.end, left->span.end);
        });
    for (const auto* declaration : function_expressions) {
        auto point = at(declaration->span.begin);
        if (!point.valid()) point = {0, none};
        static_cast<void>(add_function_scope(*declaration, scope_kind::function_expression,
                                             point.scope, point.ordinal, false));
    }
    parameter_symbols_.clear();
}

auto scope_model::add_scope(scope_kind kind, std::size_t parent, std::size_t anchor,
                            std::vector<source_span> spans, bool generated) -> std::size_t {
    const auto index = scopes_.size();
    scopes_.push_back({kind, parent, anchor, std::move(spans), {}, {}, {}, generated, none});
    if (parent == none) return index;
    scopes_[parent].children.push_back(index);
    if (generated) return index;
    if (kind == scope_kind::block || kind == scope_kind::statement_parameters) {
        scopes_[parent].items[anchor].blocks.push_back(index);
    }
    else {
        for (const auto& span : scopes_[index].spans) add_region(parent, span, index);
    }
    return index;
}

auto scope_model::add_item(std::size_t scope, source_span span) -> std::size_t {
    auto& owner = scopes_[scope];
    if (!owner.items.empty()) {
        owner.begins_sorted = owner.begins_sorted && !before(span.begin, owner.items.back().span.begin);
        owner.ends_sorted = owner.ends_sorted && !before(span.end, owner.items.back().span.end);
    }
    owner.items.push_back({span, {}});
    return owner.items.size() - 1;
}

auto scope_model::add_region(std::size_t scope, source_span span, std::size_t child) -> void {
    auto& regions = scopes_[scope].regions;
    const auto position = std::upper_bound(regions.begin(), regions.end(), span.begin,
        [](const source_position& value, const auto& region) {
            return before(value, region.first.begin);
        });
    const auto inserted = regions.insert(position, {span, child});
    if ((inserted != regions.begin() && !before(std::prev(inserted)->first.end, span.begin)) ||
        (std::next(inserted) != regions.end() && !before(span.end, std::next(inserted)->first.begin))) {
        scopes_[scope].regions_disjoint = false;
    }
}

auto scope_model::parameter_symbol(const parameter& value,
                                   std::optional<symbol_id> parent) const -> symbol_id {
    const auto found = parameter_symbols_.find(
        {value.name, value.name_span.begin.line, value.name_span.begin.column});
    if (found == parameter_symbols_.end()) return invalid_symbol;
    for (const auto* candidate : found->second) {
        if (!parent || candidate->parent == *parent) return candidate->id;
    }
    return invalid_symbol;
}

auto scope_model::add_entry(entry value) -> std::size_t {
    const auto index = entries_.size();
    if (!value.name.empty()) scopes_[value.scope].names[value.name].push_back(index);
    if (value.declared_parameter != nullptr) {
        parameter_entries_.try_emplace(value.declared_parameter, index);
    }
    entries_.push_back(std::move(value));
    return index;
}

auto scope_model::add_function_scope(const ir_declaration& declaration, scope_kind kind,
                                     std::size_t parent, std::size_t anchor, bool generated)
    -> std::size_t {
    const auto scope = add_scope(kind, parent, anchor, {declaration.span}, generated);
    const auto signature = add_item(scope, declaration.span);
    for (const auto& parameter : declaration.template_parameters) {
        auto value = entry{};
        value.name = parameter.name;
        value.kind = entry_kind::template_parameter;
        value.type = parameter.type;
        value.span = declaration.span;
        value.scope = scope;
        value.ordinal = signature;
        static_cast<void>(add_entry(std::move(value)));
    }
    for (const auto& parameter : declaration.parameters) {
        auto value = entry{};
        value.name = parameter.name;
        value.kind = entry_kind::parameter;
        value.span = parameter.name_span;
        value.scope = scope;
        value.ordinal = signature;
        value.type = parameter.type;
        value.declared_parameter = &parameter;
        value.symbol = parameter_symbol(parameter, declaration.symbol);
        static_cast<void>(add_entry(std::move(value)));
    }
    // A generated function that shares its source's body is inside the
    // source's scope, where the body's names find the source's parameters.
    const auto* body = declaration.function_statements.get();
    const auto shared = body != nullptr && !body->statements.empty()
        ? statement_points_.find(body->statements.front().get()) : statement_points_.end();
    if (declaration.symbol != invalid_symbol) {
        declaration_scopes_.try_emplace(declaration.symbol,
            shared != statement_points_.end() ? shared->second.scope : scope);
    }
    if (body != nullptr && shared == statement_points_.end()) {
        add_statements(body->statements, scope, generated);
    }
    return scope;
}

auto scope_model::add_declaration(const ir_declaration& declaration, std::size_t scope,
                                  bool generated) -> void {
    generated = generated || declaration.is_generated;
    if (declaration.kind == declaration_kind::using_declaration &&
        declaration.using_name.ends_with("::_")) {
        scopes_[scope].using_namespaces.emplace_back(
            scopes_[scope].items.size(),
            declaration.using_name.substr(0, declaration.using_name.size() - 3));
        return;
    }
    if (declaration.name == "this") return;
    const auto ordinal = add_item(scope, declaration.span);
    auto value = entry{};
    // A constructor is named `operator=`, so it does not hide its type's name.
    value.name = !declaration.special_member_owner.empty() &&
            declaration.name == declaration.special_member_owner
        ? std::string{"operator="} : declaration.name;
    value.kind = entry_kind_of(declaration.kind);
    value.symbol = declaration.symbol;
    value.span = declaration.span;
    value.scope = scope;
    value.ordinal = ordinal;
    value.type = declaration.type;
    value.is_template = !declaration.template_parameters.empty();
    value.has_this = declaration.kind == declaration_kind::function &&
        receiver(declaration) != nullptr;
    value.is_static_or_alias = !declaration.alias.empty();
    if (declaration.kind == declaration_kind::type) value.alias = declaration.alias;
    if (declaration.kind == declaration_kind::using_declaration) {
        const auto separator = declaration.using_name.rfind("::");
        value.name = separator == std::string::npos ? declaration.using_name
                                                    : declaration.using_name.substr(separator + 2);
        value.target = declaration.using_name;
    }
    if (declaration.kind == declaration_kind::namespace_node && !declaration.alias.empty()) {
        value.target = declaration.alias;
    }
    switch (declaration.kind) {
    // A concept's scope holds its template parameters.
    case declaration_kind::function:
    case declaration_kind::concept_declaration:
        value.inner = add_function_scope(declaration, scope_kind::function, scope, ordinal,
                                         generated);
        break;
    case declaration_kind::namespace_node:
        if (!declaration.alias.empty()) break;
        // A reopened namespace shares the scope of its first declaration.
        if (const auto* existing = find_in(scope, none, declaration.name);
            existing != nullptr && existing->kind == entry_kind::namespace_node &&
            existing->inner != none) {
            value.inner = existing->inner;
            scopes_[value.inner].spans.push_back(declaration.span);
            if (!scopes_[value.inner].generated) add_region(scope, declaration.span, value.inner);
        }
        else {
            value.inner = add_scope(scope_kind::namespace_scope, scope, ordinal,
                                    {declaration.span}, generated);
        }
        break;
    case declaration_kind::type:
        if (!declaration.alias.empty()) break;
        value.inner = add_scope(scope_kind::type, scope, ordinal, {declaration.span}, generated);
        break;
    default:
        break;
    }
    const auto index = add_entry(std::move(value));
    const auto inner = entries_[index].inner;
    if (declaration.symbol != invalid_symbol) {
        declaration_entries_.try_emplace(declaration.symbol, index);
        if (inner != none && declaration.kind != declaration_kind::function) {
            declaration_scopes_.try_emplace(declaration.symbol, inner);
        }
    }
    if (inner == none || declaration.kind == declaration_kind::function ||
        declaration.kind == declaration_kind::concept_declaration) {
        return;
    }
    if (scopes_[inner].owner == none) scopes_[inner].owner = index;
    for (const auto& parameter : declaration.template_parameters) {
        auto template_value = entry{};
        template_value.name = parameter.name;
        template_value.kind = entry_kind::template_parameter;
        template_value.type = parameter.type;
        template_value.span = declaration.span;
        template_value.scope = inner;
        static_cast<void>(add_entry(std::move(template_value)));
    }
    for (const auto& member : declaration.members) add_declaration(member, inner, generated);
}

auto scope_model::add_statements(const statement_list& statements, std::size_t scope,
                                 bool generated) -> void {
    for (const auto& value : statements) add_statement(*value, scope, generated);
}

auto scope_model::add_statement(const statement& value, std::size_t scope, bool generated)
    -> void {
    const auto ordinal = add_item(scope, value.span);
    statement_points_.try_emplace(&value, scope_point{scope, ordinal});
    // A let-else declaration's names are declared at an item after the
    // statement, outside its initializer and `else` block.
    const auto declared = [&](const auto& form) {
        if (!form.is_let_else) return ordinal;
        add_nested(form.else_body, scope, ordinal, nullptr, generated);
        return add_item(scope, {value.span.end, value.span.end});
    };
    std::visit([&](const auto& form) {
        using form_type = std::remove_cvref_t<decltype(form)>;
        if constexpr (std::is_same_v<form_type, statement::declaration_form>) {
            const auto declared_at = declared(form);
            if (!form.declaration) return;
            const auto& data = *form.declaration;
            auto local = entry{};
            local.name = data.name;
            local.kind = entry_kind_of(data.kind);
            local.symbol = data.symbol;
            local.span = value.span;
            local.scope = scope;
            local.ordinal = declared_at;
            local.type = data.type;
            local.target = data.kind == declaration_kind::namespace_node ? data.alias : "";
            local.is_template = !data.template_parameters.empty();
            local.is_static_or_alias = data.is_static || data.is_alias;
            if (data.kind == declaration_kind::type) local.alias = data.alias;
            local.declaring_statement = &value;
            statement_entries_.try_emplace(&value, add_entry(std::move(local)));
        }
        else if constexpr (std::is_same_v<form_type, statement::destructuring_form>) {
            const auto declared_at = declared(form);
            for (const auto& binding : form.bindings) {
                if (binding.name == "_") continue;
                auto local = entry{};
                local.name = binding.name;
                local.kind = entry_kind::object;
                local.symbol = binding.symbol;
                local.span = binding.span;
                local.scope = scope;
                local.ordinal = declared_at;
                local.type = parse_type("auto");
                local.declaring_statement = &value;
                binding_entries_.try_emplace(&binding, add_entry(std::move(local)));
            }
        }
        else if constexpr (std::is_same_v<form_type, statement::using_form>) {
            if (form.name.ends_with("::_")) {
                scopes_[scope].using_namespaces.emplace_back(
                    ordinal, form.name.substr(0, form.name.size() - 3));
                return;
            }
            auto local = entry{};
            const auto separator = form.name.rfind("::");
            local.name = separator == std::string::npos ? form.name
                                                        : form.name.substr(separator + 2);
            local.kind = entry_kind::using_name;
            local.span = value.span;
            local.scope = scope;
            local.ordinal = ordinal;
            local.target = form.name;
            local.declaring_statement = &value;
            static_cast<void>(add_entry(std::move(local)));
        }
        else if constexpr (std::is_same_v<form_type, statement::block_form>) {
            if (!form.parameters.empty()) {
                const auto parameters = add_scope(scope_kind::statement_parameters, scope, ordinal,
                                                  {value.span}, generated);
                for (const auto& parameter : form.parameters) {
                    auto local = entry{};
                    local.name = parameter.value.name;
                    local.kind = entry_kind::statement_parameter;
                    local.span = parameter.value.name_span;
                    local.scope = parameters;
                    local.ordinal = add_item(parameters, parameter.value.name_span);
                    local.type = parameter.value.type;
                    local.declared_parameter = &parameter.value;
                    if (!generated) local.symbol = parameter_symbol(parameter.value, {});
                    statement_points_.try_emplace(&parameter,
                                                  scope_point{parameters, local.ordinal});
                    static_cast<void>(add_entry(std::move(local)));
                }
                // The body is an item after the parameters, outside their initializers.
                const auto body_begin = form.body.empty() ? value.span.end
                                                          : form.body.front()->span.begin;
                add_nested(form.body, parameters,
                           add_item(parameters, {body_begin, value.span.end}), nullptr, generated);
                return;
            }
            add_nested(form.body, scope, ordinal, nullptr, generated);
            for (const auto& handler : form.handlers) {
                add_nested(handler.body, scope, ordinal, &handler.caught, generated);
            }
        }
        else if constexpr (std::is_same_v<form_type, statement::if_form>) {
            add_bound(value, form.bindings, form.body, scope, ordinal, generated);
            add_nested(form.else_body, scope, ordinal, nullptr, generated);
        }
        else if constexpr (std::is_same_v<form_type, statement::loop_form>) {
            const auto body = form.bindings.empty()
                ? add_nested(form.body, scope, ordinal,
                             form.range_parameter ? &*form.range_parameter : nullptr, generated)
                : add_bound(value, form.bindings, form.body, scope, ordinal, generated);
            // A do statement's condition follows its body, outside it.
            if (body != none && value.kind == statement_kind::do_statement) {
                scopes_[body].spans.front().end = value.span.end;
            }
        }
    }, value.form);
}

auto scope_model::add_bound(const statement& value,
                            const std::vector<statement::binding>& bindings,
                            const statement_list& body, std::size_t parent, std::size_t anchor,
                            bool generated) -> std::size_t {
    if (bindings.empty()) return add_nested(body, parent, anchor, nullptr, generated);
    const auto begin = bindings.front().span.begin;
    const auto bound = add_scope(scope_kind::statement_parameters, parent, anchor,
                                 {{begin, begin}}, generated);
    for (const auto& binding : bindings) {
        auto local = entry{};
        local.name = binding.name;
        local.kind = entry_kind::object;
        local.symbol = binding.symbol;
        local.span = binding.span;
        local.scope = bound;
        local.ordinal = add_item(bound, binding.span);
        local.type = parse_type("auto");
        local.declaring_statement = &value;
        binding_entries_.try_emplace(&binding, add_entry(std::move(local)));
    }
    const auto body_begin = body.empty() ? bindings.back().span.end : body.front()->span.begin;
    return add_nested(body, bound, add_item(bound, {body_begin, body_begin}), nullptr, generated);
}

auto scope_model::add_nested(const statement_list& statements, std::size_t parent,
                             std::size_t anchor, const parameter* leading, bool generated)
    -> std::size_t {
    const auto named = leading != nullptr && !leading->name.empty() && leading->name != "_";
    if (statements.empty() && !named) return none;
    const auto begin = named ? leading->name_span.begin : statements.front()->span.begin;
    const auto scope = add_scope(scope_kind::block, parent, anchor, {{begin, begin}}, generated);
    if (named) {
        auto local = entry{};
        local.name = leading->name;
        local.kind = entry_kind::parameter;
        local.span = leading->name_span;
        local.scope = scope;
        local.ordinal = add_item(scope, leading->name_span);
        local.type = leading->type;
        local.declared_parameter = leading;
        if (!generated) local.symbol = parameter_symbol(*leading, {});
        static_cast<void>(add_entry(std::move(local)));
    }
    add_statements(statements, scope, generated);
    return scope;
}

auto scope_model::ordered(std::size_t scope) const -> bool {
    const auto kind = scopes_[scope].kind;
    return kind != scope_kind::namespace_scope && kind != scope_kind::type;
}

auto scope_model::find_in(std::size_t scope, std::size_t ordinal, std::string_view name) const
    -> const entry* {
    const auto& names = scopes_[scope].names;
    const auto found = names.find(name);
    if (found == names.end()) return nullptr;
    if (!ordered(scope)) return &entries_[found->second.front()];
    const entry* result = nullptr;
    for (const auto index : found->second) {
        const auto& candidate = entries_[index];
        if (candidate.ordinal <= ordinal &&
            (result == nullptr || result->ordinal <= candidate.ordinal)) {
            result = &candidate;
        }
    }
    return result;
}

auto scope_model::unqualified(std::string_view name, scope_point point, int depth) const
    -> const entry* {
    for (auto scope = point.scope, ordinal = point.ordinal; scope != none;
         ordinal = scopes_[scope].anchor, scope = scopes_[scope].parent) {
        if (const auto* found = find_in(scope, ordinal, name)) return found;
        for (const auto& [at, target] : scopes_[scope].using_namespaces) {
            if (ordered(scope) && at > ordinal) continue;
            const auto* named = qualified(target, {scope, at}, depth + 1);
            if (named == nullptr) continue;
            named = resolve(*named, depth + 1);
            if (named == nullptr || named->inner == none) continue;
            if (const auto* found = find_in(named->inner, none, name)) return found;
        }
    }
    return nullptr;
}

auto scope_model::qualified(std::string_view name, scope_point point, int depth) const
    -> const entry* {
    if (depth > 8) return nullptr;
    const auto parts = qualified_parts(name);
    if (parts.size() == 1) return unqualified(parts.front(), point, depth);
    auto scope = std::size_t{0};
    for (auto index = std::size_t{0}; index + 1 < parts.size(); ++index) {
        if (index == 0 && parts.front().empty()) continue;
        const auto* found = index == 0 ? unqualified(parts[index], point, depth)
                                       : find_in(scope, none, parts[index]);
        if (found != nullptr) found = resolve(*found, depth + 1);
        // An alias of a type that is not a specialization, as `A` in `A::f`.
        if (found != nullptr && found->kind == entry_kind::type && !found->alias.empty() &&
            found->alias.find('<') == std::string::npos) {
            found = qualified(found->alias, {found->scope, found->ordinal}, depth + 1);
        }
        if (found == nullptr || found->inner == none ||
            found->kind == entry_kind::function) {
            return nullptr;
        }
        scope = found->inner;
    }
    return find_in(scope, none, parts.back());
}

auto scope_model::resolve(const entry& value, int depth) const -> const entry* {
    if (value.target.empty() || depth > 8) return &value;
    const auto* found = qualified(value.target, {value.scope, value.ordinal}, depth + 1);
    if (found == nullptr || found == &value) return &value;
    return resolve(*found, depth + 1);
}

auto scope_model::resolve(const entry& value) const -> const entry* { return resolve(value, 0); }

auto scope_model::lookup(std::string_view name, scope_point point) const -> const entry* {
    if (!point.valid() || name.empty()) return nullptr;
    return qualified(name, point, 0);
}

auto scope_model::member(const entry& owner, std::string_view name) const -> const entry* {
    const auto* resolved = resolve(owner);
    if (resolved == nullptr || resolved->inner == none ||
        resolved->kind == entry_kind::function) {
        return nullptr;
    }
    return find_in(resolved->inner, none, name);
}

auto scope_model::enclosing_type(scope_point point) const -> const entry* {
    for (auto scope = point.scope; scope != none; scope = scopes_[scope].parent) {
        if (scopes_[scope].kind == scope_kind::type) {
            const auto owner = scopes_[scope].owner;
            return owner == none ? nullptr : &entries_[owner];
        }
    }
    return nullptr;
}

auto scope_model::under_initialization(const entry& value, scope_point point) const -> bool {
    if (value.kind != entry_kind::object && value.kind != entry_kind::statement_parameter) {
        return false;
    }
    for (auto scope = point.scope, ordinal = point.ordinal; scope != none;
         ordinal = scopes_[scope].anchor, scope = scopes_[scope].parent) {
        if (scope == value.scope) return ordinal == value.ordinal;
    }
    return false;
}

auto scope_model::declared_in(const entry& value, const entry& owner) const -> bool {
    const auto* resolved = resolve(owner);
    return resolved != nullptr && resolved->inner != none && value.scope == resolved->inner;
}

auto scope_model::declared_by(const statement& value) const -> const entry* {
    const auto found = statement_entries_.find(&value);
    return found == statement_entries_.end() ? nullptr : &entries_[found->second];
}

auto scope_model::declared_by(const statement::destructuring_form::binding& value) const
    -> const entry* {
    const auto found = binding_entries_.find(&value);
    return found == binding_entries_.end() ? nullptr : &entries_[found->second];
}

auto scope_model::declared_as(const parameter& value) const -> const entry* {
    const auto found = parameter_entries_.find(&value);
    return found == parameter_entries_.end() ? nullptr : &entries_[found->second];
}

auto scope_model::parameter_of(symbol_id function, std::string_view name) const
    -> const entry* {
    const auto found = declaration_scopes_.find(function);
    if (found == declaration_scopes_.end()) return nullptr;
    const auto* value = find_in(found->second, 0, name);
    return value != nullptr && value->kind == entry_kind::parameter ? value : nullptr;
}

auto scope_model::at(const statement& value) const -> scope_point {
    const auto found = statement_points_.find(&value);
    return found == statement_points_.end() ? scope_point{} : found->second;
}

auto scope_model::at(const initialized_parameter& value) const -> scope_point {
    const auto found = statement_points_.find(&value);
    return found == statement_points_.end() ? scope_point{} : found->second;
}

// Declaration spans cover the declaration, but a statement's span may end at
// its first token, so a block scope extends from its first item to the next
// block scope or item of its parent, or to its end when that is known. Items
// and regions in source order are searched by bisection.
auto scope_model::at(source_position position) const -> scope_point {
    const auto starts_after = [&](const source_position& value, const auto& element) {
        return before(value, element.span.begin);
    };
    auto scope = std::size_t{0};
    for (;;) {
        const auto& current = scopes_[scope];
        const auto& items = current.items;
        auto ordinal = ordered(scope) ? std::size_t{0} : none;
        if (current.begins_sorted && (ordered(scope) || current.ends_sorted)) {
            const auto count = static_cast<std::size_t>(
                std::upper_bound(items.begin(), items.end(), position, starts_after) -
                items.begin());
            if (count > 0 && (ordered(scope) || contains(items[count - 1].span, position))) {
                ordinal = count - 1;
            }
        }
        else {
            for (auto index = std::size_t{0}; index < items.size(); ++index) {
                if (ordered(scope) ? !before(position, items[index].span.begin)
                                   : contains(items[index].span, position)) {
                    ordinal = index;
                }
            }
        }
        auto next = none;
        if (current.regions_disjoint) {
            const auto found = std::upper_bound(current.regions.begin(), current.regions.end(),
                position, [](const source_position& value, const auto& region) {
                    return before(value, region.first.begin);
                });
            if (found != current.regions.begin() && contains(std::prev(found)->first, position)) {
                next = std::prev(found)->second;
            }
        }
        else {
            for (const auto child : current.children) {
                const auto& candidate = scopes_[child];
                if (candidate.generated || candidate.kind == scope_kind::block ||
                    candidate.kind == scope_kind::statement_parameters) {
                    continue;
                }
                if (std::any_of(candidate.spans.begin(), candidate.spans.end(),
                                [&](const source_span& span) { return contains(span, position); })) {
                    next = child;
                    break;
                }
            }
        }
        if (next == none && ordinal < items.size()) {
            for (const auto child : items[ordinal].blocks) {
                const auto& span = scopes_[child].spans.front();
                const auto bounded = before(span.begin, span.end);
                if (!before(position, span.begin) && (!bounded || !before(span.end, position))) {
                    next = child;
                }
            }
        }
        if (next == none) return {scope, ordinal};
        scope = next;
    }
}

auto scope_model::inside(symbol_id declaration) const -> scope_point {
    if (const auto found = declaration_scopes_.find(declaration);
        found != declaration_scopes_.end()) {
        return {found->second, ordered(found->second) ? std::size_t{0} : none};
    }
    if (const auto found = declaration_entries_.find(declaration);
        found != declaration_entries_.end()) {
        const auto& value = entries_[found->second];
        return {value.scope, value.ordinal};
    }
    return {};
}

auto scope_model::outside(symbol_id declaration) const -> scope_point {
    if (const auto found = declaration_entries_.find(declaration);
        found != declaration_entries_.end()) {
        const auto& value = entries_[found->second];
        return {value.scope, value.ordinal};
    }
    if (const auto found = declaration_scopes_.find(declaration);
        found != declaration_scopes_.end()) {
        const auto& scope = scopes_[found->second];
        return {scope.parent, scope.anchor};
    }
    return {};
}

}
