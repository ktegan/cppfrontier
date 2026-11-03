#include "autodiff_metafunction.hpp"

#include <cppfrontier/pipeline.hpp>

#include <algorithm>
#include <list>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "generated_members.hpp"
#include "ir_walk.hpp"

// A port of upstream's `@autodiff` (reflect.h2). Function bodies are walked
// through their statement and expression trees; the derivatives are generated
// as Cpp2 text, as upstream generates them, and parsed into IR members.
namespace cppfrontier {

namespace {

auto replace_all(std::string text, std::string_view from, std::string_view to) -> std::string {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

auto passing_name(passing_style value) -> std::string {
    switch (value) {
    case passing_style::in: return "in";
    case passing_style::in_ref: return "in_ref";
    case passing_style::copy: return "copy";
    case passing_style::inout: return "inout";
    case passing_style::out: return "out";
    case passing_style::move: return "move";
    case passing_style::forward: return "forward";
    }
    return "in";
}

// Upstream spells a deduced type `_`.
auto type_or_wildcard(std::string_view type) -> std::string {
    return type.empty() || type == "auto" ? "_" : std::string{type};
}

auto type_or_wildcard(const type_id& type) -> std::string {
    return type_or_wildcard(type.text);
}

struct autodiff_options final {
    std::string fwd_suffix = "_d";
    std::string rws_suffix = "_b";
    int order = 1;
    bool reverse = false;
    std::string error;
};

// The `@autodiff<"suffix=_d2", "order=2", "reverse">` arguments.
auto parse_autodiff_options(const std::vector<std::string>& arguments) -> autodiff_options {
    auto result = autodiff_options{};
    for (const auto& quoted : arguments) {
        if (quoted.size() < 2 || quoted.front() != '"' || quoted.back() != '"') {
            result.error = "AD: Unknown argument: " + quoted;
            return result;
        }
        const auto argument = std::string_view{quoted}.substr(1, quoted.size() - 2);
        if (argument == "suffix=" || argument == "rws_suffix=") {
            result.error = "AD: Empty suffix: \"" + std::string{argument} + "\"";
            return result;
        }
        if (argument.starts_with("suffix=")) {
            result.fwd_suffix = argument.substr(7);
        }
        else if (argument.starts_with("rws_suffix=")) {
            result.rws_suffix = argument.substr(11);
        }
        else if (argument.starts_with("order=")) {
            const auto value = argument.substr(6);
            auto order = 0;
            for (const auto digit : value) {
                if (digit < '0' || digit > '9' || order > 1000) {
                    order = -1;
                    break;
                }
                order = order * 10 + (digit - '0');
            }
            if (value.empty() || order <= 0) {
                result.error = "AD: Could not parse derivative order: " + std::string{value};
                return result;
            }
            result.order = order;
        }
        else if (argument == "reverse") {
            result.reverse = true;
        }
        else {
            result.error = "AD: Unknown argument: \"" + std::string{argument} + "\"";
            return result;
        }
    }
    if (result.reverse && result.order != 1) {
        result.error = "AD: Alpha limitation: reverse mode with \"order=" +
            std::to_string(result.order) + "\"";
    }
    return result;
}

// The `@autodiff` arguments of each use of the metaclass on `declaration`.
auto autodiff_arguments(const ir_declaration& declaration)
    -> std::vector<std::vector<std::string>> {
    auto result = std::vector<std::vector<std::string>>{};
    for (std::size_t index = 0; index < declaration.metaclasses.size(); ++index) {
        if (declaration.metaclasses[index] != "autodiff") continue;
        result.push_back(index < declaration.metaclass_arguments.size()
                             ? declaration.metaclass_arguments[index]
                             : std::vector<std::string>{});
    }
    return result;
}

// A subexpression of a parsed expression.
struct node final {
    const expression_text* tree = nullptr;
    const expression* value = nullptr;

    [[nodiscard]] auto kind() const -> expression_kind { return value->kind; }
    [[nodiscard]] auto size() const -> std::size_t { return value->children.size(); }
    [[nodiscard]] auto child(std::size_t index) const -> node {
        return {tree, &value->children[index]};
    }
    [[nodiscard]] auto token(std::size_t index) const -> const std::string& {
        return tree->tokens[index].text;
    }
    // The operator before child `index` of a binary chain.
    [[nodiscard]] auto op_before(std::size_t index) const -> const std::string& {
        return token(value->children[index - 1].last);
    }
    [[nodiscard]] auto to_string() const -> std::string {
        auto result = std::string{};
        for (auto index = value->first; index < value->last; ++index) {
            if (index != value->first) result += tree->tokens[index].leading;
            result += tree->tokens[index].text;
        }
        return result;
    }
    [[nodiscard]] auto is_identifier() const -> bool {
        return kind() == expression_kind::name && value->last == value->first + 1;
    }
    [[nodiscard]] auto mentions(const std::string& name) const -> bool {
        for (auto index = value->first; index < value->last; ++index) {
            if (tree->tokens[index].text == name) return true;
        }
        return false;
    }
};

// A postfix expression as upstream models it: a primary and `.name`, `(args)`,
// or other operator terms.
struct postfix_term final {
    std::string op;
    // The member name for `.`; the call for `(`, whose arguments follow its callee.
    node value;
};

struct postfix_form final {
    node primary;
    std::vector<postfix_term> terms;
};

auto flatten(node value) -> postfix_form {
    switch (value.kind()) {
    case expression_kind::call: {
        auto result = flatten(value.child(0));
        result.terms.push_back({value.token(value.value->op), value});
        return result;
    }
    case expression_kind::member: {
        auto result = flatten(value.child(0));
        result.terms.push_back({value.token(value.value->op), value.child(1)});
        return result;
    }
    case expression_kind::subscript:
    case expression_kind::postfix: {
        auto result = flatten(value.child(0));
        result.terms.push_back({value.token(value.value->op), value});
        return result;
    }
    default: return {value, {}};
    }
}

auto is_postfix(node value) -> bool {
    return value.kind() == expression_kind::call || value.kind() == expression_kind::member ||
        value.kind() == expression_kind::subscript || value.kind() == expression_kind::postfix;
}

struct special_function final {
    std::string name;
    std::size_t arguments = 0;
    bool is_member = false;
    std::string primal, fwd, rws;
    std::string primal_higher_order, fwd_higher_order;
};

// Placeholders: `_o_`/`_od_` name the object and its derivative for member
// functions, `_aN_`/`_adN_`/`_abN_` the Nth argument and its derivatives, and
// `_rb_` the result's reverse derivative.
auto special_functions() -> const std::vector<special_function>& {
    static const auto functions = std::vector<special_function>{
        {"sin", 1, false, "sin(_a1_)", "cos(_a1_) * _ad1_", "_ab1_ += cos(_a1_) * _rb_;\n",
         "sin(_a1_)", "_ad1_.sin(_a1_)"},
        {"cos", 1, false, "cos(_a1_)", "-sin(_a1_) * _ad1_", "_ab1_ += -sin(_a1_) * _rb_;\n",
         "cos(_a1_)", "_ad1_.cos(_a1_)"},
        {"exp", 1, false, "exp(_a1_)", "exp(_a1_) * _ad1_", "_ab1_ += exp(_a1_) * _rb_;\n",
         "exp(_a1_)", "_ad1_.exp(_a1_)"},
        {"sqrt", 1, false, "sqrt(_a1_)", "0.5 * _ad1_ / sqrt(_a1_)",
         "_ab1_ += 0.5 * _rb_ / sqrt(_a1_);\n", "sqrt(_a1_)", "_ad1_.sqrt(_a1_)"},
        {"push_back", 1, true, "_o_.push_back(_a1_);", "_od_.push_back(_ad1_);", "TODO",
         "_o_.push_back(_a1_);", "_od_.push_back(_ad1_);"},
    };
    return functions;
}

struct declared_variable final {
    std::string name;
    std::string decl;
    bool is_active = false;
    bool is_member = false;
};

struct stack_item final {
    ir_declaration* decl = nullptr;
    std::vector<ir_declaration*> diff_request;
    std::vector<ir_declaration*> diff_done;
    std::vector<std::vector<declared_variable>> variables =
        std::vector<std::vector<declared_variable>>(1);
};

// A generated declaration added to `target`, before `before` when it is set.
struct addition final {
    symbol_id target = invalid_symbol;
    symbol_id before = invalid_symbol;
    std::string text;
    source_span span;
};

class declaration_handler;

class context final {
public:
    context(const autodiff_options& value, const std::unordered_map<const ir_declaration*,
            ir_declaration*>& parents, std::vector<diagnostic>& diagnostics)
        : fwd_suffix{value.fwd_suffix}, rws_suffix{value.rws_suffix}, order_{value.order},
          reverse_{value.reverse}, parents_{&parents}, diagnostics_{&diagnostics} {
        if (order_ != 1) {
            if (reverse_) {
                fwd_ad_type = "cpp2::taylor<double, " + std::to_string(order_ - 1) + ">";
                rws_ad_type = "cpp2::taylor<double, " + std::to_string(order_) + ">";
            }
            else {
                fwd_ad_type = "cpp2::taylor<double, " + std::to_string(order_) + ">";
            }
        }
    }

    std::string fwd_suffix;
    std::string rws_suffix;
    std::string fwd_ad_type = "double";
    std::string rws_ad_type = "double";
    std::vector<stack_item> stack;
    std::vector<addition> additions;
    // The declaration that errors are reported on.
    source_span span;
    // The function being differentiated returns an unnamed value, named `r` in the derivatives.
    bool unnamed_return = false;

    [[nodiscard]] auto is_forward() const -> bool { return !reverse_; }
    [[nodiscard]] auto is_reverse() const -> bool { return reverse_; }
    [[nodiscard]] auto is_taylor() const -> bool { return order_ != 1; }

    auto error(std::string message) -> void {
        const auto duplicate = std::any_of(diagnostics_->begin(), diagnostics_->end(),
            [&](const diagnostic& value) { return value.message == message; });
        if (!duplicate) {
            diagnostics_->push_back({"autodiff", diagnostic_severity::error, span,
                                     std::move(message)});
        }
    }

    // Parses `text`; the node lives as long as the context.
    auto parse(std::string text) -> std::optional<node> {
        return root(parsed_.emplace_back(std::move(text)));
    }

    // The expression a statement or initializer consists of.
    auto root(const parsed_expression& text) -> std::optional<node> {
        const auto& tree = text.tree();
        if (tree.root.kind != expression_kind::sequence) return node{&tree, &tree.root};
        const auto tokens = tree.tokens.size();
        if (tree.root.children.size() == 1) {
            const auto& only = tree.root.children.front();
            if (only.first == 0 && (only.last == tokens ||
                                    (only.last + 1 == tokens && tree.tokens.back().text == ";"))) {
                return node{&tree, &only};
            }
        }
        error("AD: Could not parse expression: " + text.source());
        return std::nullopt;
    }

    // A name that no name of the function, nor a derivative of one, starts with.
    auto gen_temporary() -> std::string {
        auto name = std::string{};
        do {
            name = "temp_" + std::to_string(++temporary_count_);
        } while (std::any_of(function_names_.begin(), function_names_.end(),
                             [&](const std::string& used) { return used.starts_with(name); }));
        return name;
    }

    auto add_variable_declaration(std::string name, std::string type, bool is_active,
                                  bool is_member = false) -> void {
        if (unnamed_return && name == "r") {
            error("AD: A function with an unnamed return value cannot declare `r`, the name of "
                  "its differentiated return value.");
        }
        stack.back().variables.back().push_back(
            {std::move(name), std::move(type), is_active, is_member});
    }

    auto is_variable_active(const std::string& name) -> bool {
        return lookup_variable_declaration(name).is_active;
    }

    auto lookup_variable_declaration(const std::string& name) -> declared_variable {
        if (name == "_") return {name, "_", false, false};
        for (auto item = stack.rbegin(); item != stack.rend(); ++item) {
            for (auto frame = item->variables.rbegin(); frame != item->variables.rend(); ++frame) {
                for (const auto& variable : *frame) {
                    if (variable.name == name) return variable;
                }
            }
        }
        error("AD: Could not find declaration of variable with name `" + name + "`.");
        return {};
    }

    auto lookup_declaration(const std::string& name) -> std::vector<ir_declaration*> {
        auto result = std::vector<ir_declaration*>{};
        for (auto item = stack.rbegin(); item != stack.rend(); ++item) {
            for (auto& member : item->decl->members) {
                if (member.name == name) result.push_back(&member);
            }
            if (!result.empty()) break;
        }
        return result;
    }

    auto lookup_function_declaration(const std::string& name) -> std::vector<ir_declaration*> {
        auto result = lookup_declaration(name);
        std::erase_if(result, [](const ir_declaration* value) {
            return value->kind != declaration_kind::function;
        });
        return result;
    }

    auto lookup_type_declaration(const std::string& name) -> std::vector<ir_declaration*> {
        auto result = lookup_declaration(name);
        std::erase_if(result, [](const ir_declaration* value) {
            return value->kind != declaration_kind::type;
        });
        return result;
    }

    static auto lookup_member_function_declaration(ir_declaration& type, const std::string& name)
        -> std::vector<ir_declaration*> {
        auto result = std::vector<ir_declaration*>{};
        for (auto& member : type.members) {
            if (member.kind == declaration_kind::function && member.name == name) {
                result.push_back(&member);
            }
        }
        return result;
    }

    auto is_type_active(const std::string& type) -> bool {
        auto result = false;
        const auto declarations = lookup_type_declaration(type);
        if (!declarations.empty()) {
            for (const auto& member : declarations.front()->members) {
                if (member.kind == declaration_kind::object && member.alias.empty() &&
                    member.name != "this") {
                    result |= is_object_active(type_or_wildcard(member.type), member.body.source());
                }
            }
        }
        // Without a declaration that decides, assume `double` is the active type.
        return result || type.find("double") != std::string::npos;
    }

    auto is_object_active(const std::string& type, const std::string& initializer) -> bool {
        if (type != "_") return is_type_active(type);
        if (initializer.empty()) return true;
        const auto value = parse(initializer);
        return value && is_active(*value);
    }

    // Upstream's activity check of an expression.
    auto is_active(node value) -> bool {
        switch (value.kind()) {
        case expression_kind::name: return is_variable_active(value.to_string());
        case expression_kind::literal: return value.to_string().find('.') != std::string::npos;
        case expression_kind::typed_value:
        case expression_kind::sequence:
            error("AD: Unknown primary expression kind: " + value.to_string());
            return false;
        default: break;
        }
        if (is_postfix(value)) {
            const auto form = flatten(value);
            auto is_func = false;
            for (std::size_t index = 0; index < form.terms.size(); ++index) {
                if (form.terms[index].op == ".") continue;
                if (form.terms[index].op == "(" && index + 1 == form.terms.size()) {
                    is_func = true;
                    continue;
                }
                error("AD: Unknown operator for postfix expression. op: " + form.terms[index].op +
                      " expr: " + value.to_string());
            }
            auto result = false;
            if (!is_func || form.terms.size() != 1) {
                result |= is_variable_active(form.primary.to_string());
            }
            if (is_func) {
                const auto call = form.terms.back().value;
                for (std::size_t index = 1; index < call.size(); ++index) {
                    result |= is_active(call.child(index));
                }
            }
            return result;
        }
        auto result = false;
        for (std::size_t index = 0; index < value.size(); ++index) {
            result |= is_active(value.child(index));
        }
        return result;
    }

    auto get_fwd_ad_type(const std::string& type) -> std::string {
        return ad_type(type, fwd_suffix, fwd_ad_type);
    }

    auto get_rws_ad_type(const std::string& type) -> std::string {
        return ad_type(type, rws_suffix, rws_ad_type);
    }

    auto lookup_special_function(const std::string& name, std::size_t arguments, bool is_member)
        -> const special_function* {
        for (const auto& function : special_functions()) {
            if (function.name == name && function.arguments == arguments &&
                function.is_member == is_member) {
                return &function;
            }
        }
        return nullptr;
    }

    auto parent_of(const ir_declaration& value) const -> ir_declaration* {
        const auto found = parents_->find(&value);
        return found == parents_->end() ? nullptr : found->second;
    }

    auto add_as_differentiated(ir_declaration& value) -> void {
        stack.back().diff_done.push_back(&value);
    }

    auto add_for_differentiation(ir_declaration& value) -> void {
        const auto* parent = parent_of(value);
        // An @autodiff declaration with these options, or a member of one, has its derivative.
        if (has_same_autodiff(value) || (parent != nullptr && has_same_autodiff(*parent))) return;
        for (auto item = stack.rbegin(); item != stack.rend(); ++item) {
            if (parent != nullptr && item->decl == parent) {
                if (std::find(item->diff_request.begin(), item->diff_request.end(), &value) ==
                    item->diff_request.end()) {
                    item->diff_request.push_back(&value);
                }
                return;
            }
        }
        error("AD: Could not find parent type/namespace for: " + value.name);
    }

    auto has_same_autodiff(const ir_declaration& value) const -> bool {
        const auto uses = autodiff_arguments(value);
        return std::any_of(uses.begin(), uses.end(), [&](const auto& arguments) {
            const auto other = parse_autodiff_options(arguments);
            return other.error.empty() && other.fwd_suffix == fwd_suffix &&
                other.rws_suffix == rws_suffix && other.order == order_ &&
                other.reverse == reverse_;
        });
    }

    auto add_member(const ir_declaration& target, std::string text,
                    const ir_declaration* before = nullptr) -> void {
        additions.push_back({target.symbol, before ? before->symbol : invalid_symbol,
                             std::move(text), span});
    }

    auto enter_function(const ir_declaration& function) -> void {
        temporary_count_ = 0;
        function_names_.clear();
        for (const auto& parameter : function.parameters) function_names_.push_back(parameter.name);
        for (const auto& named : function.named_returns) function_names_.push_back(named.value.name);
        if (function.function_statements) {
            ir_detail::visit_statements(function.function_statements->statements,
                                        [&](const statement& value) {
                const auto* form = std::get_if<statement::declaration_form>(&value.form);
                if (form && form->declaration) function_names_.push_back(form->declaration->name);
            });
        }
        restored_.clear();
        assigned_.clear();
        unnamed_return = false;
        stack.back().variables.emplace_back();
    }

    // A variable whose value the backprop pass restores to an earlier one.
    auto add_restored(const std::string& name) -> void { restored_.push_back(name); }

    [[nodiscard]] auto is_restored(const std::string& name) const -> bool {
        return std::find(restored_.begin(), restored_.end(), name) != restored_.end();
    }

    // A variable that holds a value: a parameter, an initialized local, or one assigned since.
    auto add_assigned(const std::string& name) -> void { assigned_.push_back(name); }

    [[nodiscard]] auto is_assigned(const std::string& name) const -> bool {
        return std::find(assigned_.begin(), assigned_.end(), name) != assigned_.end();
    }

    auto leave_function() -> void { stack.back().variables.pop_back(); }

    auto push_stack(ir_declaration& value) -> void { stack.push_back({&value}); }

    auto pop_stack() -> void;

    auto finish() -> void {
        while (!stack.empty()) pop_stack();
    }

private:
    int order_ = 1;
    bool reverse_ = false;
    int temporary_count_ = 0;
    std::vector<std::string> function_names_;
    std::vector<std::string> restored_;
    std::vector<std::string> assigned_;
    const std::unordered_map<const ir_declaration*, ir_declaration*>* parents_;
    std::vector<diagnostic>* diagnostics_;
    std::list<parsed_expression> parsed_;

    auto ad_type(const std::string& type, const std::string& suffix, const std::string& ad)
        -> std::string {
        auto result = type;
        if (type != "double") {
            const auto declarations = lookup_type_declaration(type);
            if (!declarations.empty()) {
                // A Cpp2 type: differentiate it too.
                add_for_differentiation(*declarations.front());
                result += suffix;
            }
        }
        return replace_all(result, "double", ad);
    }
};

// Forward code, and reverse code split into the primal pass and the backward
// pass, which is built in reverse.
struct diff_code final {
    const context* ctx = nullptr;
    std::string fwd;
    std::string rws_primal;
    std::string rws_backprop;

    auto add_forward(const std::string& value) -> void {
        if (ctx->is_forward()) fwd += value;
    }
    auto add_reverse_primal(const std::string& value) -> void {
        if (ctx->is_reverse()) rws_primal += value;
    }
    auto add_reverse_backprop(const std::string& value) -> void {
        if (ctx->is_reverse()) rws_backprop = value + rws_backprop;
    }
    // As upstream, plain text goes to the forward code only.
    auto operator+=(const std::string& value) -> diff_code& {
        fwd += value;
        return *this;
    }
    auto append(const diff_code& other) -> void {
        fwd += other.fwd;
        rws_primal += other.rws_primal;
        rws_backprop = other.rws_backprop + rws_backprop;
    }
};

struct names final {
    std::string primal;
    std::string fwd;
    std::string rws;
    bool active = false;
};

class expression_handler final {
public:
    explicit expression_handler(context& value) : ctx{value}, diff{&value} {}

    context& ctx;
    diff_code diff;
    std::string primal_expr;
    std::string fwd_expr;
    // The backprop statements, in which `_rb_` is the result's adjoint.
    std::string rws_expr;
    // The adjoint of a name, which an assignment to it updates.
    std::string rws_name;

    auto prepare_backprop(const std::string& rhs_b, const std::string& lhs,
                          const std::string& lhs_d, const std::string& lhs_b) const -> std::string {
        auto result = replace_all(rhs_b, "_r_", lhs);
        result = replace_all(std::move(result), "_rd_", lhs_d);
        return replace_all(std::move(result), "_rb_", lhs_b);
    }

    auto prepare_backprop(const std::string& rhs_b, const std::string& lhs) const -> std::string {
        return prepare_backprop(rhs_b, lhs, lhs + ctx.fwd_suffix, lhs + ctx.rws_suffix);
    }

    auto gen_assignment(const std::string& lhs, const std::string& lhs_d, const std::string& lhs_b,
                        const std::string& rhs, const std::string& rhs_d,
                        const std::string& rhs_b) -> void {
        diff.add_forward(lhs_d + " = " + rhs_d + ";\n");
        diff.add_forward(lhs + " = " + rhs + ";\n");
        if (ctx.is_taylor()) diff.add_reverse_primal(lhs_d + " = " + rhs_d + ";\n");
        diff.add_reverse_primal(lhs + " = " + rhs + ";\n");
        diff.add_reverse_backprop(lhs_b + " = 0.0;\n");
        diff.add_reverse_backprop(prepare_backprop(rhs_b, lhs, lhs_d, lhs_b));
    }

    auto gen_assignment(const std::string& lhs, const std::string& lhs_d,
                        const std::string& lhs_b) -> void {
        gen_assignment(lhs, lhs_d, lhs_b, primal_expr, fwd_expr, rws_expr);
    }

    auto gen_assignment(const std::string& lhs) -> void {
        gen_assignment(lhs, suffixed(lhs, ctx.fwd_suffix), suffixed(lhs, ctx.rws_suffix));
    }

    auto gen_declaration(const std::string& lhs, const std::string& lhs_d,
                         const std::string& lhs_b, const std::string& rhs,
                         const std::string& rhs_d, const std::string& rhs_b,
                         const std::string& type, const std::string& type_d,
                         const std::string& type_b) -> void {
        diff.add_forward(lhs_d + ": " + type_or_wildcard(type_d) + " = " + rhs_d + ";\n");
        diff.add_forward(lhs + ": " + type_or_wildcard(type) + " = " + rhs + ";\n");
        if (ctx.is_taylor()) {
            diff.add_reverse_primal(lhs_d + ": " + type_or_wildcard(type_d) + " = " + rhs_d + ";\n");
        }
        diff.add_reverse_primal(lhs_b + ": " + type_or_wildcard(type_b) + " = 0.0;\n");
        diff.add_reverse_primal(lhs + ": " + type_or_wildcard(type) + " = " + rhs + ";\n");
        diff.add_reverse_backprop(lhs_b + " = 0.0;\n");
        diff.add_reverse_backprop(prepare_backprop(rhs_b, lhs, lhs_d, lhs_b));
    }

    auto gen_declaration(const std::string& lhs, const std::string& type) -> void {
        gen_declaration(lhs, lhs + ctx.fwd_suffix, lhs + ctx.rws_suffix, primal_expr, fwd_expr,
                        rws_expr, type, ctx.get_fwd_ad_type(type), ctx.get_rws_ad_type(type));
    }

    auto append(const expression_handler& other) -> void { diff.append(other.diff); }

    auto handle_expression_list(node call) -> std::vector<names> {
        auto result = std::vector<names>{};
        for (std::size_t index = 1; index < call.size(); ++index) {
            result.push_back(handle_expression_term(call.child(index)));
        }
        return result;
    }

    auto handle_expression_term(node term) -> names {
        if (term.is_identifier()) {
            const auto primal = term.to_string();
            auto fwd = primal + ctx.fwd_suffix;
            auto rws = primal + ctx.rws_suffix;
            const auto decl = ctx.lookup_variable_declaration(primal);
            if (decl.is_member) {
                fwd = "this" + ctx.fwd_suffix + "." + fwd;
                rws = "this" + ctx.rws_suffix + "." + rws;
            }
            if (decl.is_active) return {primal, fwd, rws, true};
            return {primal, "", "", false};
        }
        if (term.kind() == expression_kind::group) {
            if (term.size() != 1) {
                ctx.error("Can not handle multiple expressions. " + term.to_string());
                return {"error", "", "", false};
            }
            if (term.child(0).kind() == expression_kind::assignment) {
                ctx.error("Can not handle assign expr inside of expression. " +
                          term.child(0).to_string());
                return {"error", "", "", false};
            }
            return handle_expression_term(term.child(0));
        }
        if (ctx.is_active(term)) {
            auto handler = expression_handler{ctx};
            handler.traverse(term);
            const auto temporary = ctx.gen_temporary();
            handler.gen_declaration(temporary, "double");
            append(handler);
            return {temporary, temporary + ctx.fwd_suffix, temporary + ctx.rws_suffix, true};
        }
        return {term.to_string(), "", "", false};
    }

    auto handle_function_call(const postfix_form& form, bool has_return) -> void {
        const auto& terms = form.terms;
        auto is_func = true;
        for (std::size_t index = 0; index < terms.size(); ++index) {
            if (terms[index].op == ".") continue;
            if (terms[index].op == "(" && index + 1 == terms.size()) continue;
            is_func = false;
        }
        const auto text = terms.empty() ? form.primary.to_string()
            : node{form.primary.tree, terms.back().value.value}.to_string();
        if (!is_func || terms.empty() || terms.back().op != "(") {
            ctx.error("AD: Postfix expressions are only handled for function calls, or member "
                      "function calls. Do not know how to handle: " + text);
            return;
        }
        auto object = std::string{};
        auto object_d = std::string{};
        auto object_b = std::string{};
        auto function_name = std::string{};
        auto args = std::vector<names>{};
        if (terms.size() != 1) {
            object = form.primary.to_string();
            object_d = object + ctx.fwd_suffix;
            object_b = object + ctx.rws_suffix;
        }
        else {
            function_name = form.primary.to_string();
        }
        for (std::size_t index = 0; index < terms.size(); ++index) {
            if (terms[index].op == ".") {
                const auto name = terms[index].value.to_string();
                if (index + 2 == terms.size()) {
                    function_name = name;
                }
                else {
                    object += "." + name;
                    object_d += "." + name + ctx.fwd_suffix;
                }
            }
            else {
                args = handle_expression_list(terms[index].value);
            }
        }
        if (handle_special_function(object, object_d, object_b, function_name, args)) return;
        if (object.find('.') != std::string::npos) {
            ctx.error("AD: can not handle nested member function calls: " + text);
            return;
        }

        auto call_primal = std::string{};
        auto call_fwd = std::string{};
        auto call_rws = std::string{};
        auto ret_temp = std::string{};
        if (has_return) {
            ret_temp = ctx.gen_temporary();
            call_fwd += ret_temp + " := ";
        }
        if (!object.empty()) {
            call_primal += object + ".";
            call_fwd += object + ".";
            call_rws += object + ".";
        }
        call_primal += function_name + "(";
        call_fwd += function_name + ctx.fwd_suffix + "(";
        call_rws += function_name + ctx.rws_suffix + "(";
        if (!object.empty()) {
            call_fwd += object_d + ", ";
            call_rws += object_b + ", ";
        }
        else if (calls_member_function(function_name)) {
            // A call of a member function on `this` passes this function's `this` derivative.
            call_fwd += "this" + ctx.fwd_suffix + ", ";
            call_rws += "this" + ctx.rws_suffix + ", ";
        }
        for (const auto& arg : args) {
            call_primal += arg.primal + ", ";
            call_fwd += arg.primal + ", ";
            call_rws += arg.primal + ", ";
            if (arg.active) {
                call_fwd += arg.fwd + ", ";
                call_rws += arg.rws + ", ";
            }
        }

        if (!has_return && ctx.is_reverse()) {
            // The backprop call would need the arguments as they were before the call.
            ctx.error("AD: Alpha limitation: reverse mode for calls without a return value: " +
                      text);
            return;
        }
        if (!has_return) {
            call_primal += ");\n";
            call_fwd += ");\n";
            call_rws += ");\n";
            diff.add_forward(call_fwd);
            diff.add_reverse_primal(call_primal);
            diff.add_reverse_backprop(call_rws);
            return;
        }
        auto functions = std::vector<ir_declaration*>{};
        if (!object.empty()) {
            const auto object_decl = ctx.lookup_variable_declaration(object);
            const auto types = ctx.lookup_type_declaration(object_decl.decl);
            if (types.empty()) {
                ctx.error("AD: Could not find type declaration for `" + object + " with type " +
                          object_decl.decl + "`.\n"
                          "    If cpp2 object: this is an alpha limitation, please declare it "
                          "befor the current declaration.\n"
                          "    If cpp function: please add a special handling for this member "
                          "function.");
                return;
            }
            functions = context::lookup_member_function_declaration(*types.front(), function_name);
            ctx.add_for_differentiation(*types.front());
            if (functions.empty()) {
                ctx.error("AD: Could not find member function declaration for `" +
                          function_name + "`.");
                return;
            }
        }
        else {
            functions = ctx.lookup_function_declaration(function_name);
            if (functions.empty()) {
                ctx.error("AD: Could not find function declaration for `" + function_name + "`.\n"
                          "    If cpp2 function: this is an alpha limitation, please declare it "
                          "befor the current declaration.\n"
                          "    If cpp function: please add a special handling for this "
                          "function.");
                return;
            }
            if (functions.size() != 1) {
                ctx.error("AD: No handling for overload resultion is currently implemented.");
                return;
            }
            ctx.add_for_differentiation(*functions.front());
        }
        auto ret_name = std::string{"r"};
        const auto& returns = functions.front()->named_returns;
        if (returns.size() > 1) ctx.error("AD: Expecting single return.");
        if (!returns.empty()) ret_name = returns.back().value.name;

        call_rws += "_rb_, ";
        call_primal += ")";
        call_fwd += ");\n";
        call_rws += ");\n";
        diff.add_forward(call_fwd);
        primal_expr = call_primal;
        fwd_expr = ret_temp + "." + ret_name + ctx.fwd_suffix;
        rws_expr = "_ = " + call_rws;
    }

    auto calls_member_function(const std::string& function_name) -> bool {
        const auto functions = ctx.lookup_function_declaration(function_name);
        return std::any_of(functions.begin(), functions.end(), [](const ir_declaration* f) {
            return !f->parameters.empty() && f->parameters.front().name == "this";
        });
    }

    auto handle_special_function(const std::string& object, const std::string& object_d,
                                 const std::string& object_b, const std::string& function_name,
                                 const std::vector<names>& args) -> bool {
        const auto* function = ctx.lookup_special_function(function_name, args.size(),
                                                           !object.empty());
        if (function == nullptr) return false;
        auto code_primal = ctx.is_taylor() ? function->primal_higher_order : function->primal;
        auto code_fwd = ctx.is_taylor() ? function->fwd_higher_order : function->fwd;
        auto code_rws = function->rws;
        // `TODO` marks a derivative the table does not have yet.
        if (ctx.is_reverse() && code_rws == "TODO") {
            ctx.error("AD: Alpha limitation: no derivative of `" + function_name +
                      "` in this mode.");
            return true;
        }
        if (!object.empty()) {
            code_primal = replace_all(replace_all(code_primal, "_od_", object_d), "_o_", object);
            code_fwd = replace_all(replace_all(code_fwd, "_od_", object_d), "_o_", object);
            code_rws = replace_all(replace_all(replace_all(code_rws, "_od_", object_d), "_ob_",
                                               object_b), "_o_", object);
        }
        for (std::size_t index = 0; index < args.size(); ++index) {
            const auto number = std::to_string(index + 1);
            const auto& arg = args[index];
            code_primal = replace_all(code_primal, "_a" + number + "_", arg.primal);
            code_primal = replace_all(code_primal, "_ad" + number + "_", arg.fwd);
            code_fwd = replace_all(code_fwd, "_a" + number + "_", arg.primal);
            code_fwd = replace_all(code_fwd, "_ad" + number + "_", arg.fwd);
            code_rws = replace_all(code_rws, "_a" + number + "_", arg.primal);
            code_rws = replace_all(code_rws, "_ad" + number + "_", arg.fwd);
            code_rws = replace_all(code_rws, "_ab" + number + "_", arg.rws);
        }
        primal_expr = std::move(code_primal);
        fwd_expr = std::move(code_fwd);
        rws_expr = std::move(code_rws);
        return true;
    }

    auto traverse(node value) -> void {
        switch (value.kind()) {
        case expression_kind::assignment:
            ctx.error("AD: Assign expressions are not yet handled.");
            return;
        case expression_kind::binary: {
            const auto& op = value.op_before(1);
            if (op == "+" || op == "-") return additive(value);
            if (op == "*" || op == "/" || op == "%") return multiplicative(value);
            ctx.error("AD: " + binary_kind(op) + " expressions are not yet handled.");
            return;
        }
        case expression_kind::is_as:
            ctx.error("AD: Is as expressions are not yet handled.");
            return;
        case expression_kind::prefix: return prefix(value);
        case expression_kind::name:
        case expression_kind::group:
        case expression_kind::literal: return primary(value);
        default: break;
        }
        if (is_postfix(value)) return postfix(value);
        ctx.error("AD: Unknown primary expression kind: " + value.to_string());
    }

    static auto binary_kind(const std::string& op) -> std::string {
        if (op == "||") return "Logical or";
        if (op == "&&") return "Logical and";
        if (op == "|") return "Bit or";
        if (op == "^") return "Bit xor";
        if (op == "&") return "Bit and";
        if (op == "==" || op == "!=") return "Equality or";
        if (op == "<=>") return "Compare or";
        if (op == "<<" || op == ">>") return "Shift or";
        return "Relational";
    }

private:
    static auto suffixed(const std::string& name, const std::string& suffix) -> std::string {
        return name == "_" ? name : name + suffix;
    }

    auto additive(node value) -> void {
        auto first_fwd = true;
        auto op = std::string{"+"};
        auto fwd = std::string{};
        auto rws = std::string{};
        auto primal = std::string{};
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (index != 0) {
                op = value.op_before(index);
                primal += " " + op + " ";
            }
            const auto var = handle_expression_term(value.child(index));
            if (var.active) {
                if (first_fwd) fwd += op == "-" ? "-" + var.fwd : var.fwd;
                else fwd += op + " " + var.fwd;
                rws += var.rws + " " + op + "= _rb_;\n";
                first_fwd = false;
            }
            primal += var.primal;
        }
        primal_expr = std::move(primal);
        fwd_expr = std::move(fwd);
        rws_expr = std::move(rws);
    }

    auto multiplicative(node value) -> void {
        auto var_a = handle_expression_term(value.child(0));
        for (std::size_t index = 1; index < value.size(); ++index) {
            const auto var_b = handle_expression_term(value.child(index));
            const auto& op = value.op_before(index);
            auto fwd = std::string{};
            auto rws = std::string{};
            auto primal = std::string{};
            if (op == "*") {
                if (ctx.is_taylor()) {
                    fwd = var_a.fwd + "..mul(" + var_b.fwd + ", " + var_a.primal + ", " +
                        var_b.primal + ")";
                    if (var_a.active) {
                        rws += var_a.rws + " += " + var_b.fwd + "..mul(_rb_, " + var_b.primal +
                            ", _r_);\n";
                    }
                    if (var_b.active) {
                        rws += var_b.rws + " += " + var_a.fwd + "..mul(_rb_, " + var_a.primal +
                            ", _r_);\n";
                    }
                }
                else {
                    if (var_a.active) {
                        fwd += var_b.primal + " * " + var_a.fwd;
                        rws += var_a.rws + " += " + var_b.primal + " * _rb_;\n";
                    }
                    if (var_b.active) {
                        if (!fwd.empty()) fwd += " + ";
                        fwd += var_a.primal + " * " + var_b.fwd;
                        rws += var_b.rws + " += " + var_a.primal + " * _rb_;\n";
                    }
                }
                primal = var_a.primal + " * " + var_b.primal;
            }
            else if (op == "/") {
                if (ctx.is_taylor()) {
                    fwd = var_a.fwd + ".div(" + var_b.fwd + ", " + var_a.primal + ", " +
                        var_b.primal + ")";
                    if (var_a.active) {
                        rws += var_a.rws + " += _rb_.div(" + var_b.fwd + ", _r_, " +
                            var_b.primal + ");\n";
                    }
                    if (var_b.active) {
                        rws += var_b.rws + " -= " + var_a.fwd + ".mul(_rb_, " + var_a.primal +
                            ", _r_).div(" + var_b.fwd + ".mul(" + var_b.fwd + ", " +
                            var_b.primal + ", " + var_b.primal + "), " + var_a.primal +
                            " * _r_, " + var_b.primal + " * " + var_b.primal + ");\n";
                    }
                }
                else {
                    if (var_a.active) {
                        fwd += var_a.fwd + " / " + var_b.primal;
                        rws = var_a.rws + " += _rb_ / " + var_b.primal + ";\n";
                    }
                    if (var_b.active) {
                        if (!fwd.empty()) fwd += " + ";
                        fwd += "-" + var_a.primal + " * " + var_b.fwd + " / (" + var_b.primal +
                            " * " + var_b.primal + ")";
                        rws += var_b.rws + " -= " + var_a.primal + " * _rb_ / (" +
                            var_b.primal + " * " + var_b.primal + ");\n";
                    }
                }
                primal = var_a.primal + " / " + var_b.primal;
            }
            else {
                ctx.error("unkown multiplicative operator '" + op + "'");
            }

            if (index + 1 == value.size()) {
                primal_expr = std::move(primal);
                fwd_expr = std::move(fwd);
                rws_expr = std::move(rws);
            }
            else {
                var_a.primal = ctx.gen_temporary();
                var_a.fwd = var_a.primal + ctx.fwd_suffix;
                var_a.rws = var_a.primal + ctx.rws_suffix;
                var_a.active = var_a.active || var_b.active;
                gen_declaration(var_a.primal, var_a.fwd, var_a.rws, primal, fwd, rws, "", "", "");
            }
        }
    }

    auto prefix(node value) -> void {
        const auto ops = value.child(0).value->first - value.value->first;
        if (ops != 1) {
            ctx.error("AD: Can only handle one prefix operation. Expression is: " +
                      value.to_string());
        }
        if (ctx.is_reverse()) {
            ctx.error("AD: Alpha limitiation now reverse mode for prefix operators.");
        }
        auto handler = expression_handler{ctx};
        handler.traverse(value.child(0));
        append(handler);
        const auto& op = value.token(value.value->first);
        primal_expr = op + handler.primal_expr;
        fwd_expr = op + handler.fwd_expr;
    }

    auto postfix(node value) -> void {
        const auto form = flatten(value);
        auto is_func = false;
        for (std::size_t index = 0; index < form.terms.size(); ++index) {
            if (form.terms[index].op == ".") continue;
            if (form.terms[index].op == "(" && index + 1 == form.terms.size()) {
                is_func = true;
                continue;
            }
            ctx.error("AD: Unknown operator for postfix expression. op: " + form.terms[index].op +
                      " expr: " + value.to_string());
        }
        if (is_func) {
            handle_function_call(form, true);
            return;
        }
        if (ctx.is_reverse()) {
            ctx.error("AD: Alpha limitiation now reverse mode for member access: " +
                      value.to_string());
        }
        auto access = form.primary.to_string();
        auto access_d = access + ctx.fwd_suffix;
        for (const auto& term : form.terms) {
            const auto name = term.value.to_string();
            access += term.op + name;
            access_d += term.op + name + ctx.fwd_suffix;
        }
        primal_expr = std::move(access);
        fwd_expr = std::move(access_d);
    }

    auto primary(node value) -> void {
        if (value.kind() == expression_kind::name) {
            primal_expr = value.to_string();
            fwd_expr = suffixed(primal_expr, ctx.fwd_suffix);
            rws_name = suffixed(primal_expr, ctx.rws_suffix);
            const auto decl = ctx.lookup_variable_declaration(primal_expr);
            if (decl.is_member) {
                fwd_expr = "this" + ctx.fwd_suffix + "." + fwd_expr;
                rws_name = "this" + ctx.rws_suffix + "." + rws_name;
            }
            if (decl.is_active) rws_expr = rws_name + " += _rb_;\n";
            // A passive variable has a zero derivative.
            else if (primal_expr != "_") fwd_expr = "()";
        }
        else if (value.kind() == expression_kind::group) {
            if (value.size() == 0) {
                primal_expr = fwd_expr = "()";
            }
            else {
                ctx.error("AD: Do not know how to handle non empty expression list inside of "
                          "primary_expression: " + value.to_string());
            }
        }
        else {
            // A literal has no adjoint, so it has no backprop statement.
            primal_expr = value.to_string();
            fwd_expr = "()";
        }
    }
};

class statement_handler final {
public:
    statement_handler(context& value, const ir_declaration& function)
        : ctx{value}, diff{&value}, function_{&function} {}

    context& ctx;
    diff_code diff;
    std::vector<std::string> overwritten;
    bool overwrite_push_pop = false;
    // The last statement of the function body, where reverse mode allows a return.
    const statement* last = nullptr;
    // Generates only the primal pass, recording nothing, for a block whose
    // backprop pass reruns it.
    bool plain = false;

    auto traverse(const statement& value) -> void {
        switch (value.kind) {
        case statement_kind::declaration_statement: {
            const auto& form = value.as<statement::declaration_form>();
            if (!form.declaration) return expression_statement(form.text);
            const auto& declaration = *form.declaration;
            if (declaration.is_named_return) return;
            if (declaration.kind != declaration_kind::object || declaration.is_alias) {
                ctx.error("AD: Do not know how to handle declaration: " + form.text.source());
                return;
            }
            return object(declaration);
        }
        case statement_kind::destructuring_statement:
            ctx.error("AD: destructuring declarations are not supported");
            return;
        case statement_kind::expression:
            return expression_statement(value.as<statement::expression_form>().text);
        case statement_kind::discard_statement: {
            // `_ = value`, which upstream sees as an assignment.
            if (const auto parsed = ctx.parse(
                    "_ = " + value.as<statement::expression_form>().text.source())) {
                expression(*parsed);
            }
            return;
        }
        case statement_kind::return_statement: {
            // The backprop pass follows the primal pass, so a reverse derivative
            // cannot return before the end.
            if (ctx.is_reverse() && &value != last) {
                ctx.error("AD: Alpha limitiation now reverse mode for a return before the end "
                          "of the function.");
            }
            const auto& text = value.as<statement::expression_form>().text;
            if (!text.empty()) {
                if (const auto parsed = ctx.root(text)) {
                    auto handler = expression_handler{ctx};
                    handler.traverse(*parsed);
                    handler.gen_assignment("r");
                    diff.append(handler.diff);
                }
            }
            if (&value != last) diff.add_forward("return;\n");
            return;
        }
        case statement_kind::block: {
            const auto& form = value.as<statement::block_form>();
            if (form.parameters.empty()) return compound(form.body);
            if (form.body.size() == 1 && is_loop(*form.body.front())) {
                return loop(*form.body.front(), form.parameters);
            }
            ctx.error("AD: Statement parameters are only handled on loops.");
            return;
        }
        case statement_kind::if_statement: return if_statement(value.as<statement::if_form>());
        case statement_kind::while_statement:
        case statement_kind::do_statement:
        case statement_kind::for_statement: return loop(value, {});
        case statement_kind::break_statement:
        case statement_kind::continue_statement: {
            if (ctx.is_reverse()) {
                ctx.error("AD: Alpha limitiation now reverse mode for break or continue.");
            }
            const auto& label = value.as<statement::jump_form>().label;
            diff += std::string{value.kind == statement_kind::break_statement ? "break" : "continue"} +
                (label.empty() ? "" : " " + label) + ";\n";
            return;
        }
        case statement_kind::try_statement:
        case statement_kind::using_declaration:
        case statement_kind::contract_statement:
        case statement_kind::defer_statement:
        case statement_kind::unsafe_block:
            ctx.error("AD: Do not know how to handle this kind of statement.");
            return;
        }
    }

    // A block's primal pass saves the variables the block overwrites; its
    // backprop pass restores them, reruns the block to record what the block's
    // backprop needs, and runs that backprop.
    auto compound(const statement_list& statements) -> void {
        auto handler = statement_handler{ctx, *function_};
        handler.plain = true;
        auto rerun = statement_handler{ctx, *function_};
        rerun.overwrite_push_pop = true;
        // The rerun is traversed even when unused, which numbers temporaries as upstream.
        for (const auto& value : statements) {
            handler.traverse(*value);
            rerun.traverse(*value);
        }
        add_overwritten(handler.overwritten);
        diff.add_forward("{\n" + handler.diff.fwd + "}\n");
        if (plain) {
            diff.add_reverse_primal("{\n" + handler.diff.rws_primal + "}\n");
            return;
        }
        auto pushes = std::string{};
        auto pops = std::string{};
        for (const auto& name : handler.overwritten) {
            const auto variable = ctx.lookup_variable_declaration(name);
            ctx.add_restored(name);
            pushes += "cpp2::ad_stack::push<" + variable.decl + ">(" + name + ");";
            pops += name + " = cpp2::ad_stack::pop<" + variable.decl + ">();";
        }
        diff.add_reverse_primal("{\n" + pushes + handler.diff.rws_primal + "}\n");
        diff.add_reverse_backprop("{\n" + pops + rerun.diff.rws_primal + rerun.diff.rws_backprop +
                                  "}\n");
    }

private:
    const ir_declaration* function_;

    static auto is_loop(const statement& value) -> bool {
        return value.kind == statement_kind::while_statement ||
            value.kind == statement_kind::do_statement ||
            value.kind == statement_kind::for_statement;
    }

    // As upstream, every if has an else branch, and an `else if` is a compound
    // holding the nested if. In reverse mode the primal pass records the branch
    // taken, and the backprop pass pops it to run that branch's backprop.
    auto if_statement(const statement::if_form& form) -> void {
        auto then_branch = statement_handler{ctx, *function_};
        then_branch.plain = plain;
        then_branch.compound(form.body);
        auto else_branch = statement_handler{ctx, *function_};
        else_branch.plain = plain;
        else_branch.compound(form.else_body);
        add_overwritten(then_branch.overwritten);
        add_overwritten(else_branch.overwritten);
        const auto& then_diff = then_branch.diff;
        const auto& else_diff = else_branch.diff;
        const auto condition = form.condition.source();
        if (form.is_constexpr) {
            const auto head = "if constexpr " + condition;
            diff.add_forward(head + then_diff.fwd + "else " + else_diff.fwd);
            diff.add_reverse_primal(head + then_diff.rws_primal + "else " + else_diff.rws_primal);
            diff.add_reverse_backprop(head + then_diff.rws_backprop + "else " +
                                      else_diff.rws_backprop);
            return;
        }
        diff.add_forward("if " + condition + then_diff.fwd + "else " + else_diff.fwd);
        if (plain) {
            diff.add_reverse_primal("if " + condition + then_diff.rws_primal + "else " +
                                    else_diff.rws_primal);
            return;
        }
        if (!ctx.is_reverse()) return;
        // The branch is pushed after it runs, so it is popped before the branch's own pops.
        const auto taken = ctx.gen_temporary();
        diff.add_reverse_primal(taken + ": bool = " + condition + ";\nif " + taken + " " +
                                then_diff.rws_primal + "else " + else_diff.rws_primal +
                                "cpp2::ad_stack::push<bool>(" + taken + ");\n");
        diff.add_reverse_backprop("if cpp2::ad_stack::pop<bool>() " + then_diff.rws_backprop +
                                  "else " + else_diff.rws_backprop);
    }

    auto add_overwritten(const std::vector<std::string>& names) -> void {
        for (const auto& name : names) {
            if (std::find(overwritten.begin(), overwritten.end(), name) == overwritten.end()) {
                overwritten.push_back(name);
            }
        }
    }

    auto handle_stmt_parameters(const std::vector<initialized_parameter>& parameters) -> diff_code {
        auto result = diff_code{&ctx};
        for (const auto& parameter : parameters) {
            const auto& name = parameter.value.name;
            const auto type = type_or_wildcard(parameter.value.type);
            const auto style = passing_name(parameter.value.passing);
            const auto active = ctx.is_object_active(type, parameter.initializer.source());
            auto init = std::string{};
            auto init_d = std::string{};
            if (!parameter.initializer.empty()) {
                if (const auto parsed = ctx.root(parameter.initializer)) {
                    auto handler = expression_handler{ctx};
                    handler.traverse(*parsed);
                    init = " = " + handler.primal_expr;
                    if (active) init_d = " = " + handler.fwd_expr;
                }
            }
            result.add_forward(style + " " + name + " : " + type + init + ", ");
            result.add_reverse_primal(style + " " + name + " : " + type + init + ", ");
            if (active) {
                result.add_forward(style + " " + name + ctx.fwd_suffix + " : " +
                                   ctx.get_fwd_ad_type(type) + init_d + ", ");
            }
            ctx.add_variable_declaration(name, type, active);
            if (!parameter.initializer.empty()) ctx.add_assigned(name);
        }
        return result;
    }

    auto object(const statement::declaration_data& declaration) -> void {
        const auto& lhs = declaration.name;
        const auto type = type_or_wildcard(declaration.type);
        const auto has_initializer = !declaration.initializer.empty();
        if (type == "_" && !has_initializer) {
            ctx.error("AD: A deduced declaration requires an initializer: " + lhs);
            return;
        }
        const auto active = ctx.is_object_active(type, declaration.initializer.source());
        if (active) {
            const auto fwd_ad_type = ctx.get_fwd_ad_type(type);
            // A deduced active variable has the active scalar type, which its
            // adjoint and saved values are declared with.
            const auto rws_ad_type = ctx.get_rws_ad_type(type == "_" ? "double" : type);
            auto prim_init = std::string{};
            auto fwd_init = std::string{};
            if (has_initializer) {
                if (const auto parsed = ctx.root(declaration.initializer)) {
                    auto handler = expression_handler{ctx};
                    handler.traverse(*parsed);
                    diff.append(handler.diff);
                    prim_init = " = " + handler.primal_expr;
                    fwd_init = " = " + handler.fwd_expr;
                    // The reset keeps a call's `inout` adjoint argument from being a last use.
                    diff.add_reverse_backprop(lhs + ctx.rws_suffix + " = 0.0;\n");
                    diff.add_reverse_backprop(handler.prepare_backprop(handler.rws_expr, lhs));
                    if (type == "_" && handler.fwd_expr == "()") {
                        // A deduced type initialized from a literal.
                        fwd_init = " = " + ctx.get_fwd_ad_type("double") + "()";
                    }
                }
            }
            diff.add_forward(lhs + ctx.fwd_suffix + " : " + fwd_ad_type + fwd_init + ";\n");
            diff.add_forward(lhs + " : " + type + prim_init + ";\n");
            diff.add_reverse_primal(lhs + ctx.rws_suffix + " : " + rws_ad_type + " = ();\n");
            diff.add_reverse_primal(lhs + " : " + type + prim_init + ";\n");
        }
        else {
            auto text = lhs + ": " + type;
            if (has_initializer) text += " = " + declaration.initializer.source();
            text += ";\n";
            diff.add_forward(text);
            diff.add_reverse_primal(text);
        }
        ctx.add_variable_declaration(lhs, active && type == "_" ? "double" : type, active);
        if (has_initializer) ctx.add_assigned(lhs);
    }

    auto expression_statement(const parsed_expression& text) -> void {
        // An empty statement, such as the `;` that ends a do loop.
        if (text.source().find_first_not_of(" \t\r\n;") == std::string::npos) return;
        if (const auto parsed = ctx.root(text)) expression(*parsed);
    }

    auto expression(node value) -> void {
        if (value.kind() == expression_kind::assignment) return assignment(value);
        if (is_postfix(value)) {
            const auto form = flatten(value);
            auto is_func = false;
            for (std::size_t index = 0; index < form.terms.size(); ++index) {
                if (form.terms[index].op == ".") continue;
                if (form.terms[index].op == "(" && index + 1 == form.terms.size()) {
                    is_func = true;
                    continue;
                }
                ctx.error("AD: Unknown operator for standalone postfix expression. op: " +
                          form.terms[index].op + " expr: " + value.to_string());
            }
            if (!is_func) {
                ctx.error("AD: Postfix expressions are only handled for function calls, or "
                          "member function calls. Do not know how to handle: " +
                          value.to_string());
                return;
            }
            auto handler = expression_handler{ctx};
            handler.handle_function_call(form, false);
            handler.diff += handler.fwd_expr + "\n";
            handler.diff += handler.primal_expr + "\n";
            diff.append(handler.diff);
            return;
        }
        const auto kind = value.kind() == expression_kind::binary
            ? (value.op_before(1) == "+" || value.op_before(1) == "-" ? std::string{"Additive"}
               : value.op_before(1) == "*" || value.op_before(1) == "/" ||
                       value.op_before(1) == "%"
                   ? std::string{"Multiplicative"}
                   : expression_handler::binary_kind(value.op_before(1)))
            : value.kind() == expression_kind::prefix ? std::string{"Prefix"}
            : value.kind() == expression_kind::is_as ? std::string{"Is as"}
            : std::string{"Primary"};
        ctx.error("AD: " + kind + " expressions are not yet handled as standalone statements.");
    }

    auto assignment(node value) -> void {
        const auto lhs = value.child(0);
        const auto rhs = value.child(1);
        auto active = ctx.is_active(lhs);
        if (lhs.to_string() == "_") active = ctx.is_active(rhs) || active;
        if (!active) {
            diff.add_forward(value.to_string() + ";\n");
            diff.add_reverse_primal(value.to_string() + ";\n");
            return;
        }
        if (value.token(value.value->op) != "=") {
            ctx.error("AD: Compound assignments are not yet handled: " + value.to_string());
            return;
        }
        auto h_lhs = expression_handler{ctx};
        h_lhs.traverse(lhs);
        auto h = expression_handler{ctx};
        h.traverse(rhs);
        const auto& name = h_lhs.primal_expr;
        const auto is_variable = lhs.is_identifier() && name != "_";
        const auto reads_self = is_variable ? rhs.mentions(name)
                                            : rhs.to_string().find(name) != std::string::npos;
        // Overwriting a value saves it for the backprop of the statements that read it.
        const auto saves = is_variable && (reads_self || ctx.is_assigned(name)) &&
            overwrite_push_pop;
        auto type = std::string{};
        if (saves) {
            type = ctx.lookup_variable_declaration(name).decl;
            diff.add_reverse_primal("cpp2::ad_stack::push<" + type + ">(" + name + ");");
        }
        if (reads_self && ctx.is_reverse()) {
            const auto t_b = ctx.gen_temporary() + ctx.rws_suffix;
            h.gen_assignment(name, h_lhs.fwd_expr, t_b);
            diff.append(h.diff);
            diff.add_reverse_backprop(h_lhs.rws_name + " = 0.0;\n");
            diff.add_reverse_backprop(t_b + " := " + h_lhs.rws_name + ";\n");
        }
        else {
            h.gen_assignment(name, h_lhs.fwd_expr, h_lhs.rws_name);
            diff.append(h.diff);
        }
        if (saves) {
            diff.add_reverse_backprop(name + " = cpp2::ad_stack::pop<" + type + ">();");
            ctx.add_restored(name);
        }
        if (is_variable) {
            add_overwritten({name});
            ctx.add_assigned(name);
        }
    }

    static auto reverse_next(context& ctx, const std::string& expression) -> std::string {
        if (expression.find("+=") != std::string::npos) return replace_all(expression, "+=", "-=");
        if (expression.find("-=") != std::string::npos) return replace_all(expression, "-=", "+=");
        ctx.error("AD: Do not know how to reverse: " + expression);
        return "Error";
    }

    auto loop(const statement& value, const std::vector<initialized_parameter>& parameters)
        -> void {
        const auto& form = value.as<statement::loop_form>();
        const auto diff_params = handle_stmt_parameters(parameters);
        const auto is_while = value.kind == statement_kind::while_statement;
        const auto is_do = value.kind == statement_kind::do_statement;
        if (ctx.is_reverse()) {
            ctx.error("AD: Alpha limitiation now reverse mode for while, do while or for.");
        }
        const auto& next = form.next.source();
        if (is_while) {
            if (!parameters.empty()) diff.add_forward("(" + diff_params.fwd + ")");
            diff += "while " + form.condition.source() + " ";
            if (!next.empty()) diff += "next " + next + " ";
            compound(form.body);
            return;
        }
        if (is_do) {
            if (!parameters.empty()) diff.add_forward("(" + diff_params.fwd + ")");
            diff += "do ";
            compound(form.body);
            if (!next.empty()) diff += "next " + next + " ";
            diff += "while " + form.condition.source() + ";";
            return;
        }
        if (!form.range_parameter) {
            ctx.error("AD: A for loop requires a parameter.");
            return;
        }
        const auto& range = form.condition.source();
        const auto& parameter = *form.range_parameter;
        const auto style = passing_name(parameter.passing);
        const auto& name = parameter.name;
        const auto type = type_or_wildcard(parameter.type);
        // A passive range, such as a `std::vector<int>`, has no derivative to iterate.
        if (const auto parsed = ctx.parse(range); !ctx.is_reverse() && parsed &&
            !ctx.is_active(*parsed)) {
            if (!parameters.empty()) diff.add_forward("(" + diff_params.fwd + ")");
            diff += "for " + range + " ";
            if (!next.empty()) diff += "next " + next + " ";
            diff += "do (" + style + " " + name + ": " + type + ") ";
            ctx.add_variable_declaration(name, type, false);
            compound(form.body);
            return;
        }
        auto rws = std::string{"("};
        auto rws_restore = std::string{};
        diff.add_forward("(");
        diff.add_reverse_primal("{\n");
        if (!parameters.empty()) {
            for (const auto& current : parameters) {
                if (current.initializer.empty()) continue;
                const auto current_type = type_or_wildcard(current.value.type);
                diff.add_reverse_primal(current.value.name + ": " + current_type + " = " +
                                        current.initializer.source() + ";\n");
                rws_restore += "cpp2::ad_stack::push<" + current_type + ">(" +
                    current.value.name + ");\n";
                rws += passing_name(current.value.passing) + " " + current.value.name + ": " +
                    current_type + " = cpp2::ad_stack::pop<" + current_type + ">(), ";
            }
            diff.add_forward(diff_params.fwd);
        }
        diff.add_forward("copy " + name + ctx.fwd_suffix + "_iter := " + range + ctx.fwd_suffix +
                         ".begin())\n");
        diff.add_forward("for " + range + " next (");
        rws += "copy " + name + ctx.rws_suffix + "_iter := " + range + ctx.rws_suffix +
            ".rbegin())\n";
        rws += "for std::ranges::reverse_view(" + range + ") next (";
        diff.add_reverse_primal("for " + range + " next (");
        if (!next.empty()) {
            diff.add_forward(next + ", ");
            diff.add_reverse_primal(next + ", ");
            rws += reverse_next(ctx, next) + ", ";
        }
        diff.add_forward(name + ctx.fwd_suffix + "_iter++");
        diff.add_forward(") do (" + style + " " + name + ": " + type + ") {\n");
        rws += name + ctx.rws_suffix + "_iter++";
        rws += ") do (" + style + " " + name + ": " + type + ") {\n";
        rws += "(inout " + name + ctx.rws_suffix + " := " + name + ctx.rws_suffix + "_iter*)\n";
        diff.add_reverse_primal(") do (" + style + " " + name + ": " + type + ")");
        diff.add_forward("(" + style + " " + name + ctx.fwd_suffix + ": " + type + " = " + name +
                         ctx.fwd_suffix + "_iter*)");
        ctx.add_variable_declaration(name, type, true);
        diff.add_reverse_backprop("}\n");
        compound(form.body);
        diff.add_forward("}\n");
        if (!next.empty()) diff.add_reverse_primal(reverse_next(ctx, next) + ";\n");
        if (!plain) diff.add_reverse_primal(rws_restore);
        diff.add_reverse_primal("}\n");
        diff.add_reverse_backprop(rws);
    }
};

class declaration_handler final {
public:
    declaration_handler(context& value, ir_declaration& decl)
        : ctx{value}, decl_{&decl}, diff{&value} {}

    context& ctx;
    bool is_type_context = false;
    std::string diff_ad_type;

    auto traverse(ir_declaration& value) -> void {
        if (value.kind == declaration_kind::function) return function(value);
        if (value.kind == declaration_kind::type) return type(value);
        if (value.kind == declaration_kind::object && value.alias.empty() &&
            value.name != "this") {
            return object(value);
        }
    }

private:
    ir_declaration* decl_;
    diff_code diff;

    auto function(ir_declaration& f) -> void {
        const auto saved_span = std::exchange(ctx.span, f.span);
        if (f.deduction != return_deduction::none &&
            f.deduction != return_deduction::by_forwarding_typed) {
            // A derivative returns named values, which need a type.
            ctx.error("AD: Alpha limitation: a deduced return type, declare the return type of " +
                      f.name);
            ctx.span = saved_span;
            return;
        }
        if (!f.template_parameters.empty()) {
            ctx.error("AD: Alpha limitation: a function template: " + f.name);
            ctx.span = saved_span;
            return;
        }
        ctx.enter_function(f);
        const auto unnamed_return = f.named_returns.empty() && !f.type.empty() && !f.type.is_void();
        ctx.unnamed_return = unnamed_return;
        diff.add_forward("    " + f.name + ctx.fwd_suffix + ": (");
        diff.add_reverse_primal("    " + f.name + ctx.rws_suffix + ": (");

        for (const auto& parameter : f.parameters) {
            const auto& name = parameter.name;
            const auto fwd_style = passing_name(parameter.passing);
            const auto rws_style = std::string{"inout"};
            if (name == "this") {
                // Only forward mode generates the type of member derivatives.
                if (ctx.is_reverse()) {
                    ctx.error("AD: Alpha limitation: reverse mode for member functions: " + f.name);
                }
                const auto fwd_ad_type = ctx.get_fwd_ad_type(decl_->name);
                const auto rws_ad_type = ctx.get_rws_ad_type(decl_->name);
                diff.add_forward(fwd_style + " " + name + ", ");
                diff.add_forward(fwd_style + " " + name + ctx.fwd_suffix + ": " + fwd_ad_type +
                                 ", ");
                diff.add_reverse_primal(fwd_style + " " + name + ", ");
                if (ctx.is_taylor()) {
                    diff.add_reverse_primal(fwd_style + " " + name + ctx.fwd_suffix + ": " +
                                            fwd_ad_type + ", ");
                }
                diff.add_reverse_primal(rws_style + " " + name + ctx.rws_suffix + ": " +
                                        rws_ad_type + ", ");
                continue;
            }
            const auto type = type_or_wildcard(parameter.type);
            const auto active = ctx.is_object_active(type, {});
            diff.add_forward(fwd_style + " " + name + " : " + type + ", ");
            diff.add_reverse_primal(fwd_style + " " + name + " : " + type + ", ");
            if (active) {
                diff.add_forward(fwd_style + " " + name + ctx.fwd_suffix + " : " +
                                 ctx.get_fwd_ad_type(type) + ", ");
                if (ctx.is_taylor()) {
                    diff.add_reverse_primal(fwd_style + " " + name + ctx.fwd_suffix + " : " +
                                            ctx.get_fwd_ad_type(type) + ", ");
                }
                diff.add_reverse_primal(rws_style + " " + name + ctx.rws_suffix + " : " +
                                        ctx.get_rws_ad_type(type) + ", ");
            }
            ctx.add_variable_declaration(name, type, active);
            ctx.add_assigned(name);
        }

        if (unnamed_return) {
            diff.add_reverse_primal("inout r" + ctx.rws_suffix + ": " +
                                    ctx.get_rws_ad_type(f.type.text) + ", ");
        }
        else {
            for (const auto& named : f.named_returns) {
                diff.add_reverse_primal("inout " + named.value.name + ctx.rws_suffix + " : " +
                                        ctx.get_rws_ad_type(named.value.type.text) + " , ");
            }
        }
        // A function without returns has a derivative without returns.
        const auto returns_list = unnamed_return || !f.named_returns.empty() ? ") -> (" : "";
        diff.add_forward(returns_list);
        diff.add_reverse_primal(returns_list);
        if (unnamed_return) {
            diff.add_forward("r: " + f.type.text + " = (), r" + ctx.fwd_suffix + ": " +
                             ctx.get_fwd_ad_type(f.type.text) + " = (), ");
            diff.add_reverse_primal("r: " + f.type.text + " = (), ");
            if (ctx.is_taylor()) {
                diff.add_reverse_primal("r" + ctx.fwd_suffix + ": " +
                                        ctx.get_fwd_ad_type(f.type.text) + " = (), ");
            }
        }
        else {
            for (const auto& named : f.named_returns) {
                const auto& name = named.value.name;
                const auto& type = named.value.type.text;
                diff.add_forward("out " + name + " : " + type + " = 0.0, ");
                diff.add_forward("out " + name + ctx.fwd_suffix + " : " +
                                 ctx.get_fwd_ad_type(type) + " = 0.0, ");
                diff.add_reverse_primal("out " + name + " : " + type + " = 0.0, ");
                if (ctx.is_taylor()) {
                    diff.add_reverse_primal("out " + name + ctx.fwd_suffix + " : " +
                                            ctx.get_fwd_ad_type(type) + " = 0.0, ");
                }
                ctx.add_variable_declaration(name, type, true);
            }
        }
        diff.add_forward(") = {");
        diff.add_reverse_primal(") = {");

        if (!f.function_statements) {
            ctx.error("temporary alpha limitation: a differentiable function must have a "
                      "{}-enclosed body");
            ctx.leave_function();
            ctx.span = saved_span;
            diff = diff_code{&ctx};
            return;
        }
        auto body = statement_handler{ctx, f};
        body.overwrite_push_pop = true;
        const auto& statements = f.function_statements->statements;
        if (!statements.empty()) body.last = statements.back().get();
        for (const auto& value : statements) body.traverse(*value);
        diff.add_forward(body.diff.fwd);
        diff.add_reverse_primal(body.diff.rws_primal);
        // The backprop pass restores overwritten values, so it keeps the returned ones aside.
        auto returns = std::vector<std::string>{};
        if (unnamed_return) returns.emplace_back("r");
        for (const auto& named : f.named_returns) returns.push_back(named.value.name);
        auto restore = std::string{};
        for (const auto& name : returns) {
            if (!ctx.is_restored(name)) continue;
            const auto saved = ctx.gen_temporary();
            diff.add_reverse_primal(saved + " := " + name + ";\n");
            restore += name + " = " + saved + ";\n";
        }
        diff.add_reverse_primal(body.diff.rws_backprop);
        diff.add_reverse_primal(restore);
        diff.add_forward("}");
        diff.add_reverse_primal("}");
        ctx.leave_function();

        if (ctx.is_forward()) ctx.add_member(*decl_, diff.fwd);
        if (ctx.is_reverse()) ctx.add_member(*decl_, diff.rws_primal);
        diff = diff_code{&ctx};
        ctx.add_as_differentiated(f);
        ctx.span = saved_span;
    }

    auto object(ir_declaration& o) -> void {
        const auto saved_span = std::exchange(ctx.span, o.span);
        const auto type = type_or_wildcard(o.type);
        auto init = std::string{};
        if (!o.body.empty()) {
            if (const auto parsed = ctx.root(o.body)) {
                auto handler = expression_handler{ctx};
                handler.traverse(*parsed);
                if (!handler.diff.fwd.empty()) {
                    ctx.error("AD: The initializer of a differentiated member must be a simple "
                              "expression: " + o.body.source());
                }
                init = " = " + handler.fwd_expr;
            }
        }
        const auto text = o.name + ctx.fwd_suffix + " : " + ctx.get_fwd_ad_type(type) + init + ";";
        if (is_type_context) diff_ad_type += "public " + text + "\n";
        else ctx.add_member(*decl_, text);
        ctx.add_variable_declaration(o.name, type, true, true);
        ctx.span = saved_span;
    }

    auto type(ir_declaration& t) -> void {
        const auto saved_span = std::exchange(ctx.span, t.span);
        ctx.push_stack(t);
        auto handler = declaration_handler{ctx, t};
        handler.is_type_context = true;
        for (auto& member : t.members) handler.traverse(member);
        ctx.pop_stack();
        ctx.add_as_differentiated(t);
        if (!handler.diff_ad_type.empty()) {
            ctx.add_member(*decl_, t.name + ctx.fwd_suffix + " : type = {\n" +
                           handler.diff_ad_type + "}", &t);
        }
        ctx.span = saved_span;
    }
};

auto context::pop_stack() -> void {
    auto& top = stack.back();
    auto handler = declaration_handler{*this, *top.decl};
    // Differentiating a request may add more.
    for (std::size_t index = 0; index < stack.back().diff_request.size(); ++index) {
        auto* request = stack.back().diff_request[index];
        const auto& done = stack.back().diff_done;
        if (std::find(done.begin(), done.end(), request) == done.end()) {
            handler.traverse(*request);
        }
    }
    stack.pop_back();
}

auto parents_of(std::vector<ir_declaration>& declarations, ir_declaration* parent,
                std::unordered_map<const ir_declaration*, ir_declaration*>& parents) -> void {
    for (auto& declaration : declarations) {
        parents[&declaration] = parent;
        parents_of(declaration.members, &declaration, parents);
    }
}

auto find_declaration(std::vector<ir_declaration>& declarations, symbol_id symbol)
    -> ir_declaration* {
    for (auto& declaration : declarations) {
        if (declaration.symbol == symbol) return &declaration;
        if (auto* found = find_declaration(declaration.members, symbol)) return found;
    }
    return nullptr;
}

// Parses a generated member of `target` into IR, allocating its symbols in `program`.
auto parse_generated(const addition& value, const ir_declaration& target, ir_program& program,
                     std::vector<diagnostic>& diagnostics) -> std::vector<ir_declaration> {
    auto parsed = parse_generated_members(value.text, target, program, value.span);
    if (!parsed.error.empty()) {
        diagnostics.push_back({"autodiff", diagnostic_severity::error, value.span,
                               "AD: Generated code is not valid Cpp2 (" + parsed.error + "):\n" +
                                   value.text});
    }
    return std::move(parsed.members);
}

auto apply_additions(ir_program& program, const std::vector<addition>& additions,
                     std::vector<diagnostic>& diagnostics) -> void {
    for (const auto& value : additions) {
        auto* target = find_declaration(program.declarations, value.target);
        if (target == nullptr) continue;
        auto members = parse_generated(value, *target, program, diagnostics);
        auto position = target->members.end();
        if (value.before != invalid_symbol) {
            position = std::find_if(target->members.begin(), target->members.end(),
                [&](const ir_declaration& member) { return member.symbol == value.before; });
        }
        auto generated = ir_declaration::generated_member{value.text, std::nullopt};
        if (position != target->members.end() && !position->is_generated) {
            generated.before = position->span.begin;
        }
        target->generated_members.push_back(std::move(generated));
        target->members.insert(position, std::make_move_iterator(members.begin()),
                               std::make_move_iterator(members.end()));
    }
}

// Runs one `@autodiff` on `type`.
auto autodiff(ir_program& program, symbol_id type_symbol,
              const std::vector<std::string>& arguments,
              std::vector<diagnostic>& diagnostics) -> void {
    auto parents = std::unordered_map<const ir_declaration*, ir_declaration*>{};
    parents_of(program.declarations, nullptr, parents);
    auto* type = find_declaration(program.declarations, type_symbol);
    const auto settings = parse_autodiff_options(arguments);
    if (!settings.error.empty()) {
        diagnostics.push_back({"autodiff", diagnostic_severity::error, type->span, settings.error});
        return;
    }
    if (type->kind != declaration_kind::type) {
        diagnostics.push_back({"autodiff", diagnostic_severity::error, type->span,
                               "'@autodiff' applies only to types"});
        return;
    }
    const auto previous_diagnostics = diagnostics.size();
    auto ctx = context{settings, parents, diagnostics};
    ctx.span = type->span;
    if (auto* parent = parents[type]) {
        auto chain = std::vector<ir_declaration*>{};
        for (auto* scope = parent; scope != nullptr; scope = parents[scope]) chain.push_back(scope);
        for (auto scope = chain.rbegin(); scope != chain.rend(); ++scope) ctx.push_stack(**scope);
        auto handler = declaration_handler{ctx, *parent};
        handler.traverse(*type);
    }
    else {
        // At file scope only the type's own functions are differentiated.
        ctx.push_stack(*type);
        auto handler = declaration_handler{ctx, *type};
        for (auto& member : type->members) {
            if (member.kind == declaration_kind::function) handler.traverse(member);
        }
        ctx.pop_stack();
    }
    ctx.finish();
    // Code generated after an error is incomplete; parsing it would only report that again.
    const auto has_error = std::any_of(
        diagnostics.begin() + static_cast<std::ptrdiff_t>(previous_diagnostics), diagnostics.end(),
        [](const diagnostic& value) { return value.severity == diagnostic_severity::error; });
    if (!has_error) apply_additions(program, ctx.additions, diagnostics);
}

}

auto apply_autodiff(ir_program& program, std::vector<diagnostic>& diagnostics) -> void {
    auto uses = std::vector<std::pair<symbol_id, std::vector<std::string>>>{};
    const auto collect = [&](const auto& self, const std::vector<ir_declaration>& declarations)
        -> void {
        for (const auto& declaration : declarations) {
            for (auto& arguments : autodiff_arguments(declaration)) {
                uses.emplace_back(declaration.symbol, std::move(arguments));
            }
            self(self, declaration.members);
        }
    };
    collect(collect, program.declarations);
    for (const auto& [symbol, arguments] : uses) {
        autodiff(program, symbol, arguments, diagnostics);
    }
}

auto autodiff_includes(const std::vector<ir_declaration>& declarations)
    -> std::vector<std::string> {
    auto taylor = false;
    auto stack = false;
    ir_detail::visit_declarations(declarations, [&](const ir_declaration& declaration) {
        for (const auto& arguments : autodiff_arguments(declaration)) {
            const auto settings = parse_autodiff_options(arguments);
            taylor |= settings.order != 1;
            stack |= settings.reverse;
        }
    });
    auto result = std::vector<std::string>{};
    if (stack) result.emplace_back("\"cpp2ad_stack.h\"");
    if (taylor) result.emplace_back("\"cpp2taylor.h\"");
    return result;
}

}
