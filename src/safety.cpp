#include <cppfrontier/expression.hpp>
#include <cppfrontier/pipeline.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "brackets.hpp"
#include "cpp1_expression_renderer.hpp"
#include "cpp1_text.hpp"
#include "ir_walk.hpp"
#include "known_types.hpp"
#include "mentions.hpp"
#include "scope_model.hpp"

namespace cppfrontier {
namespace {

enum class initialization_state : std::uint8_t {
    uninitialized,
    partial,
    initialized,
};

// Experiment `use-after-move`: whether a name has been moved from on the
// current path since it was last assigned.
enum class move_state : std::uint8_t { not_moved, maybe_moved, moved };

struct local_state final {
    initialization_state initialization = initialization_state::uninitialized;
    source_span declaration;
    std::string name;
    bool deferred = false;
    bool reported = false;
    // An `out` argument that may not be evaluated was passed it uninitialized.
    bool conditional_out = false;
    // An `out` parameter or named return value, which every return must initialize.
    bool returned = false;
    bool named_return = false;
    // The number of loops enclosing the declaration.
    int loop_depth = 0;
    // A local or parameter whose explicit moves are followed (experiment
    // `use-after-move`): not a scalar, whose move is a copy.
    bool tracks_moves = false;
    move_state moved = move_state::not_moved;
    // The statement that moved it.
    source_span moved_at;
    // While a loop is analyzed again from the state of its next iteration:
    // the statement inside the loop whose move reached this iteration.
    std::optional<source_span> carried;
    // The `@linear` type of a value that must be consumed exactly once, or null.
    const scope_model::entry* linear_type = nullptr;
    // An `inout` or `out` parameter, whose value is the caller's: a linear
    // one must not be left moved from.
    bool borrowed = false;
};

using scope_entry = scope_model::entry;

// The functions of the program, to find how a call passes an argument.
class known_callees final {
public:
    explicit known_callees(const known_types& types) : types_{types} {}

    [[nodiscard]] auto types() const -> const known_types& { return types_; }

    // Whether the call that `value` is a whole argument of passes it by
    // `copy`, `move` or `forward`: every candidate of the call, and at least
    // one, has such a parameter there.
    [[nodiscard]] auto passes_by_value(const mention& value, scope_point point) const -> bool {
        if (value.call == nullptr) return false;
        return types_.callees(*value.text, *value.call, point).all_bind(value.argument,
            [](const parameter& found) {
                return found.passing == passing_style::copy ||
                    found.passing == passing_style::move || found.passing == passing_style::forward;
            });
    }

private:
    const known_types& types_;
};

struct analysis_state final {
    // Ordered by declaration, so diagnostics are reported in a stable order.
    std::map<const scope_entry*, local_state> locals;
    // Set after a `return`, `throw`, `break` or `continue`; such a branch does
    // not reach the code after its if.
    bool leaves = false;
};

// The loop being analyzed: the states at each `break` and `continue` that
// targets it, which reach the code after the loop and its next iteration.
struct loop_frame final {
    std::string label;
    // The loop depth of the body: locals declared at this depth or deeper
    // leave their scope at a jump to this loop.
    int depth = 1;
    // The number of defer statements registered outside the loop.
    std::size_t defers = 0;
    std::vector<analysis_state> breaks;
    std::vector<analysis_state> continues;
};

// How a text uses one local.
struct local_use final {
    mention_role role = mention_role::read;
    // The token of the mention in the text, or of the string or function
    // expression holding it.
    std::size_t token = 0;
    // For a linear local, a whole name that passes the value on: a whole
    // initializer, or a whole argument for a `copy`, `move` or `forward`
    // parameter.
    bool passes = false;
    // The inspect alternatives holding the mention, outermost first, as the
    // inspect expression and the index of the alternative among its children.
    std::vector<std::pair<const expression*, std::size_t>> alternatives;
    // The operands holding the mention that are evaluated on some paths only,
    // outermost first, as the node and the index of the operand among its
    // children: an inspect alternative, and an operand after the first of
    // `&&` or `||`.
    std::vector<std::pair<const expression*, std::size_t>> choices;
    // Where a mention in a capture `x$` is, which `token` does not say.
    std::optional<source_span> span;
};

// The group of a contract as an expression, as in `assert<group*>(c)`; empty
// for the default group and `unevaluated`.
auto contract_group_text(const contract_clause& clause) -> parsed_expression {
    if (clause.group.empty() || clause.group == "_" || clause.group == "default" ||
        clause.group == "unevaluated") {
        return {};
    }
    return parsed_expression{clause.group, false, false, false, clause.group_begin};
}

// The uses of the locals of `state` in a text, by local.
using local_uses = std::map<const scope_entry*, std::vector<local_use>>;

// For the moves of a local on one path through a text: `move name` moves
// it, and passing a linear value on consumes it, except in a contract;
// `name = value` and `out name` assign it, and any other use reads it.
struct move_access final {
    int moves = 0;
    int reads = 0;
    bool assigned = false;

    auto operator<=>(const move_access&) const = default;
};

auto add_use(move_access& result, const local_use& use, bool linear, bool contract) -> void {
    if (use.role == mention_role::assigned || use.role == mention_role::out) {
        result.assigned = true;
    }
    else if (linear ? !contract && (use.role == mention_role::moved || use.passes)
                    : use.role == mention_role::moved) {
        ++result.moves;
    }
    else {
        ++result.reads;
    }
}

// The distinct accesses of the paths through a text that `uses` make, from
// the alternatives at `depth`: the uses in one alternative of an inspect
// expression exclude those in its other alternatives.
auto move_accesses(const std::vector<const local_use*>& uses, bool linear, bool contract,
                   std::size_t depth = 0) -> std::vector<move_access> {
    auto common = move_access{};
    auto inspects = std::map<const expression*, std::map<std::size_t,
                                                         std::vector<const local_use*>>>{};
    for (const auto* use : uses) {
        if (use->alternatives.size() == depth) {
            add_use(common, *use, linear, contract);
        }
        else {
            const auto& [inspect, index] = use->alternatives[depth];
            inspects[inspect][index].push_back(use);
        }
    }
    auto result = std::vector<move_access>{common};
    for (const auto& [inspect, alternatives] : inspects) {
        auto choices = std::set<move_access>{};
        const auto count = std::count_if(inspect->children.begin(), inspect->children.end(),
            [](const expression& child) { return child.kind == expression_kind::alternative; });
        if (static_cast<std::size_t>(count) > alternatives.size()) {
            choices.insert(move_access{});
        }
        for (const auto& [index, group] : alternatives) {
            for (const auto& nested : move_accesses(group, linear, contract, depth + 1)) {
                choices.insert(nested);
            }
        }
        auto combined = std::set<move_access>{};
        for (const auto& path : result) {
            for (const auto& choice : choices) {
                combined.insert({path.moves + choice.moves, path.reads + choice.reads,
                                 path.assigned || choice.assigned});
            }
        }
        result.assign(combined.begin(), combined.end());
    }
    return result;
}

// Whether `node` is a chain of `&&` or of `||`, whose later operands are
// evaluated on some paths only.
auto is_short_circuit(const expression_text& text, const expression& node) -> bool {
    if (node.kind != expression_kind::binary || node.children.size() < 2) return false;
    const auto op = node.children.front().last;
    return op < text.tokens.size() &&
        (text.tokens[op].text == "&&" || text.tokens[op].text == "||");
}

// The index of the child of `node` that holds token `token`, if any.
auto child_holding(const expression& node, std::size_t token) -> std::optional<std::size_t> {
    // The children are ordered and do not overlap.
    const auto next = std::upper_bound(node.children.begin(), node.children.end(), token,
        [](std::size_t value, const expression& child) { return value < child.first; });
    if (next == node.children.begin() || token >= std::prev(next)->last) return std::nullopt;
    return static_cast<std::size_t>(std::prev(next) - node.children.begin());
}

// Sets `use.alternatives` and `use.choices` for the mention at token `token` of `text`.
auto locate_use(const expression_text& text, std::size_t token, local_use& use) -> void {
    for (const auto* parent = &text.root;;) {
        const auto index = child_holding(*parent, token);
        if (!index) break;
        const auto& child = parent->children[*index];
        if (parent->kind == expression_kind::inspect &&
            child.kind == expression_kind::alternative) {
            use.alternatives.emplace_back(parent, *index);
            use.choices.emplace_back(parent, *index);
        }
        else if (*index > 0 && is_short_circuit(text, *parent)) {
            use.choices.emplace_back(parent, *index);
        }
        parent = &child;
    }
}

// Whether C++17 sequences the evaluation of child `first` of `node` before its
// child `second`: the operands of `<<`, `>>`, `&&` and `||` and the elements of
// a list `(a, b)`, a braced list in Cpp1, left to right, an assignment's
// right operand before its left, and an `inspect` subject before its
// alternatives. Other operands, such as those of `+` and the
// arguments of a call, are unsequenced.
auto sequenced(const expression_text& text, const expression& node, std::size_t first,
               std::size_t second) -> bool {
    switch (node.kind) {
    case expression_kind::binary: {
        const auto op = binary_operator(text.tokens, node, 1);
        return first < second && (op == "<<" || op == ">>" || op == "&&" || op == "||");
    }
    case expression_kind::assignment:
        return first == 1 && second == 0;
    case expression_kind::group:
        return node.children.size() > 1 && first < second;
    case expression_kind::inspect:
        return first == 0 && second > 1;
    default:
        return false;
    }
}

// Whether the mention at token `first` of `text` is evaluated before the one at
// `second`, by the `sequenced` order of the operands where they part. With
// `whenever`, also whenever the second is: the first is not in an operand,
// below that point, that is evaluated on some paths only.
auto evaluated_before(const expression_text& text, std::size_t first, std::size_t second,
                      bool whenever) -> bool {
    const auto* node = &text.root;
    for (;;) {
        const auto one = child_holding(*node, first);
        const auto other = child_holding(*node, second);
        if (!one || !other) return false;
        if (*one == *other) {
            node = &node->children[*one];
            continue;
        }
        if (!sequenced(text, *node, *one, *other)) return false;
        if (!whenever) return true;
        // The first's operands below the split are evaluated whenever their parents are.
        for (node = &node->children[*one];;) {
            const auto index = child_holding(*node, first);
            if (!index) return true;
            const auto& child = node->children[*index];
            if ((node->kind == expression_kind::inspect &&
                 child.kind == expression_kind::alternative) ||
                (*index > 0 && is_short_circuit(text, *node))) {
                return false;
            }
            node = &child;
        }
    }
}

// Whether one of `outs`, `out` arguments, is evaluated on every path through
// the operands from the choices at `depth`.
auto evaluated_on_every_path(const std::vector<const local_use*>& outs, std::size_t depth = 0)
    -> bool {
    auto groups = std::map<const expression*, std::map<std::size_t,
                                                       std::vector<const local_use*>>>{};
    for (const auto* use : outs) {
        if (use->choices.size() == depth) return true;
        const auto& [node, index] = use->choices[depth];
        groups[node][index].push_back(use);
    }
    for (const auto& [node, operands] : groups) {
        // Every alternative of an inspect.
        if (node->kind != expression_kind::inspect) continue;
        const auto needed = static_cast<std::size_t>(std::count_if(node->children.begin(),
            node->children.end(),
            [](const expression& child) { return child.kind == expression_kind::alternative; }));
        const auto covered = std::count_if(operands.begin(), operands.end(),
            [&](const auto& operand) { return evaluated_on_every_path(operand.second, depth + 1); });
        if (static_cast<std::size_t>(covered) == needed) return true;
    }
    return false;
}

// The second of two `out` arguments in `uses` that can both be evaluated,
// that is not in different alternatives of one inspect expression.
auto repeated_out(const std::vector<local_use>& uses) -> const local_use* {
    const auto exclusive = [](const local_use& a, const local_use& b) {
        const auto depth = std::min(a.alternatives.size(), b.alternatives.size());
        for (auto index = std::size_t{0}; index < depth; ++index) {
            if (a.alternatives[index].first != b.alternatives[index].first) return false;
            if (a.alternatives[index].second != b.alternatives[index].second) return true;
        }
        return false;
    };
    for (auto later = std::size_t{0}; later < uses.size(); ++later) {
        if (uses[later].role != mention_role::out) continue;
        for (auto earlier = std::size_t{0}; earlier < later; ++earlier) {
            if (uses[earlier].role == mention_role::out && !exclusive(uses[earlier], uses[later])) {
                return &uses[later];
            }
        }
    }
    return nullptr;
}

// For the initialization of a deferred local: `name = value` assigns it when
// it is the first use other than an `out` argument, `out name` assigns it
// when an `out` argument is evaluated on every path, and any other use reads
// it, unless an `out` argument is evaluated before it whenever it is.
struct initialization_access final {
    bool assignment = false;
    bool out_argument = false;
    // An `out` argument that is evaluated on some paths only, and so does not initialize.
    bool conditional_out = false;
    bool read = false;
    // The token of the first assignment or `out` argument.
    std::size_t position = std::numeric_limits<std::size_t>::max();
    // The token of the assignment's target, when `assignment`.
    std::size_t assigned = std::numeric_limits<std::size_t>::max();
    // The token of the first read, and where it is when `read_position` does not say.
    std::size_t read_position = std::numeric_limits<std::size_t>::max();
    std::optional<source_span> read_span;

    auto read_at(std::size_t token, std::optional<source_span> span = std::nullopt) -> void {
        if (!read || token < read_position) {
            read_position = token;
            read_span = span;
        }
        read = true;
    }
};

auto initialization_access_of(const std::vector<local_use>& uses, const expression_text& text)
    -> initialization_access {
    auto result = initialization_access{};
    auto outs = std::vector<const local_use*>{};
    for (const auto& use : uses) {
        if (use.role == mention_role::out) outs.push_back(&use);
    }
    for (const auto& use : uses) {
        if (use.role == mention_role::assigned && !result.assignment && !result.read) {
            result.assignment = true;
            result.assigned = use.token;
            result.position = std::min(result.position, use.token);
        }
        else if (use.role == mention_role::out) {
            result.position = std::min(result.position, use.token);
        }
        else if (std::none_of(outs.begin(), outs.end(), [&](const local_use* out) {
                     return evaluated_before(text, out->token, use.token, true);
                 })) {
            result.read_at(use.token, use.span);
        }
    }
    result.out_argument = evaluated_on_every_path(outs);
    result.conditional_out = !outs.empty() && !result.out_argument;
    return result;
}

// The span of token `token` of `text`, or `fallback` when the text does not
// know where it begins.
auto token_span(const parsed_expression& text, std::size_t token, source_span fallback)
    -> source_span {
    const auto begin = text.position_of(token);
    if (!begin) return fallback;
    auto end = *begin;
    end.column += static_cast<std::uint32_t>(text.tree().tokens[token].text.size());
    return {*begin, end};
}

// Joins the move state of another path into `result`.
auto merge_moves(local_state& result, const local_state& other) -> void {
    if (result.moved == move_state::not_moved) result.moved_at = other.moved_at;
    if (result.moved != other.moved) result.moved = move_state::maybe_moved;
}

auto before(const source_position& left, const source_position& right) -> bool {
    return left.line != right.line ? left.line < right.line : left.column < right.column;
}


auto report_once(local_state& state, std::vector<diagnostic>& diagnostics,
                 std::string_view code, source_span span, std::string message) -> void {
    if (state.reported) return;
    diagnostics.push_back({std::string{code}, diagnostic_severity::error, span,
                           std::move(message)});
    state.reported = true;
}

// The note on a diagnostic about `local` not being initialized, for an `out`
// argument that may not have initialized it.
auto out_hint(const local_state& local) -> std::string {
    return local.conditional_out
        ? " (an 'out' argument after '&&' or '||' or in one inspect alternative may not be "
          "evaluated, and does not initialize it)"
        : "";
}

auto merge_initialization(initialization_state first, initialization_state second)
    -> initialization_state {
    if (first == initialization_state::initialized &&
        second == initialization_state::initialized) {
        return initialization_state::initialized;
    }
    if (first == initialization_state::uninitialized &&
        second == initialization_state::uninitialized) {
        return initialization_state::uninitialized;
    }
    return initialization_state::partial;
}

// The state where `paths`, which split after `before`, meet again. A path
// that leaves does not reach it; when every path leaves, neither does the
// result, which then merges every path, as the state at the end of the body.
auto join(const analysis_state& before, const std::vector<const analysis_state*>& paths)
    -> analysis_state {
    auto result = before;
    result.leaves = true;
    const auto all_leave = std::all_of(paths.begin(), paths.end(),
                                       [](const analysis_state* path) { return path->leaves; });
    auto first = true;
    for (const auto* path : paths) {
        // A local reported on any path is not reported again.
        for (auto& [symbol, local] : result.locals) {
            const auto found = path->locals.find(symbol);
            if (found != path->locals.end()) local.reported = local.reported || found->second.reported;
        }
        if (path->leaves && !all_leave) continue;
        for (auto& [symbol, local] : result.locals) {
            const auto found = path->locals.find(symbol);
            if (found == path->locals.end()) continue;
            if (first) {
                local.initialization = found->second.initialization;
                local.conditional_out = found->second.conditional_out;
                local.moved = found->second.moved;
                local.moved_at = found->second.moved_at;
                local.carried = found->second.carried;
            }
            else {
                local.initialization = merge_initialization(local.initialization,
                                                            found->second.initialization);
                local.conditional_out = local.conditional_out || found->second.conditional_out;
                merge_moves(local, found->second);
                if (!local.carried) local.carried = found->second.carried;
            }
        }
        first = false;
        result.leaves = all_leave;
    }
    return result;
}

// An `if` that initializes a local on one branch but not the other. A
// partial branch was already reported by the nested `if` that produced it.
auto check_branch_initialization(analysis_state& before, const analysis_state& first,
                                 const analysis_state& second, source_span span,
                                 std::vector<diagnostic>& diagnostics) -> void {
    for (auto& [symbol, local] : before.locals) {
        const auto first_state = first.locals.find(symbol);
        const auto second_state = second.locals.find(symbol);
        if (first_state == first.locals.end() || second_state == second.locals.end()) continue;
        const auto merged = merge_initialization(
            first_state->second.initialization, second_state->second.initialization);
        if (merged == initialization_state::partial &&
            local.initialization != initialization_state::partial &&
            first_state->second.initialization != initialization_state::partial &&
            second_state->second.initialization != initialization_state::partial) {
            diagnostics.push_back({
                "initialization-branch-mismatch", diagnostic_severity::error, local.declaration,
                "local variable " + local.name +
                    " must be initialized on both branches or neither branch"});
            diagnostics.push_back({
                "initialization-branch-mismatch", diagnostic_severity::error, span,
                "\"if\" initializes " + local.name + " on one branch but not the other"});
            local.reported = true;
        }
    }
}

// A diagnostic of the flow analysis about a local, keyed by a code and a
// position, which is reported once.
using flow_report = std::tuple<const scope_entry*, std::string_view, std::uint32_t, std::uint32_t>;

struct analysis_context final {
    std::vector<diagnostic>& diagnostics;
    initialization_plan& plan;
    const scope_model& scopes;
    const std::vector<function_expression>& expressions;
    const known_callees& callees;
    // Experiment `use-after-move`.
    bool use_after_move = false;
    // Experiment `try-operator`, whose `?` may return early.
    bool try_operator = false;
    // The loops enclosing the statement being analyzed, innermost last.
    std::vector<loop_frame>* loops;
    // The defer statements registered in the enclosing blocks, with their
    // loop depths, in order of registration.
    std::vector<std::pair<const statement*, int>>* defers;
    // The `@linear` types declared in the program.
    const std::set<symbol_id>* linear_types;
    std::set<flow_report>* reported;
    // While a loop is analyzed again from the state of its next iteration,
    // where `move-in-loop` is reported; other diagnostics go to `diagnostics`
    // and are dropped, since the first pass found them.
    std::vector<diagnostic>* loop_diagnostics = nullptr;
    // The `that` of the copy constructor being analyzed, which a capture
    // `that$` would copy by calling the constructor again; and its span.
    const scope_entry* copied_that = nullptr;
    source_span copy_span;

    // Whether moves are followed: for every tracked name under experiment
    // `use-after-move`, and for values of `@linear` types under `linear`.
    [[nodiscard]] auto follows_moves() const -> bool {
        return use_after_move || !linear_types->empty();
    }

    // Whether `entry` has no report `code` at `at` yet, which records it.
    // While a loop is analyzed again, only `move-in-loop`, a report of `carried`, is made.
    [[nodiscard]] auto first_report(const scope_entry& entry, std::string_view code,
                                    source_span at, bool carried = false) const -> bool {
        return (loop_diagnostics == nullptr || carried) &&
            reported->insert({&entry, code, at.begin.line, at.begin.column}).second;
    }
};

auto same_position(const source_span& left, const source_span& right) -> bool {
    return left.begin.line == right.begin.line && left.begin.column == right.begin.column;
}

// The `@linear` type of the known type of `entry`, or null.
auto linear_type_of(const scope_entry& entry, const analysis_context& context)
    -> const scope_entry* {
    if (context.linear_types->empty()) return nullptr;
    const auto& types = context.callees.types();
    const auto type = types.type_of(entry);
    const auto* found = type ? types.declared_type(*type) : nullptr;
    return found != nullptr && context.linear_types->contains(found->symbol) ? found : nullptr;
}

// Declares the local or parameter `entry` at loop depth `loop_depth`. The
// moves are followed of one that owns its value or is an `inout` or `out`
// parameter, when its type is known: under `use-after-move` unless it is a
// scalar, whose move is a copy, and when it is linear.
auto declare(analysis_state& state, const scope_entry& entry, int loop_depth,
             const analysis_context& context) -> local_state& {
    const auto* parameter = entry.declared_parameter;
    const auto* form = entry.declaring_statement != nullptr
        ? std::get_if<statement::declaration_form>(&entry.declaring_statement->form) : nullptr;
    const auto* declaration = form != nullptr && form->declaration ? &*form->declaration : nullptr;
    // A statement parameter has an initializer.
    const auto initialized = parameter != nullptr
        ? parameter->passing != passing_style::out ||
            entry.kind == scope_model::entry_kind::statement_parameter
        : declaration == nullptr || !declaration->initializer.empty();
    auto& local = state.locals[&entry];
    local.declaration = entry.span;
    local.name = entry.name;
    local.initialization = initialized ? initialization_state::initialized
                                       : initialization_state::uninitialized;
    local.deferred = !initialized;
    local.loop_depth = loop_depth;
    local.named_return = declaration != nullptr && declaration->is_named_return;
    local.returned = local.named_return || (parameter != nullptr && !initialized);
    // An `in` parameter is a const reference, which a move copies.
    const auto passing = parameter != nullptr ? parameter->passing : passing_style::move;
    if (entry.is_static_or_alias || passing == passing_style::in) return local;
    if (!local.named_return) local.linear_type = linear_type_of(entry, context);
    local.borrowed = passing == passing_style::inout || passing == passing_style::out;
    const auto type = context.callees.types().type_of(entry);
    local.tracks_moves = local.linear_type != nullptr ||
        (context.use_after_move && type && !known_types::is_scalar(*type));
    return local;
}

auto declare_bindings(analysis_state& state, const std::vector<statement::binding>& bindings,
                      int loop_depth, const analysis_context& context) -> void {
    for (const auto& binding : bindings) {
        if (const auto* entry = context.scopes.declared_by(binding)) {
            declare(state, *entry, loop_depth, context);
        }
    }
}

// Reports a `@linear` value that a path leaving its scope has not consumed,
// or for an `inout` or `out` parameter, has left moved from.
auto report_unconsumed(const scope_entry& entry, const local_state& local,
                       const analysis_context& context) -> void {
    if (local.linear_type == nullptr ||
        local.moved == (local.borrowed ? move_state::not_moved : move_state::moved) ||
        !context.first_report(entry, "linear-not-consumed", local.declaration)) {
        return;
    }
    context.diagnostics.push_back({"linear-not-consumed", diagnostic_severity::error,
        local.declaration, local.linear_type->name + " " + local.name +
        (local.borrowed ? " is moved from and not reassigned on every path"
                        : " is not consumed on every path")});
}

// Reports the `@linear` values declared at `loop_depth` or deeper, whose
// scope the current path leaves.
auto report_unconsumed(const analysis_state& state, const analysis_context& context,
                       int loop_depth) -> void {
    for (const auto& [entry, local] : state.locals) {
        if (local.loop_depth >= loop_depth) report_unconsumed(*entry, local, context);
    }
}

// Whether `local` is the declaration its name finds at `point`.
auto names(const scope_model& scopes, const scope_entry* local, const local_state& state,
           scope_point point) -> bool {
    return scopes.lookup(state.name, point) == local;
}

// The uses in `text` of the locals of `state` its mentions name at `point`,
// in declaration order. With `initializer`, the text initializes a value, as
// a capture `x$` initializes the capture.
auto uses_of_locals(const parsed_expression& text, const analysis_state& state,
                    const analysis_context& context, scope_point point, bool initializer)
    -> local_uses {
    auto result = local_uses{};
    for_each_mention(text.tree(), [&](const mention& value) {
        if (value.compile_time || value.function_name) return;
        const auto* entry = context.scopes.lookup(value.name(), point);
        if (entry == nullptr || !state.locals.contains(entry)) return;
        auto passes = false;
        if (state.locals.at(entry).linear_type != nullptr) {
            const auto* whole = whole_expression(*value.text);
            passes = whole != nullptr && whole->first == value.token && is_single_name(*whole) &&
                (value.capture || (initializer && value.text == &text.tree()));
            if (!passes) passes = context.callees.passes_by_value(value, point);
        }
        auto& use = result[entry].emplace_back(local_use{value.role, value.path.front(), passes});
        locate_use(text.tree(), value.path.front(), use);
        if (value.capture != nullptr) {
            if (const auto begin = value.capture->position_of(value.token)) {
                auto end = *begin;
                end.column += static_cast<std::uint32_t>(value.name().size());
                use.span = source_span{*begin, end};
            }
            const auto at = use.span.value_or(context.copy_span);
            if (entry == context.copied_that && value.capture->source() == "that" &&
                context.first_report(*entry, "copy-captures-that", at)) {
                context.diagnostics.push_back({"copy-captures-that", diagnostic_severity::error,
                    at, "capturing 'that' copies it with this copy constructor, which recurses; "
                        "capture its members instead, as 'that.member$'"});
            }
        }
    }, &context.expressions);
    return result;
}

// A deferred local that is not yet initialized.
auto is_pending(const local_state& local) -> bool {
    return local.deferred && local.initialization != initialization_state::initialized;
}

// Experiment `use-after-move`: reports a use in `statement` of `local`,
// moved on some path reaching it, once per move: as `move-in-loop` when the
// move reached it from the previous iteration of a loop.
auto report_moved_use(const scope_entry& entry, const local_state& local, source_span at,
                      bool consumes, const analysis_context& context) -> void {
    if (local.carried && context.first_report(entry, "moved-use", *local.carried, true)) {
        context.loop_diagnostics->push_back({"move-in-loop", diagnostic_severity::error,
            *local.carried, local.name +
            " is moved inside a loop and is used moved-from on the next iteration"});
    }
    if (local.moved == move_state::not_moved ||
        !context.first_report(entry, "moved-use", local.moved_at)) {
        return;
    }
    const auto line = std::to_string(local.moved_at.begin.line);
    const auto verb = consumes ? std::string{"consumed"} : std::string{"moved"};
    const auto again = consumes ? " and is consumed again here" : " and is used here";
    if (local.moved == move_state::moved) {
        context.diagnostics.push_back({consumes ? "linear-consumed-twice" : "use-after-move",
            diagnostic_severity::error, at,
            local.name + " was " + verb + " on line " + line + again});
    }
    else {
        context.diagnostics.push_back({consumes ? "linear-consumed-twice" : "use-after-maybe-move",
            diagnostic_severity::error, at,
            local.name + " may have been " + verb + " on line " + line + again});
    }
}

// Experiment `use-after-move`: reports a use of a moved name in a text of
// `statement`, and records the moves and assignments the text makes. A name
// moved and used again on one path through a statement is reported, since
// the order of evaluation is unspecified. Under `linear`, assigning a linear
// value that is not consumed drops it.
auto analyze_moves(const statement& statement, const local_uses& uses, analysis_state& state,
                   const analysis_context& context, bool contract = false) -> void {
    if (!context.follows_moves()) return;
    for (const auto& [entry, local_uses] : uses) {
        auto& local = state.locals.at(entry);
        if (!local.tracks_moves) continue;
        const auto linear = local.linear_type != nullptr;
        auto pointers = std::vector<const local_use*>{};
        for (const auto& use : local_uses) pointers.push_back(&use);
        const auto paths = move_accesses(pointers, linear, contract);
        const auto any = [&](const auto& test) {
            return std::any_of(paths.begin(), paths.end(), test);
        };
        const auto moves = any([](const move_access& path) { return path.moves > 0; });
        if (any([](const move_access& path) { return path.moves + path.reads > 0; })) {
            report_moved_use(*entry, local, statement.span, linear && moves, context);
        }
        if (any([](const move_access& path) {
                return path.moves > 0 && path.moves + path.reads > 1;
            }) &&
            context.first_report(*entry, "same-statement", statement.span)) {
            const auto twice = linear &&
                any([](const move_access& path) { return path.moves > 1; });
            context.diagnostics.push_back({twice ? "linear-consumed-twice" : "use-after-move",
                diagnostic_severity::error, statement.span, local.name +
                (twice ? " is consumed twice in the same statement"
                       : " is moved and used again in the same statement")});
        }
        if (linear && local.moved != move_state::moved &&
            local.initialization == initialization_state::initialized &&
            any([](const move_access& path) { return path.assigned && path.moves == 0; }) &&
            context.first_report(*entry, "linear-overwritten", statement.span)) {
            context.diagnostics.push_back({"linear-not-consumed", diagnostic_severity::error,
                statement.span, local.linear_type->name + " " + local.name +
                " is assigned before it is consumed"});
        }
        // The state after the statement joins its paths.
        auto after = std::optional<move_state>{};
        for (const auto& path : paths) {
            const auto state = path.assigned ? move_state::not_moved
                : path.moves > 0             ? move_state::moved
                                             : local.moved;
            after = !after || *after == state ? state : move_state::maybe_moved;
        }
        if (any([](const move_access& path) { return path.moves > 0 && !path.assigned; })) {
            local.moved_at = statement.span;
        }
        if (std::all_of(paths.begin(), paths.end(), [](const move_access& path) {
                return path.assigned || path.moves > 0; })) {
            local.carried.reset();
        }
        local.moved = *after;
    }
}

auto analyze_statements(const std::vector<std::unique_ptr<statement>>& statements,
                        analysis_state& state, const analysis_context& context, int loop_depth,
                        std::vector<analysis_state>* states = nullptr,
                        const source_span* body_end = nullptr) -> void;

auto report_uninitialized(const local_state& local, std::vector<diagnostic>& diagnostics)
    -> void {
    if (!local.deferred || local.initialization == initialization_state::initialized ||
        local.reported) {
        return;
    }
    diagnostics.push_back({
        "use-before-initialization", diagnostic_severity::error, local.declaration,
        "local variable " + local.name +
            (local.initialization == initialization_state::partial
                ? " must be initialized on every path" : " is never initialized") +
            out_hint(local)});
}

// Reports and removes the locals of `inner` that a nested body declared after `outer`.
auto close_scope(const analysis_state& outer, analysis_state& inner,
                 const analysis_context& context) -> void {
    std::erase_if(inner.locals, [&](const auto& item) {
        if (outer.locals.contains(item.first)) return false;
        report_uninitialized(item.second, context.diagnostics);
        // A path that leaves by a jump was checked at the jump.
        if (!inner.leaves) report_unconsumed(*item.first, item.second, context);
        return true;
    });
}

// Analyzes a nested body from `before`. With `states`, it receives the
// state before each statement and at the end.
auto analyze_branch(const std::vector<std::unique_ptr<statement>>& statements,
                    const analysis_state& before, const analysis_context& context,
                    int loop_depth, std::vector<analysis_state>* states = nullptr)
    -> analysis_state {
    auto result = before;
    analyze_statements(statements, result, context, loop_depth, states);
    close_scope(before, result, context);
    return result;
}

auto leave_function(analysis_state& state, source_span at, const analysis_context& context,
                    bool end = false, const scope_entry* returned_name = nullptr) -> void;

// Experiment `try-operator`: the token ranges of the operands of the `?` in
// `text`.
auto question_operands(const expression_text& text, const expression& node,
                       std::vector<std::pair<std::size_t, std::size_t>>& result) -> void {
    if (node.kind == expression_kind::postfix && text.tokens[node.op].text == "?" &&
        !node.children.empty()) {
        result.emplace_back(node.children.front().first, node.children.front().last);
        return;
    }
    for (const auto& child : node.children) question_operands(text, child, result);
}

// Records the moves of `uses`, the uses of `text`. Under `try-operator`, the
// operands of its `?` are evaluated before the rest of `statement`, and the
// function may return after them.
auto analyze_text_moves(const statement& statement, const parsed_expression& text,
                        local_uses uses, analysis_state& state, const analysis_context& context,
                        bool contract = false) -> void {
    auto operands = std::vector<std::pair<std::size_t, std::size_t>>{};
    if (context.try_operator) question_operands(text.tree(), text.tree().root, operands);
    if (!operands.empty()) {
        auto hoisted = local_uses{};
        for (auto& [entry, list] : uses) {
            const auto first = std::stable_partition(list.begin(), list.end(),
                [&](const local_use& use) {
                    return std::none_of(operands.begin(), operands.end(), [&](const auto& range) {
                        return use.token >= range.first && use.token < range.second;
                    });
                });
            if (first != list.end()) hoisted[entry].assign(first, list.end());
            list.erase(first, list.end());
        }
        analyze_moves(statement, hoisted, state, context, contract);
        auto early = state;
        leave_function(early, statement.span, context);
    }
    analyze_moves(statement, uses, state, context, contract);
}

// Reports `local`, the local of `entry`, passed as `out` twice in `text`.
auto report_repeated_out(const scope_entry& entry, const local_state& local,
                         const std::vector<local_use>& uses, const parsed_expression& text,
                         source_span fallback, const analysis_context& context) -> void {
    const auto* twice = repeated_out(uses);
    if (twice == nullptr) return;
    const auto at = token_span(text, twice->token, fallback);
    if (context.first_report(entry, "out-argument-twice", at)) {
        context.diagnostics.push_back({"out-argument-twice", diagnostic_severity::error, at,
            local.name + " is passed as 'out' more than once in one expression"});
    }
}

// How a statement evaluates a text.
enum class text_kind : std::uint8_t {
    statement,    // an expression statement, where `name = value` may construct
    initializer,  // of a value, which a whole name passes on
    condition,
    other,
};

// Analyzes `text`, which `statement` evaluates at `point`. Outside an
// expression statement, only `out` arguments initialize a deferred local.
auto analyze_expression(const statement& statement, const parsed_expression& text,
                        analysis_state& state, const analysis_context& context, int loop_depth,
                        text_kind kind, scope_point point) -> void {
    const auto constructs = kind == text_kind::statement;
    const auto condition = kind == text_kind::condition;
    const auto uses = uses_of_locals(text, state, context, point, kind == text_kind::initializer);
    analyze_text_moves(statement, text, uses, state, context);
    auto initialized = std::vector<local_state*>{};
    auto positions = std::vector<std::size_t>{};
    for (const auto& [entry, local_uses] : uses) {
        auto& local = state.locals.at(entry);
        report_repeated_out(*entry, local, local_uses, text, statement.span, context);
        if (!is_pending(local)) continue;
        auto access = initialization_access_of(local_uses, text.tree());
        // Initializing a local that is initialized on some paths only reads it.
        const auto reinitialized = local.initialization == initialization_state::partial &&
            !local.conditional_out && (access.assignment || access.out_argument) &&
            access.position <= access.read_position;
        if ((!constructs || local.initialization == initialization_state::partial) &&
            access.assignment) {
            access.assignment = false;
            access.read_at(access.assigned);
        }
        if (local.initialization == initialization_state::partial && access.out_argument) {
            access.out_argument = false;
            access.read_at(access.position);
        }
        const auto at = [&](std::size_t token) { return token_span(text, token, statement.span); };
        const auto read_at = access.read_span.value_or(at(access.read_position));
        if (access.assignment || access.out_argument) {
            if (loop_depth > local.loop_depth) {
                report_once(local, context.diagnostics, "initialization-in-loop", at(access.position),
                    "local variable " + local.name + " cannot be initialized inside a loop");
            }
            else if (local.initialization == initialization_state::uninitialized) {
                initialized.push_back(&local);
                positions.push_back(access.position);
                // A statement analyzed again, in a loop or a defer body, constructs once.
                auto& constructions = context.plan.constructions[statement.id];
                if (access.assignment &&
                    std::none_of(constructions.begin(), constructions.end(),
                                 [&](const construction& value) {
                                     return value.position == access.assigned;
                                 })) {
                    constructions.push_back({access.assigned, local.name});
                }
            }
        }
        if (access.read && reinitialized) {
            report_once(local, context.diagnostics, "use-before-initialization",
                read_at, "local variable " + local.name +
                    " cannot be initialized here, where it is initialized on some paths only");
        }
        else if (access.read) {
            report_once(local, context.diagnostics, "use-before-initialization",
                read_at, "local variable " + local.name + " is used " +
                    (condition ? "in a condition " : "") + "before it was initialized" +
                    out_hint(local));
        }
        else if (access.conditional_out && !access.assignment && !access.out_argument &&
                 local.initialization == initialization_state::uninitialized) {
            local.initialization = initialization_state::partial;
            local.conditional_out = true;
        }
    }
    // Deferred locals other than named return values are initialized in declaration order.
    for (auto index = std::size_t{0}; index < initialized.size(); ++index) {
        const auto* later = initialized[index];
        for (auto& [entry, local] : state.locals) {
            if (!local.deferred || local.named_return ||
                local.initialization == initialization_state::initialized ||
                std::find(initialized.begin(), initialized.end(), &local) != initialized.end() ||
                !before(local.declaration.begin, later->declaration.begin) ||
                !names(context.scopes, entry, local, point)) {
                continue;
            }
            report_once(local, context.diagnostics, "initialization-order",
                token_span(text, positions[index], statement.span),
                "local variable " + local.name + " must be initialized before " + later->name +
                " (local variables must be initialized in the order they are declared)");
        }
    }
    // Locals initialized by one statement are initialized in the order of
    // evaluation, taken as left to right where it is unsequenced.
    const auto initialized_before = [&](std::size_t first, std::size_t second) {
        return evaluated_before(text.tree(), first, second, false) ||
            (first < second && !evaluated_before(text.tree(), second, first, false));
    };
    for (auto later = std::size_t{0}; later < initialized.size(); ++later) {
        for (auto earlier = std::size_t{0}; earlier < initialized.size(); ++earlier) {
            if (initialized[earlier]->named_return || initialized[later]->named_return ||
                !before(initialized[earlier]->declaration.begin,
                        initialized[later]->declaration.begin) ||
                initialized_before(positions[earlier], positions[later])) {
                continue;
            }
            report_once(*initialized[earlier], context.diagnostics, "initialization-order",
                token_span(text, positions[later], statement.span),
                "local variable " + initialized[earlier]->name +
                " must be initialized before " + initialized[later]->name +
                " (local variables must be initialized in the order they are declared)");
        }
    }
    for (auto* local : initialized) local->initialization = initialization_state::initialized;
}

// Reports a read in `texts`, the texts of a contract, of a deferred local
// that is not yet initialized. A contract may not be evaluated, so its `out`
// arguments do not initialize, and its mentions never consume a linear value.
auto check_reads(const statement& statement, const std::vector<const parsed_expression*>& texts,
                 analysis_state& state, const analysis_context& context) -> void {
    const auto point = context.scopes.at(statement);
    // The first read of each local.
    auto reads = std::map<const scope_entry*, source_span>{};
    for (const auto* text : texts) {
        const auto uses = uses_of_locals(*text, state, context, point, false);
        analyze_text_moves(statement, *text, uses, state, context, true);
        for (const auto& [entry, local_uses] : uses) {
            report_repeated_out(*entry, state.locals.at(entry), local_uses, *text,
                                statement.span, context);
            const auto access = initialization_access_of(local_uses, text->tree());
            if (access.read) {
                reads.try_emplace(entry, access.read_span.value_or(
                    token_span(*text, access.read_position, statement.span)));
            }
        }
    }
    for (const auto& [entry, at] : reads) {
        auto& local = state.locals.at(entry);
        if (is_pending(local)) {
            report_once(local, context.diagnostics, "use-before-initialization", at,
                "local variable " + local.name + " is used before it was initialized" +
                    out_hint(local));
        }
    }
}

// Runs the defer bodies registered after the first `registered`, in reverse
// order, on a path that leaves their blocks. While one runs, it and the later
// ones are not registered.
auto run_defers(analysis_state& state, std::size_t registered, const analysis_context& context)
    -> void {
    const auto pending = std::vector(context.defers->begin() + static_cast<std::ptrdiff_t>(registered),
                                     context.defers->end());
    for (auto index = pending.size(); index-- > 0;) {
        context.defers->resize(registered + index);
        const auto& [defer, loop_depth] = pending[index];
        state = analyze_branch(defer->as<statement::block_form>().body, state, context,
                               loop_depth);
    }
    context.defers->insert(context.defers->end(), pending.begin(), pending.end());
}

// A path that returns from the function at `at`, after evaluating the
// returned value: an `out` parameter or named return value must be
// initialized, a named return value is read, the defers run, `return name;`
// consumes `returned_name`, and each linear value must have been consumed.
// The defers see the name intact, since such a return copies a name a defer
// mentions. With `end`, the path falls off the end of the body, and an
// uninitialized one is reported at its declaration.
auto leave_function(analysis_state& state, source_span at, const analysis_context& context,
                    bool end, const scope_entry* returned_name) -> void {
    for (auto& [entry, local] : state.locals) {
        if (local.returned && is_pending(local)) {
            if (end) report_uninitialized(local, context.diagnostics);
            else report_once(local, context.diagnostics, "use-before-initialization", at,
                             local.name + " must be initialized before 'return'" +
                                 out_hint(local));
            local.reported = true;
        }
        if (local.named_return && local.tracks_moves && context.follows_moves()) {
            report_moved_use(*entry, local, at, false, context);
        }
    }
    run_defers(state, 0, context);
    if (const auto found = state.locals.find(returned_name);
        found != state.locals.end() && found->second.tracks_moves) {
        found->second.moved = move_state::moved;
        found->second.moved_at = at;
    }
    report_unconsumed(state, context, 0);
    state.leaves = true;
}

// The paths of one pass over a loop: to its next iteration, and out of it.
struct loop_paths final {
    analysis_state next;
    std::vector<analysis_state> exits;
};

// Analyzes an iteration of `loop`, the form of `statement`, from `entry`.
auto analyze_iteration(const statement& statement, const statement::loop_form& loop,
                       analysis_state entry, const analysis_context& context, int loop_depth)
    -> loop_paths {
    const auto point = context.scopes.at(statement);
    context.loops->push_back({loop.label, loop_depth + 1, context.defers->size(), {}, {}});
    if (statement.kind == statement_kind::while_statement) {
        analyze_expression(statement, loop.condition, entry, context, loop_depth + 1,
                           text_kind::other, point);
    }
    auto body = entry;
    if (loop.range_parameter) {
        if (const auto* parameter = context.scopes.declared_as(*loop.range_parameter)) {
            declare(body, *parameter, loop_depth + 1, context);
        }
    }
    declare_bindings(body, loop.bindings, loop_depth + 1, context);
    analyze_statements(loop.body, body, context, loop_depth + 1);
    auto frame = std::move(context.loops->back());
    context.loops->pop_back();
    auto next = std::vector<const analysis_state*>{&body};
    for (const auto& path : frame.continues) next.push_back(&path);
    auto result = loop_paths{join(body, next), std::move(frame.breaks)};
    analyze_expression(statement, loop.next, result.next, context, loop_depth + 1,
                       text_kind::other, point);
    if (statement.kind == statement_kind::do_statement) {
        analyze_expression(statement, loop.condition, result.next, context, loop_depth + 1,
                           text_kind::other, point);
    }
    close_scope(entry, body, context);
    // A while loop's condition may end it before its body, and a for loop
    // may have no iteration; a do loop runs its body at least once.
    if (statement.kind != statement_kind::do_statement) result.exits.push_back(std::move(entry));
    result.exits.push_back(result.next);
    return result;
}

// Analyzes `statements` from `state`. With `states`, it receives the state
// before each statement and at the end. With `body_end`, they are a function
// body, whose end returns.
auto analyze_statements(const std::vector<std::unique_ptr<statement>>& statements,
                        analysis_state& state, const analysis_context& context, int loop_depth,
                        std::vector<analysis_state>* states, const source_span* body_end)
    -> void {
    const auto registered = context.defers->size();
    for (const auto& statement_ptr : statements) {
        const auto& statement = *statement_ptr;
        if (states != nullptr) states->push_back(state);
        const auto point = context.scopes.at(statement);
        // A defer body runs when a path leaves the block.
        if (statement.kind == statement_kind::defer_statement) {
            context.defers->emplace_back(&statement, loop_depth);
            continue;
        }
        const auto* declaration_form = std::get_if<statement::declaration_form>(&statement.form);
        if (declaration_form && declaration_form->declaration) {
            const auto& declaration = *declaration_form->declaration;
            if (declaration.kind != declaration_kind::object || declaration.is_alias) continue;
            if (!declaration.initializer.empty()) {
                analyze_expression(statement, declaration.initializer, state,
                                   context, loop_depth, text_kind::initializer, point);
            }
            // A let-else `else` block leaves the scope.
            if (declaration_form->is_let_else) {
                static_cast<void>(analyze_branch(declaration_form->else_body, state, context,
                                                 loop_depth));
            }
            if (const auto* entry = context.scopes.declared_by(statement)) {
                declare(state, *entry, loop_depth, context);
            }
        }
        else if (const auto* destructuring =
                     std::get_if<statement::destructuring_form>(&statement.form)) {
            analyze_expression(statement, destructuring->initializer, state,
                               context, loop_depth, text_kind::other, point);
            if (destructuring->is_let_else) {
                static_cast<void>(analyze_branch(destructuring->else_body, state, context,
                                                 loop_depth));
            }
            declare_bindings(state, destructuring->bindings, loop_depth, context);
        }
        else if (const auto* block = std::get_if<statement::block_form>(&statement.form)) {
            // The parameters are locals of the body.
            const auto outer = block->parameters.empty() ? analysis_state{} : state;
            for (const auto& parameter : block->parameters) {
                const auto at = context.scopes.at(parameter);
                const auto passing = parameter.value.passing;
                analyze_expression(statement, parameter.initializer, state, context, loop_depth,
                    passing == passing_style::copy || passing == passing_style::move ||
                            passing == passing_style::forward
                        ? text_kind::initializer : text_kind::other,
                    at);
                if (const auto* entry = context.scopes.declared_as(parameter.value)) {
                    declare(state, *entry, loop_depth, context);
                }
            }
            // A handler starts from the join of the states before and after
            // each statement of the try block.
            auto steps = std::vector<analysis_state>{};
            auto paths = std::vector<analysis_state>{};
            paths.push_back(analyze_branch(block->body, state, context, loop_depth,
                                           block->handlers.empty() ? nullptr : &steps));
            // A throw from an expression statement undoes its initializations:
            // an assignment constructs last, and an `out` argument is destroyed
            // when its expression throws. So after a last such statement, only
            // its moves reach a handler.
            if (!block->handlers.empty() && !block->body.empty() &&
                (block->body.back()->kind == statement_kind::expression ||
                 block->body.back()->kind == statement_kind::discard_statement)) {
                const auto& before_last = steps[steps.size() - 2];
                for (auto& [entry, local] : steps.back().locals) {
                    const auto before = before_last.locals.find(entry);
                    if (before == before_last.locals.end()) continue;
                    local.initialization = before->second.initialization;
                    local.conditional_out = before->second.conditional_out;
                }
            }
            auto thrown = std::vector<const analysis_state*>{&state};
            for (const auto& step : steps) thrown.push_back(&step);
            const auto throwing = join(state, thrown);
            for (const auto& handler : block->handlers) {
                auto caught = throwing;
                if (const auto* entry = context.scopes.declared_as(handler.caught)) {
                    declare(caught, *entry, loop_depth, context);
                }
                paths.push_back(analyze_branch(handler.body, caught, context, loop_depth));
                close_scope(state, paths.back(), context);
            }
            auto reached = std::vector<const analysis_state*>{};
            for (const auto& path : paths) reached.push_back(&path);
            state = join(state, reached);
            if (!block->parameters.empty()) close_scope(outer, state, context);
        }
        else if (const auto* loop = std::get_if<statement::loop_form>(&statement.form)) {
            // A range is evaluated once, before the loop.
            if (statement.kind == statement_kind::for_statement) {
                analyze_expression(statement, loop->condition, state, context, loop_depth,
                                   text_kind::other, point);
            }
            auto paths = analyze_iteration(statement, *loop, state, context, loop_depth);
            // A second pass, from the paths that reach the loop and its next
            // iteration, finds the uses of a move made in the previous one. A
            // pass reaches the state that more passes would, since each name
            // is either set on a path or left as it was, so inside the second
            // pass of an enclosing loop, whose first pass ran this loop's
            // second, one pass is enough.
            auto carries = false;
            if (context.follows_moves() && context.loop_diagnostics == nullptr) {
                for (auto& [entry, before] : state.locals) {
                    auto& local = paths.next.locals.at(entry);
                    if (local.tracks_moves && local.moved != move_state::not_moved &&
                        (before.moved == move_state::not_moved ||
                         !same_position(before.moved_at, local.moved_at))) {
                        local.carried = local.moved_at;
                        carries = true;
                    }
                }
            }
            if (carries) {
                auto dropped = std::vector<diagnostic>{};
                const auto again = analysis_context{dropped, context.plan, context.scopes,
                    context.expressions, context.callees, context.use_after_move,
                    context.try_operator, context.loops, context.defers, context.linear_types,
                    context.reported, &context.diagnostics};
                paths = analyze_iteration(statement, *loop, join(state, {&state, &paths.next}),
                                          again, loop_depth);
            }
            auto exits = std::vector<const analysis_state*>{};
            for (const auto& path : paths.exits) exits.push_back(&path);
            auto after = join(state, exits);
            for (auto& [entry, local] : after.locals) local.carried = state.locals.at(entry).carried;
            state = std::move(after);
        }
        else if (const auto* branch = std::get_if<statement::if_form>(&statement.form)) {
            analyze_expression(statement, branch->condition, state, context, loop_depth,
                               text_kind::condition, point);
            auto bound = state;
            declare_bindings(bound, branch->bindings, loop_depth, context);
            auto then_state = analyze_branch(branch->body, bound, context, loop_depth);
            close_scope(state, then_state, context);
            const auto else_state = analyze_branch(branch->else_body, state, context, loop_depth);
            check_branch_initialization(state, then_state.leaves ? else_state : then_state,
                                        else_state.leaves ? then_state : else_state,
                                        statement.span, context.diagnostics);
            state = join(state, {&then_state, &else_state});
        }
        else if (statement.kind == statement_kind::expression ||
                 statement.kind == statement_kind::discard_statement) {
            const auto& text = statement.as<statement::expression_form>().text;
            analyze_expression(statement, text, state, context, loop_depth, text_kind::statement,
                               point);
            const auto& tokens = text.tree().tokens;
            // A path that throws needs no local initialized.
            if (!tokens.empty() && tokens.front().text == "throw") {
                state.leaves = true;
                for (auto& [entry, local] : state.locals) {
                    local.initialization = initialization_state::initialized;
                }
            }
        }
        else if (statement.kind == statement_kind::break_statement ||
                 statement.kind == statement_kind::continue_statement) {
            if (!context.loops->empty()) {
                const auto& label = statement.as<statement::jump_form>().label;
                auto target = std::find_if(context.loops->rbegin(), context.loops->rend(),
                    [&](const loop_frame& frame) { return frame.label == label; });
                if (label.empty() || target == context.loops->rend()) {
                    target = context.loops->rbegin();
                }
                run_defers(state, target->defers, context);
                (statement.kind == statement_kind::break_statement ? target->breaks
                                                                   : target->continues)
                    .push_back(state);
                report_unconsumed(state, context, target->depth);
            }
            state.leaves = true;
        }
        else if (const auto* contract = std::get_if<statement::contract_form>(&statement.form)) {
            const auto group = contract_group_text(*contract);
            auto texts = std::vector<const parsed_expression*>{&group, &contract->condition,
                                                               &contract->message};
            for (const auto& flag : contract->flags) texts.push_back(&flag);
            check_reads(statement, texts, state, context);
        }
        else if (statement.kind == statement_kind::return_statement) {
            const auto& text = statement.as<statement::expression_form>().text;
            analyze_expression(statement, text, state, context, loop_depth,
                               text_kind::initializer, point);
            const auto* value = whole_expression(text.tree());
            leave_function(state, statement.span, context, false,
                           value != nullptr && is_single_name(*value)
                               ? context.scopes.lookup(text.tree().tokens[value->first].text, point)
                               : nullptr);
        }
    }
    if (states != nullptr) states->push_back(state);
    if (!state.leaves) {
        if (body_end != nullptr) leave_function(state, *body_end, context, true);
        else run_defers(state, registered, context);
    }
    context.defers->resize(registered);
}

auto report_uninitialized(const analysis_state& state, std::vector<diagnostic>& diagnostics)
    -> void {
    for (const auto& [entry, local] : state.locals) report_uninitialized(local, diagnostics);
}

// An expression body, evaluated once, initializes the `out` parameters it
// assigns or passes as `out` arguments on every path, and must not read one
// before.
auto analyze_expression_body(const ir_declaration& declaration, analysis_state& state,
                             const analysis_context& context) -> void {
    const auto point = context.scopes.inside(declaration.symbol);
    for (const auto& [entry, uses] : uses_of_locals(declaration.body, state, context, point,
                                                    false)) {
        auto& local = state.locals.at(entry);
        report_repeated_out(*entry, local, uses, declaration.body, declaration.span, context);
        if (!is_pending(local)) continue;
        const auto access = initialization_access_of(uses, declaration.body.tree());
        if (access.read) {
            report_once(local, context.diagnostics, "use-before-initialization",
                        access.read_span.value_or(
                            token_span(declaration.body, access.read_position, declaration.span)),
                        "local variable " + local.name + " is used before it was initialized" +
                            out_hint(local));
        }
        else if (access.assignment || access.out_argument) {
            local.initialization = initialization_state::initialized;
        }
        else if (access.conditional_out) {
            local.conditional_out = true;
        }
    }
}

// Reports a read of an `out` parameter or named return value in what the
// function evaluates on entry, after declaring its named return values: a
// precondition and the `x$` captures of a postcondition.
auto check_entry_contracts(const ir_declaration& declaration, analysis_state& state,
                           const analysis_context& context) -> void {
    auto point = context.scopes.inside(declaration.symbol);
    const auto none = statement_list{};
    for (const auto& statement : declaration.function_statements
                                     ? declaration.function_statements->statements : none) {
        const auto* form = std::get_if<statement::declaration_form>(&statement->form);
        if (form == nullptr || !form->declaration || !form->declaration->is_named_return) break;
        if (const auto* entry = context.scopes.declared_by(*statement)) {
            declare(state, *entry, 0, context);
        }
        point = context.scopes.at(*statement);
    }
    for (const auto& contract : declaration.contracts) {
        if (contract.group == "unevaluated") continue;
        auto texts = std::vector<const parsed_expression*>{};
        if (contract.is_post) {
            for (const auto& capture : contract.captures) texts.push_back(&capture.expression);
        }
        else {
            texts = {&contract.condition, &contract.message};
            for (const auto& flag : contract.flags) texts.push_back(&flag);
        }
        const auto group = contract_group_text(contract);
        texts.push_back(&group);
        for (const auto* text : texts) {
            for (const auto& [entry, uses] : uses_of_locals(*text, state, context, point, false)) {
                auto& local = state.locals.at(entry);
                report_repeated_out(*entry, local, uses, *text, declaration.span, context);
                const auto access = initialization_access_of(uses, text->tree());
                if (access.read && is_pending(local)) {
                    report_once(local, context.diagnostics, "use-before-initialization",
                                access.read_span.value_or(
                                    token_span(*text, access.read_position, declaration.span)),
                                "local variable " + local.name +
                                    " is used before it was initialized");
                }
            }
        }
    }
}


// The name a returned value refers into: the leading name of a chain of member
// accesses, subscripts, calls and postfix operators, through parentheses, up to
// a postfix `*`, which dereferences and so leaves the name.
auto returned_name(const parsed_expression& text) -> std::string_view {
    const auto& tree = text.tree();
    const auto* value = whole_expression(tree);
    while (value != nullptr &&
           (value->kind == expression_kind::call || value->kind == expression_kind::subscript ||
            value->kind == expression_kind::member ||
            (value->kind == expression_kind::postfix && tree.tokens[value->op].text != "*") ||
            (value->kind == expression_kind::group && value->children.size() == 1))) {
        value = &value->children.front();
    }
    if (value == nullptr || value->kind != expression_kind::name ||
        tree.tokens[value->first].kind != expression_token_kind::identifier) {
        return {};
    }
    return tree.tokens[value->first].text;
}

// Whether a value returned from `function` that leads with `name` refers into
// an object the function owns: a local or a `copy` or `move` parameter, and,
// with `reference`, an `in` parameter, which may be passed by value. `at` is
// the return statement, or null for an expression body, which has no locals.
auto refers_into_function(std::string_view name, const ir_declaration& function,
                          const scope_model& scopes, const statement* at, bool reference)
    -> bool {
    if (name.empty()) return false;
    const auto* parameter = scopes.parameter_of(function.symbol, name);
    const auto* found = at == nullptr ? parameter : scopes.lookup(name, scopes.at(*at));
    if (found == nullptr) return false;
    if (found == parameter) {
        const auto passing = found->declared_parameter->passing;
        return (reference && passing == passing_style::in) || passing == passing_style::copy ||
            passing == passing_style::move;
    }
    return found->kind == scope_model::entry_kind::object &&
        found->declaring_statement != nullptr && !found->is_static_or_alias;
}

// The forwarding returns of `function` that refer into it: an error when the
// return type is a reference, `-> forward T` or `-> forward_ref _`, and
// otherwise, for `-> forward _`, a text whose type must not be a reference
// (a by-value `in` parameter then returns a value).
auto check_forward_returns(const statement_list& statements, const ir_declaration& function,
                           const scope_model& scopes, bool reference,
                           initialization_plan& plan, std::vector<diagnostic>& diagnostics)
    -> void {
    for (const auto& statement_ptr : statements) {
        const auto& statement = *statement_ptr;
        if (statement.kind == statement_kind::return_statement &&
            refers_into_function(returned_name(statement.as<statement::expression_form>().text),
                                 function, scopes, &statement, reference)) {
            if (reference) {
                diagnostics.push_back({"forward-return-reference", diagnostic_severity::error,
                    statement.span, std::string{forward_return_message}});
            }
            else {
                plan.value_returns.insert(statement_key(statement));
            }
        }
        for_each_nested_list(statement, [&](const statement_list& nested) {
            check_forward_returns(nested, function, scopes, reference, plan, diagnostics);
        });
    }
}

auto is_star_run(std::string_view text) -> bool {
    return !text.empty() && text.find_first_not_of('*') == std::string_view::npos;
}

auto is_assignment_operator(std::string_view text) -> bool {
    return text == "=" || text == "+=" || text == "-=" || text == "*=" || text == "/=" ||
        text == "%=" || text == "<<=" || text == ">>=" || text == "&=" || text == "|=" ||
        text == "^=";
}

auto is_null_or_integer(const token& value) -> bool {
    return value.text == "nullptr" || value.kind == token_kind::integer_literal;
}

// cppfront's partial pointer-safety rules: a name is a pointer when it is
// declared with a `*` type, returns one, or is deduced from `x&`, `x*` or a
// pointer-valued name or call. Names are the program's resolved references.
class pointer_checker final {
public:
    pointer_checker(const ir_program& program, const token_list& tokens)
        : program_{program}, tokens_{tokens} {
        for (auto index = std::size_t{0}; index < tokens.size(); ++index) {
            token_index_[key(tokens[index].span.begin)] = index;
        }
        for (const auto& reference : program.references) {
            references_.try_emplace(key(reference.span.begin), reference.symbol);
        }
    }

    auto check() -> std::vector<diagnostic> {
        for (const auto& value : program_.symbols) {
            if (value.kind == declaration_kind::object && !value.name.empty()) {
                check_null_initializer(value);
            }
        }
        for (auto index = std::size_t{0}; index < tokens_.size(); ++index) {
            check_use(index);
        }
        return std::move(diagnostics_);
    }

private:
    struct shape final {
        int stars = 0;
        // A deduced type: the initializer's leading name and how it is used.
        const symbol* source = nullptr;
        int dereferences = 0;
        int addresses = 0;
    };

    static auto key(const source_position& position) -> std::uint64_t {
        return (std::uint64_t{position.line} << 32) | position.column;
    }

    auto text(std::size_t index) const -> std::string_view {
        return index < tokens_.size() ? std::string_view{tokens_[index].text} : "";
    }

    // Whether the value beginning at `index` is null or an integer: `nullptr`,
    // an integer literal or `()`, in any number of grouping parentheses.
    auto is_null_value(std::size_t index) const -> bool {
        auto depth = std::size_t{0};
        while (text(index + depth) == "(") ++depth;
        if (depth > 0 && text(index + depth) == ")") return true;
        if (index + depth >= tokens_.size() || !is_null_or_integer(tokens_[index + depth])) {
            return false;
        }
        for (auto close = std::size_t{1}; close <= depth; ++close) {
            if (text(index + depth + close) != ")") return false;
        }
        return true;
    }

    auto name_token(const symbol& value) const -> std::optional<std::size_t> {
        const auto found = token_index_.find(key(value.span.begin));
        if (found == token_index_.end()) return std::nullopt;
        for (auto index = found->second; index < tokens_.size() && index < found->second + 4;
             ++index) {
            if (tokens_[index].text == value.name) return index;
        }
        return std::nullopt;
    }

    // The object, function or parameter token `index` names.
    auto referenced(std::size_t index) const -> const symbol* {
        const auto found = references_.find(key(tokens_[index].span.begin));
        if (found == references_.end()) return nullptr;
        const auto* value = find_symbol(program_, found->second);
        if (value == nullptr || (value->kind != declaration_kind::object &&
                                 value->kind != declaration_kind::function)) {
            return nullptr;
        }
        return value;
    }

    // The data member `index` names in `object.member`, when `object` is
    // declared with a type that is one type with that member.
    auto member_referenced(std::size_t index) const -> const symbol* {
        const auto* object = referenced(index - 2);
        if (object == nullptr || object->kind != declaration_kind::object ||
            object->type.kind != type_kind::name) {
            return nullptr;
        }
        const symbol* result = nullptr;
        for (const auto& member : program_.symbols) {
            const auto* type = find_symbol(program_, member.parent);
            if (member.kind != declaration_kind::object || member.name != text(index) ||
                type == nullptr || type->kind != declaration_kind::type ||
                type->name != object->type.text) {
                continue;
            }
            if (result != nullptr) return nullptr;
            result = &member;
        }
        return result;
    }

    auto shape_of(const symbol& value) const -> shape {
        auto result = shape{};
        if (value.kind == declaration_kind::function ||
            (!value.type.empty() && !value.type.is_cpp1_auto())) {
            result.stars = pointer_depth(value.type);
            return result;
        }
        const auto name = name_token(value);
        if (!name) return result;
        auto index = *name + 1;
        if (text(index) == ":=") {
            ++index;
        }
        else {
            if (text(index) != ":" || text(index + 1) != "=") return result;
            index += 2;
        }
        if (index >= tokens_.size()) return result;
        const auto& first = tokens_[index];
        if (first.kind == token_kind::pointer_dereference) {
            result.source = referenced(index);
            result.dereferences = 1;
            for (auto next = index + 1; is_star_run(text(next)); ++next) {
                result.dereferences += static_cast<int>(text(next).size());
            }
        }
        else if (first.kind == token_kind::identifier) {
            result.source = referenced(index);
            if (text(index + 1) == "&" && (text(index + 2) == ";" || index + 2 >= tokens_.size())) {
                result.addresses = 1;
            }
        }
        return result;
    }

    auto is_pointer(const symbol* value, int dereferences, int addresses, int depth = 0)
        -> bool {
        if (addresses > dereferences) return true;
        if (value == nullptr || depth > 16) return false;
        const auto found = shapes_.find(value);
        const auto& value_shape = found != shapes_.end()
            ? found->second : shapes_.emplace(value, shape_of(*value)).first->second;
        if (value_shape.source != nullptr) {
            const auto* source = value_shape.source;
            return is_pointer(source, dereferences + value_shape.dereferences,
                              addresses + value_shape.addresses, depth + 1);
        }
        return value_shape.stars + addresses - dereferences > 0;
    }

    auto add(std::size_t index, std::string code, std::string message) -> void {
        diagnostics_.push_back({std::move(code), diagnostic_severity::error,
                                tokens_[index].span, std::move(message)});
    }

    auto check_null_initializer(const symbol& value) -> void {
        const auto name = name_token(value);
        if (!name || text(*name + 1) != ":" || pointer_depth(value.type) == 0) return;
        auto index = *name + 2;
        for (auto depth = 0; index < tokens_.size(); ++index) {
            if (text(index) == "(" || text(index) == "[" || text(index) == "{") ++depth;
            else if (text(index) == ")" || text(index) == "]" || text(index) == "}") {
                if (--depth < 0) return;
            }
            else if (depth == 0 && (text(index) == ";" || text(index) == ",")) return;
            else if (depth == 0 && text(index) == "=") break;
        }
        ++index;
        if (index >= tokens_.size()) return;
        if (is_null_value(index)) {
            add(index, "pointer-null-initialization",
                "pointer cannot be initialized to null or int - leave it uninitialized and "
                "then set it to a non-null value when you have one");
        }
    }

    auto check_use(std::size_t index) -> void {
        const auto& value = tokens_[index];
        if (value.kind != token_kind::identifier) return;
        // A member named through `this.` is checked like the bare name, and
        // one named through an object like the member of its type.
        const auto through_this =
            index > 1 && text(index - 1) == "." && text(index - 2) == "this";
        const auto through_object = index > 1 && text(index - 1) == "." && !through_this;
        if (index > 0 && !through_this && !through_object &&
            (text(index - 1) == "." || text(index - 1) == ".." || text(index - 1) == "->")) {
            return;
        }
        if (text(index + 1) == ":" || text(index + 1) == ":=") return;
        const auto* found = through_object ? member_referenced(index) : referenced(index);
        if (found == nullptr || !is_pointer(found, 0, 0)) return;
        // The whole right operand of a binary `+`, after an operand that is
        // not a pointer, as in `1 + p`.
        auto start = through_this || through_object ? index - 2 : index;
        while (start > 1 && text(start - 1) == "::") start -= 2;
        const auto next = text(index + 1);
        const auto whole = next != "." && next != ".." && next != "(" && next != "[" &&
            next != "->" && next != "*" && next != "&" && next != "$" && next != "::" &&
            next != "++" && next != "--";
        if (whole && start > 1 && text(start - 1) == "+" &&
            (tokens_[start - 2].kind == token_kind::identifier ||
             tokens_[start - 2].kind == token_kind::integer_literal ||
             text(start - 2) == ")" || text(start - 2) == "]")) {
            const auto* left = tokens_[start - 2].kind == token_kind::identifier
                ? referenced(start - 2) : nullptr;
            if (left == nullptr || !is_pointer(left, 0, 0)) {
                add(start - 1, "pointer-arithmetic",
                    "+ - pointer arithmetic is illegal - use std::span or gsl::span instead");
            }
        }
        auto op = index + 1;
        const auto called = text(op) == "(";
        if (called) op = detail::bracket_end(tokens_, op, tokens_.size()) + 1;
        const auto operation = text(op);
        if (operation == "++" || operation == "--" || operation == "[") {
            add(op, "pointer-arithmetic", std::string{operation} +
                " - pointer arithmetic is illegal - use std::span or gsl::span instead");
        }
        else if (operation == "~") {
            add(op, "pointer-bitwise", std::string{operation} +
                " - pointer bitwise manipulation is illegal - use std::bit_cast to convert to "
                "raw bytes first");
        }
        else if (called) {
            return;
        }
        else if (is_assignment_operator(operation) && is_null_value(op + 1)) {
            add(op, "pointer-null-assignment", std::string{operation} +
                " - pointer assignment from null or integer is illegal");
        }
        else if (operation == "+" || operation == "+=" || operation == "-" || operation == "-=") {
            add(op, "pointer-arithmetic", std::string{operation} +
                " - pointer arithmetic is illegal - use std::span or gsl::span instead");
        }
    }

    const ir_program& program_;
    const token_list& tokens_;
    std::unordered_map<std::uint64_t, std::size_t> token_index_;
    std::unordered_map<std::uint64_t, symbol_id> references_;
    std::unordered_map<const symbol*, shape> shapes_;
    std::vector<diagnostic> diagnostics_;
};


// A mention of a name in an expression text.
struct use_site final {
    // The text holding the mention: a capture has its own text.
    text_key text;
    std::vector<std::size_t> path;
    std::string name;
    // The declaration the mention names.
    const scope_entry* target = nullptr;
    // Inside a fold or the operand of a pack expansion `x...`, where the
    // mention may be evaluated more than once, so it is never moved from
    // unless it names the pack expanded.
    bool expanded = false;
    // The function name of a UFCS call `x.f()`, which names no member of `this`.
    bool function_name = false;
    // In a compile-time context, where a mention is not a use.
    bool compile_time = false;
    // A use that is not moved: with experiment `move-only-to-known-callees`,
    // a call argument not known to be taken by value or rvalue reference; a
    // pattern test's subject, or a mention of a name its bindings refer to.
    bool unmoved_argument = false;
    // The inspect alternatives whose value holds the mention; see `mention`.
    std::vector<std::pair<const expression*, const expression*>> alternatives;
};

// Whether at most one of two mentions is evaluated: they are in the values of
// different alternatives of one inspect.
auto exclusive(const use_site& a, const use_site& b) -> bool {
    return std::ranges::any_of(a.alternatives, [&](const auto& first) {
        return std::ranges::any_of(b.alternatives, [&](const auto& second) {
            return first.first == second.first && first.second != second.second;
        });
    });
}

// The subject of the pattern test in `text` that binds `name` (experiment
// `condition-bindings`), and whether it binds it by `as`, or null.
auto bound_subject(const expression_text& text, const expression& node, std::string_view name)
    -> std::pair<const expression*, bool> {
    if (node.kind == expression_kind::pattern_test) {
        const auto parts = inspect_alternative_of(text.tokens, node.children[1]);
        if (parts.binding != nullptr && text.tokens[parts.binding->first].text == name) {
            return {&node.children[0], true};
        }
        if (parts.destructuring != nullptr &&
            std::any_of(parts.destructuring->children.begin(),
                        parts.destructuring->children.end(), [&](const expression& item) {
                            return text.tokens[item.first].text == name;
                        })) {
            return {&node.children[0], false};
        }
    }
    for (const auto& child : node.children) {
        if (const auto found = bound_subject(text, child, name); found.first != nullptr) {
            return found;
        }
    }
    return {nullptr, false};
}

// The condition of the `if` or `while` statement `value`, or null.
auto condition_of(const statement& value) -> const parsed_expression* {
    if (const auto* form = std::get_if<statement::if_form>(&value.form)) return &form->condition;
    if (const auto* form = std::get_if<statement::loop_form>(&value.form)) return &form->condition;
    return nullptr;
}

// The initializer `value as T` of the let-else declaration `value`
// (experiment `let-else`), or null.
auto let_else_initializer(const statement& value) -> const parsed_expression* {
    if (const auto* form = std::get_if<statement::declaration_form>(&value.form);
        form && form->is_let_else && form->declaration) {
        return &form->declaration->initializer;
    }
    if (const auto* form = std::get_if<statement::destructuring_form>(&value.form);
        form && form->is_let_else) {
        return &form->initializer;
    }
    return nullptr;
}

struct last_use_candidate final {
    const scope_entry* target = nullptr;
    last_use_kind kind = last_use_kind::move;
    // `return name;` of an initialized local or a `copy` or `move` parameter is
    // left to Cpp1's implicit move.
    bool initialized_local = false;
    // For a `move this` receiver, the type whose members are named without `this.`.
    const scope_entry* type = nullptr;
    // Mentioned by a postcondition, which is checked after the body's statements
    // but before a `return` statement's value.
    bool in_postcondition = false;
};

// Finds the definite last uses of one candidate at a time, scanning backward
// from the end of its scope and tracking whether the name may be mentioned
// later (it is "live"). A mention where it is not live is a definite last use.
// A `return`, `throw`, `break` or `continue` leaves the scope, so the name is
// not live before it; nothing in a loop is a last use; an `if` condition is
// live when either branch is; a `try` body is live wherever a handler is.
class last_use_finder final {
public:
    last_use_finder(const ir_program& program, const scope_model& scopes,
                    const known_callees& callees, initialization_plan& plan,
                    const experiment_set& experiments)
        : program_{program}, scopes_{scopes}, callees_{callees}, plan_{plan},
          move_only_to_known_callees_{experiments.enabled("move-only-to-known-callees")} {
        for (const auto& declaration : program.declarations) {
            function_expressions_.emplace(declaration.name, declaration.symbol);
        }
    }

    auto find(const last_use_candidate& candidate, const statement_list& statements,
              std::size_t begin) -> void {
        candidate_ = &candidate;
        static_cast<void>(scan(statements, begin, false));
    }

    // Records the last uses in `output` instead of the plan.
    auto find(const last_use_candidate& candidate, const statement_list& statements,
              last_use_map& output) -> void {
        output_ = &output;
        find(candidate, statements, 0);
        output_ = &plan_.last_uses;
    }

    auto find(const last_use_candidate& candidate, const text_key& key,
              const parsed_expression& text, scope_point point) -> void {
        candidate_ = &candidate;
        static_cast<void>(scan_text(key, text, point, false));
    }

private:
    const ir_program& program_;
    const scope_model& scopes_;
    const known_callees& callees_;
    initialization_plan& plan_;
    last_use_map* output_ = &plan_.last_uses;
    bool move_only_to_known_callees_;
    std::unordered_map<std::string, symbol_id> function_expressions_;
    const last_use_candidate* candidate_ = nullptr;
    // Whether the name is live after a `return`, `throw`, `break` or
    // `continue`: inside a loop, or in a `try` body whose handler is live.
    bool live_at_exit_ = false;
    // The mentions found, so that a probe can tell whether it found one.
    std::size_t mentions_found_ = 0;
    // Set while scanning a `return` statement.
    bool in_return_ = false;
    std::map<text_key, std::vector<use_site>> sites_;
    // For each statement list, the statements that may mention each declaration.
    std::unordered_map<const statement_list*,
                       std::unordered_map<const scope_entry*, std::vector<std::size_t>>>
        mentions_;
    // For each statement list, the indices of its defer statements.
    std::unordered_map<const statement_list*, std::vector<std::size_t>> defers_;
    std::unordered_map<const statement_list*, std::size_t> exits_;
    // Set while collecting the declarations a statement mentions.
    std::vector<const scope_entry*>* collecting_ = nullptr;

    // The mentions in a text at `point`, each with the declaration it names.
    auto collect(const expression_text& parsed, const text_key& key, scope_point point,
                 std::vector<use_site>& output) -> void {
        for_each_mention(parsed, [&](const mention& value) {
            const auto first = output.size();
            if (is_function_expression_name(value.name())) {
                add_captures(value.name(), output);
            }
            else {
                output.push_back({key, value.path, value.name(),
                                  scopes_.lookup(value.name(), point), value.expanded,
                                  value.function_name, false,
                                  value.subject ||
                                      (move_only_to_known_callees_ && unmoved(value, point)),
                                  value.alternatives});
                if (output.back().target != nullptr) {
                    add_referents(*output.back().target, key, output, 0);
                }
            }
            if (value.compile_time) {
                for (auto site = first; site < output.size(); ++site) output[site].compile_time = true;
            }
        });
    }

    // Whether a mention is a whole argument of a call whose callee is not
    // known to take it by value or rvalue reference: a function of this
    // program whose viable overloads all have a `copy`, `move` or `forward`
    // parameter there. The receiver of a UFCS call `x.f()` is its first
    // argument, which a `move this` member function takes by value.
    [[nodiscard]] auto unmoved(const mention& value, scope_point point) const -> bool {
        return value.call != nullptr && !callees_.passes_by_value(value, point);
    }

    // A binding refers to its pattern test's subject, so a mention of it is
    // also an unmoved use of the names the subject mentions.
    auto add_referents(const scope_entry& binding, const text_key& key,
                       std::vector<use_site>& output, int depth) -> void {
        const auto* declaring = binding.declaring_statement;
        const auto* condition = declaring != nullptr ? condition_of(*declaring) : nullptr;
        const auto* initializer = declaring != nullptr ? let_else_initializer(*declaring)
                                                       : nullptr;
        if ((condition == nullptr && initializer == nullptr) || depth > 8) return;
        const auto& tree = (condition != nullptr ? condition : initializer)->tree();
        // A let-else name refers into the value its initializer casts.
        const auto* subject = condition != nullptr
            ? bound_subject(tree, tree.root, binding.name).first
            : &tree.root.children.front().children[0];
        if (subject == nullptr) return;
        const auto point = scopes_.at(*declaring);
        for_each_mention(tree, [&](const mention& value) {
            if (value.token < subject->first || value.token >= subject->last) return;
            const auto* target = scopes_.lookup(value.name(), point);
            output.push_back({key, value.path, value.name(), target, false, false, false, true});
            if (target != nullptr) add_referents(*target, key, output, depth + 1);
        });
    }

    // A function expression's captures are mentions where it appears.
    auto add_captures(std::string_view name, std::vector<use_site>& output) -> void {
        const auto found = std::find_if(program_.function_expressions.begin(),
            program_.function_expressions.end(),
            [&](const auto& expression) { return expression.name == name; });
        const auto symbol = function_expressions_.find(std::string{name});
        if (found == program_.function_expressions.end() ||
            symbol == function_expressions_.end()) {
            return;
        }
        const auto point = scopes_.outside(symbol->second);
        for (auto index = std::size_t{0}; index < found->captures.size(); ++index) {
            const auto& captured = sites({symbol->second, text_key::part::capture,
                                          static_cast<std::uint32_t>(index)},
                                         found->captures[index].expression.tree(), point);
            output.insert(output.end(), captured.begin(), captured.end());
        }
    }

    auto sites(const text_key& key, const expression_text& parsed, scope_point point)
        -> const std::vector<use_site>& {
        if (const auto found = sites_.find(key); found != sites_.end()) return found->second;
        auto& result = sites_[key];
        collect(parsed, key, point, result);
        return result;
    }

    [[nodiscard]] auto matches(const use_site& site) const -> bool {
        if (site.target == nullptr) return false;
        if (site.target == candidate_->target) return true;
        return candidate_->type != nullptr && !site.function_name &&
            site.target->kind != scope_model::entry_kind::template_parameter &&
            scopes_.declared_in(*site.target, *candidate_->type);
    }

    // Records the last mention in `sites` unless `live`, `returned` or it is not
    // safe to move. Returns whether the name is live before `sites`.
    auto scan_sites(const std::vector<use_site>& sites, bool live, bool returned) -> bool {
        if (collecting_ != nullptr) {
            for (const auto& site : sites) {
                if (site.target != nullptr) collecting_->push_back(site.target);
            }
            return false;
        }
        auto uses = std::vector<const use_site*>{};
        for (const auto& site : sites) {
            if (matches(site) && !site.compile_time) uses.push_back(&site);
        }
        if (uses.empty()) return live;
        ++mentions_found_;
        if (live || returned || (candidate_->in_postcondition && !in_return_)) return true;
        // A definite last use is the only use of the name in its statement,
        // except for uses in other alternatives of an inspect, which are not
        // evaluated with it, so each alternative may hold one.
        const auto* last = uses.back();
        for (const auto* use : uses) {
            if (use->target != last->target) continue;
            const auto alone = std::ranges::none_of(sites, [&](const use_site& site) {
                return &site != use && site.target == use->target && !site.compile_time &&
                    !exclusive(site, *use);
            });
            // A pack parameter in its own expansion names each element once.
            const auto pack = use->target->declared_parameter != nullptr &&
                use->target->declared_parameter->is_variadic;
            if (!alone || (use->expanded && !pack) || use->unmoved_argument) continue;
            const auto kind = candidate_->kind == last_use_kind::this_member &&
                    use->target == candidate_->target
                ? last_use_kind::move : candidate_->kind;
            (*output_)[use->text].push_back({use->path, use->name, kind});
        }
        return true;
    }

    auto scan_text(const text_key& key, const parsed_expression& text, scope_point point,
                   bool live, bool returned = false) -> bool {
        return scan_sites(sites(key, text.tree(), point), live, returned);
    }

    // Whether `statements` mention the name, recording no last use there.
    auto probe(const statement_list& statements) -> bool {
        const auto saved = live_at_exit_;
        const auto before = mentions_found_;
        live_at_exit_ = true;
        static_cast<void>(scan(statements, 0, true));
        live_at_exit_ = saved;
        return mentions_found_ != before;
    }

    auto probe(const text_key& key, const auto& text, scope_point point) -> bool {
        const auto before = mentions_found_;
        static_cast<void>(scan_text(key, text, point, true));
        return mentions_found_ != before;
    }

    [[nodiscard]] auto returns_candidate(const parsed_expression& text) const -> bool {
        const auto* value = whole_expression(text.tree());
        return candidate_->initialized_local && value != nullptr && is_single_name(*value) &&
            text.tree().tokens[value->first].text == candidate_->target->name;
    }

    // Returns whether the name is live before `statements`, given `live` after
    // them. A statement that mentions no declaration the candidate matches is skipped.
    auto scan(const statement_list& statements, std::size_t begin, bool live) -> bool {
        // A defer body runs at every exit of the block, so a name it mentions
        // has no last use in the block.
        if (collecting_ == nullptr) {
            for (const auto index : defers_in(statements)) {
                if (index >= begin && scan(*statements[index], false)) return true;
            }
        }
        // The statements after the first exit are never reached.
        const auto end = collecting_ == nullptr ? first_exit(statements) : statements.size();
        if (end < statements.size()) {
            if (end < begin) return live;
            live = scan(*statements[end], live);
        }
        if (collecting_ != nullptr || candidate_->type != nullptr) {
            for (auto index = end; index-- > begin;) live = scan(*statements[index], live);
            return live;
        }
        const auto& mentions = mentions_in(statements);
        const auto found = mentions.find(candidate_->target);
        if (found == mentions.end()) return live;
        for (auto index = found->second.rbegin();
             index != found->second.rend() && *index >= begin; ++index) {
            if (*index < end) live = scan(*statements[*index], live);
        }
        return live;
    }

    // The index of the first `return`, `throw`, `break` or `continue` in
    // `statements`, or their size.
    auto first_exit(const statement_list& statements) -> std::size_t {
        const auto [found, added] = exits_.try_emplace(&statements, statements.size());
        if (added) {
            for (auto index = std::size_t{0}; index < statements.size(); ++index) {
                if (is_exit(*statements[index])) {
                    found->second = index;
                    break;
                }
            }
        }
        return found->second;
    }

    [[nodiscard]] static auto is_exit(const statement& value) -> bool {
        switch (value.kind) {
        case statement_kind::return_statement:
        case statement_kind::break_statement:
        case statement_kind::continue_statement:
            return true;
        case statement_kind::expression:
        case statement_kind::discard_statement: {
            const auto& tokens = value.as<statement::expression_form>().text.tree().tokens;
            return !tokens.empty() && tokens.front().text == "throw";
        }
        default:
            return false;
        }
    }

    auto defers_in(const statement_list& statements) -> const std::vector<std::size_t>& {
        const auto [found, added] = defers_.try_emplace(&statements);
        if (added) {
            for (auto index = std::size_t{0}; index < statements.size(); ++index) {
                if (statements[index]->kind == statement_kind::defer_statement) {
                    found->second.push_back(index);
                }
            }
        }
        return found->second;
    }

    auto mentions_in(const statement_list& statements)
        -> const std::unordered_map<const scope_entry*, std::vector<std::size_t>>& {
        if (const auto found = mentions_.find(&statements); found != mentions_.end()) {
            return found->second;
        }
        auto result = std::unordered_map<const scope_entry*, std::vector<std::size_t>>{};
        auto targets = std::vector<const scope_entry*>{};
        collecting_ = &targets;
        for (auto index = std::size_t{0}; index < statements.size(); ++index) {
            targets.clear();
            static_cast<void>(scan(*statements[index], false));
            std::sort(targets.begin(), targets.end(), std::less<>{});
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
            for (const auto* target : targets) result[target].push_back(index);
        }
        collecting_ = nullptr;
        return mentions_[&statements] = std::move(result);
    }

    // Returns whether the name is live before `value`, given `live` after it.
    auto scan(const statement& value, bool live) -> bool {
        using enum text_key::part;
        const auto point = scopes_.at(value);
        const auto key = [&](text_key::part which, std::size_t index = 0) {
            return statement_key(value, which, index);
        };
        switch (value.kind) {
        case statement_kind::declaration_statement: {
            const auto& form = value.as<statement::declaration_form>();
            if (!form.declaration) return scan_text(key(text), form.text, point, live);
            const auto& data = *form.declaration;
            if (data.kind != declaration_kind::object || data.is_alias ||
                data.initializer.empty()) {
                return live;
            }
            // A let-else `else` block runs after the initializer and leaves.
            if (form.is_let_else) live = scan(form.else_body, 0, live_at_exit_) || live;
            return scan_text(key(initializer), data.initializer, point, live);
        }
        case statement_kind::destructuring_statement: {
            const auto& form = value.as<statement::destructuring_form>();
            if (form.is_let_else) live = scan(form.else_body, 0, live_at_exit_) || live;
            return scan_text(key(initializer), form.initializer, point, live);
        }
        case statement_kind::expression:
        case statement_kind::discard_statement: {
            return scan_text(key(text), value.as<statement::expression_form>().text, point,
                             is_exit(value) ? live_at_exit_ : live);
        }
        case statement_kind::return_statement: {
            const auto& form = value.as<statement::expression_form>();
            in_return_ = true;
            const auto before = scan_text(key(text), form.text, point, live_at_exit_,
                                          returns_candidate(form.text));
            in_return_ = false;
            return before;
        }
        case statement_kind::block:
        case statement_kind::unsafe_block: {
            const auto& form = value.as<statement::block_form>();
            live = scan(form.body, 0, live);
            for (auto index = form.parameters.size(); index-- > 0;) {
                const auto& parameter = form.parameters[index];
                const auto point = scopes_.at(parameter);
                // An `inout` or `out` parameter binds its initializer as an lvalue.
                const auto passing = parameter.value.passing;
                const auto initializer_key = key(text_key::part::parameter, index);
                live = passing == passing_style::inout || passing == passing_style::out
                    ? probe(initializer_key, parameter.initializer, point) || live
                    : scan_text(initializer_key, parameter.initializer, point, live);
            }
            return live;
        }
        case statement_kind::defer_statement:
            // The body runs when the enclosing block exits, after every
            // other statement of that block.
            return probe(value.as<statement::block_form>().body) || live;
        case statement_kind::try_statement: {
            // A handler may run after any point of the try body.
            const auto& form = value.as<statement::block_form>();
            auto handler_live = false;
            for (const auto& handler : form.handlers) {
                handler_live = scan(handler.body, 0, live) || handler_live;
            }
            const auto saved = live_at_exit_;
            live_at_exit_ = live_at_exit_ || handler_live;
            live = scan(form.body, 0, live || handler_live);
            live_at_exit_ = saved;
            return live;
        }
        case statement_kind::if_statement: {
            const auto& form = value.as<statement::if_form>();
            const auto in_else = scan(form.else_body, 0, live);
            const auto in_then = scan(form.body, 0, live);
            return scan_text(key(condition), form.condition, point, in_else || in_then);
        }
        case statement_kind::while_statement:
        case statement_kind::do_statement: {
            const auto& form = value.as<statement::loop_form>();
            return probe(key(condition), form.condition, point) ||
                probe(key(next), form.next, point) || probe(form.body) || live;
        }
        case statement_kind::for_statement: {
            const auto& form = value.as<statement::loop_form>();
            if (probe(key(next), form.next, point) || probe(form.body)) return true;
            return scan_text(key(condition), form.condition, point, live);
        }
        case statement_kind::contract_statement: {
            const auto& form = value.as<statement::contract_form>();
            // A mention is a use that is never moved.
            auto mentioned = probe(key(message), form.message, point) ||
                probe(key(condition), form.condition, point) ||
                probe(key(group), contract_group_text(form), point);
            for (auto index = std::size_t{0}; !mentioned && index < form.flags.size(); ++index) {
                mentioned = probe(key(flag, index), form.flags[index], point);
            }
            return live || mentioned;
        }
        case statement_kind::break_statement:
        case statement_kind::continue_statement:
            return live_at_exit_;
        case statement_kind::using_declaration:
            break;
        }
        return live;
    }
};

// Whether a declared type is or may be a Cpp1 reference: `cpp1_ref<T>`, a
// `decltype`, a template type parameter, which may be given explicitly as a
// reference, or a type depending on one, a type this translation unit does
// not declare, such as the Cpp1 alias `std::vector<T>::reference` or one a
// using-declaration names, or an alias of these.
auto may_be_reference(const type_id& type, const scope_model& scopes, scope_point point,
                      int depth = 0) -> bool {
    if (depth > 8) return false;
    if (type.kind == type_kind::reference || type.kind == type_kind::decltype_of ||
        type.kind == type_kind::cpp1) {
        return true;
    }
    if (type.kind == type_kind::constant) {
        return !type.children.empty() &&
            may_be_reference(type.children.front(), scopes, point, depth);
    }
    if (type.kind != type_kind::name || type.components.empty()) return false;
    const auto& last = type.components.back().identifier;
    if (type.is_dependent || last == "cpp1_ref" || last == "cpp1_rvalue_ref") return true;
    // A qualified name is looked up through this translation unit's scopes;
    // a member of a template specialization, such as `Box<int>::Inner`, is not.
    auto name = std::string{type.is_global ? "::" : ""};
    for (auto index = std::size_t{0}; index < type.components.size(); ++index) {
        const auto& component = type.components[index];
        if (index + 1 < type.components.size() && component.has_arguments) return true;
        if (index != 0) name += "::";
        name += component.identifier;
    }
    const auto* entry = scopes.lookup(name, point);
    if (entry != nullptr) entry = scopes.resolve(*entry);
    // An unresolved using-declaration names a Cpp1 declaration.
    if (entry == nullptr || entry->kind == scope_model::entry_kind::using_name) return true;
    if (entry->kind == scope_model::entry_kind::template_parameter) return true;
    return entry->kind == scope_model::entry_kind::type && !entry->alias.empty() &&
        may_be_reference(parse_type(entry->alias), scopes, {entry->scope, entry->ordinal},
                         depth + 1);
}

// How a parameter's definite last use passes it on: a `copy` or `move` parameter
// is moved unless its type may be a reference, and a `forward` one is forwarded.
auto movable_kind(const parameter& value, const scope_model& scopes)
    -> std::optional<last_use_kind> {
    if (value.passing == passing_style::forward) return last_use_kind::forward;
    if (value.passing != passing_style::copy && value.passing != passing_style::move) {
        return std::nullopt;
    }
    const auto* entry = scopes.declared_as(value);
    return entry != nullptr &&
            may_be_reference(value.type, scopes, {entry->scope, entry->ordinal})
        ? last_use_kind::forward : last_use_kind::move;
}

// `guard`-prefixed names are never moved or forwarded; their destruction is their last use.
auto movable_name(std::string_view name) -> bool {
    return !name.empty() && name != "_" && name != "this" && !name.starts_with("guard");
}

// The locals and statement parameters declared in `statements`, with their scopes.
auto find_local_last_uses(last_use_finder& finder, const scope_model& scopes,
                          const statement_list& statements) -> void {
    for (auto index = std::size_t{0}; index < statements.size(); ++index) {
        const auto& statement = *statements[index];
        if (const auto* form = std::get_if<statement::declaration_form>(&statement.form)) {
            if (form->declaration) {
                const auto& data = *form->declaration;
                const auto* entry = scopes.declared_by(statement);
                // A let-else name refers into its value, and a reference refers
                // to another object, which only forwarding leaves alone. A
                // deferred local is held by value, never as a reference.
                const auto kind = form->is_let_else ||
                        (entry != nullptr && !data.initializer.empty() &&
                         may_be_reference(data.type, scopes, scopes.at(statement)))
                    ? last_use_kind::forward : last_use_kind::move;
                // A `static` local outlives the call, so no use of it is a last use.
                if (entry != nullptr && data.kind == declaration_kind::object && !data.is_alias &&
                    !data.is_static && !data.is_named_return && movable_name(data.name)) {
                    finder.find({entry, kind, !data.initializer.empty() && !form->is_let_else,
                                 nullptr},
                                statements, index + 1);
                }
            }
        }
        else if (const auto* form = std::get_if<statement::destructuring_form>(&statement.form)) {
            // A binding may name an element of a reference, which only forwarding
            // leaves alone, and is not moved implicitly by `return`. A let-else
            // binding names an element of its value, and is never moved.
            for (const auto& binding : form->bindings) {
                const auto* entry = scopes.declared_by(binding);
                if (entry != nullptr && !form->is_let_else &&
                    movable_name(binding.name)) {
                    finder.find({entry, last_use_kind::forward, false, nullptr}, statements,
                                index + 1);
                }
            }
        }
        else if (const auto* form = std::get_if<statement::block_form>(&statement.form)) {
            for (const auto& parameter : form->parameters) {
                const auto kind = movable_kind(parameter.value, scopes);
                const auto* entry = scopes.declared_as(parameter.value);
                if (entry != nullptr && kind && movable_name(parameter.value.name)) {
                    finder.find({entry, *kind, true, nullptr}, form->body, 0);
                }
            }
        }
        if (const auto* condition = condition_of(statement)) {
            // An `as` binding may bind a temporary, which only forwarding moves;
            // a destructured name is an element of the subject.
            const auto& body = statement.is<statement::if_form>()
                ? statement.as<statement::if_form>().body
                : statement.as<statement::loop_form>().body;
            const auto& bindings = statement.is<statement::if_form>()
                ? statement.as<statement::if_form>().bindings
                : statement.as<statement::loop_form>().bindings;
            for (const auto& binding : bindings) {
                const auto* entry = scopes.declared_by(binding);
                const auto& tree = condition->tree();
                if (entry != nullptr && bound_subject(tree, tree.root, binding.name).second &&
                    movable_name(binding.name)) {
                    finder.find({entry, last_use_kind::forward, false, nullptr}, body, 0);
                }
            }
        }
        if (const auto* form = std::get_if<statement::loop_form>(&statement.form)) {
            const auto kind = form->range_parameter
                ? movable_kind(*form->range_parameter, scopes) : std::nullopt;
            const auto* entry = kind ? scopes.declared_as(*form->range_parameter) : nullptr;
            if (entry != nullptr && movable_name(form->range_parameter->name)) {
                finder.find({entry, *kind, false, nullptr}, form->body, 0);
            }
        }
        for_each_nested_list(statement, [&](const statement_list& nested) {
            find_local_last_uses(finder, scopes, nested);
        });
    }
}

// The text lowering renders for a single-expression body: `_ = value` discards `value`.
auto expression_body_text(const parsed_expression& body) -> parsed_expression {
    const auto& tokens = body.tree().tokens;
    if (tokens.size() > 2 && tokens[0].text == "_" && tokens[1].text == "=") {
        return parsed_expression{body.source().substr(body.source().find('=') + 1)};
    }
    return body;
}

// Whether a checked postcondition of `declaration` mentions `name`.
auto in_postcondition(const ir_declaration& declaration, std::string_view name) -> bool {
    const auto mentions = [&](const parsed_expression& text) {
        auto found = false;
        for_each_mention(text.tree(), [&](const mention& value) {
            found = found || value.name() == name;
        });
        return found;
    };
    return std::any_of(declaration.contracts.begin(), declaration.contracts.end(),
        [&](const function_contract& contract) {
            return contract.is_post && contract.group != "unevaluated" &&
                (mentions(contract.condition) || mentions(contract.message) ||
                 mentions(contract_group_text(contract)) ||
                 std::any_of(contract.flags.begin(), contract.flags.end(), mentions));
        });
}

// The member `m` of `x.m` when token `index` is its object `x`, unless it is called.
auto accessed_member(const expression_text& text, std::size_t index)
    -> std::optional<std::string> {
    const auto find = [&](const auto& self, const expression& node,
                          bool callee) -> std::optional<std::string> {
        if (node.kind == expression_kind::member && text.tokens[node.op].text == "." &&
            is_single_name(node.children[0]) && node.first == index) {
            const auto& member = text.tokens[node.children[1].first];
            if (callee || member.kind != expression_token_kind::identifier) return std::nullopt;
            return member.text;
        }
        for (auto child = std::size_t{0}; child < node.children.size(); ++child) {
            const auto& value = node.children[child];
            if (value.first > index || value.last <= index) continue;
            return self(self, value, node.kind == expression_kind::call && child == 0 &&
                                         text.tokens[node.op].text == "(");
        }
        return std::nullopt;
    };
    return find(find, text.root, false);
}

// Member initializers run in declaration order, so a generated copy from `that`
// may follow a leading `member = value;` statement. A last use of `that` there
// is not moved when a later generated initializer reads the same part of `that`.
auto keep_generated_reads(const ir_declaration& function,
                          last_use_map& uses) -> void {
    for (const auto* list : {&function.member_initializers, &function.that_assignments}) {
        for (auto initializer = list->begin(); initializer != list->end(); ++initializer) {
            const auto* source = initializer_statement(function, *initializer);
            if (source == nullptr) continue;
            const auto& text = source->as<statement::expression_form>().text;
            const auto found = uses.find(statement_key(*source));
            if (found == uses.end()) continue;
            std::erase_if(found->second, [&](const last_use& use) {
                const auto member = use.path.size() == 1
                    ? accessed_member(text.tree(), use.path.front()) : std::nullopt;
                return std::any_of(initializer + 1, list->end(), [&](const auto& later) {
                    if (later.source) return false;
                    if (later.is_base) return later.value == "that" && !later.move_that;
                    return member ? later.value == "that." + cpp1_identifier(*member)
                                  : later.value.starts_with("that.");
                });
            });
            if (found->second.empty()) uses.erase(found);
        }
    }
}

// The name an expression consists of, or empty.
auto returned_parameter(const parsed_expression& text) -> std::string_view {
    const auto* value = whole_expression(text.tree());
    return value != nullptr && is_single_name(*value) ? text.tree().tokens[value->first].text
                                                      : std::string_view{};
}

auto find_last_uses(const ir_program& program, const scope_model& scopes,
                    const known_callees& callees, initialization_plan& plan,
                    const experiment_set& experiments) -> void {
    auto finder = last_use_finder{program, scopes, callees, plan, experiments};
    auto visited = std::set<const function_body*>{};
    const auto visit = [&](const auto& self, const ir_declaration& declaration) -> void {
        for (const auto& member : declaration.members) self(self, member);
        if (declaration.kind != declaration_kind::function) return;
        const auto* body = declaration.function_statements.get();
        const auto expression_body = body == nullptr && !declaration.body.empty() &&
            declaration.symbol != invalid_symbol;
        if (body == nullptr && !expression_body) return;
        const auto first_visit = body == nullptr || visited.insert(body).second;

        // A returned name is left to Cpp1's implicit move; where the return type is
        // `decltype(auto)`, the result of `cpp2::move(x)` would be a reference to `x`.
        auto candidates = std::vector<last_use_candidate>{};
        for (const auto& parameter : declaration.parameters) {
            const auto kind = movable_kind(parameter, scopes);
            const auto* entry = scopes.declared_as(parameter);
            if (entry != nullptr && kind && movable_name(parameter.name)) {
                candidates.push_back({entry, *kind, parameter.passing != passing_style::forward,
                                      nullptr, in_postcondition(declaration, parameter.name)});
            }
        }
        const auto inside = scopes.inside(declaration.symbol);
        if (const auto* type = scopes.enclosing_type(inside);
            type != nullptr && declaration.is_type_member &&
            has_receiver(declaration, passing_style::move)) {
            if (const auto* self = scopes.declared_as(declaration.parameters.front())) {
                candidates.push_back({self, last_use_kind::this_member, false, type});
            }
        }
        if (body != nullptr) {
            for (auto candidate : candidates) {
                if (candidate.target->name != "that") {
                    if (first_visit) finder.find(candidate, body->statements, 0);
                }
                else if (!body->statements.empty() && declaration.symbol != invalid_symbol) {
                    // The body's mentions name the `that` of the function whose scope holds it.
                    candidate.target =
                        scopes.lookup("that", scopes.at(*body->statements.front()));
                    if (candidate.target == nullptr) continue;
                    auto& uses = plan.that_last_uses[declaration.symbol];
                    finder.find(candidate, body->statements, uses);
                    keep_generated_reads(declaration, uses);
                }
            }
            if (first_visit) find_local_last_uses(finder, scopes, body->statements);
            return;
        }
        const auto text = expression_body_text(declaration.body);
        const auto key = text_key{declaration.symbol, text_key::part::body};
        const auto returned = returned_parameter(text);
        for (const auto& candidate : candidates) {
            if (candidate.initialized_local && candidate.target->name == returned) continue;
            finder.find(candidate, key, text, inside);
        }
    };
    for (const auto& declaration : program.declarations) visit(visit, declaration);
    for (auto& [function, texts] : plan.that_last_uses) {
        for (auto& [text, uses] : texts) {
            if (const auto* others = plan.last_uses_in(text)) {
                uses.insert(uses.end(), others->begin(), others->end());
            }
        }
    }
}

// Warns where a member `p.m` of a `move` parameter `p` (including `that`) is
// copied because it is not the last use of `p`: when `p.m` is a whole
// initializer, `=` source or returned value in a user-written function (a call
// argument is not reported, since the callee may not copy), outside a loop
// and a compile-time context, and `m` is not known to have a fundamental, pointer or lvalue reference type
// nor read by a later generated initializer.
class member_copy_checker final {
public:
    member_copy_checker(const scope_model& scopes, const initialization_plan& plan,
                        std::vector<diagnostic>& diagnostics)
        : scopes_{scopes}, plan_{plan}, diagnostics_{diagnostics} {}

    auto check(const ir_declaration& function) -> void {
        if (function.kind != declaration_kind::function || function.is_generated ||
            !function.function_statements) {
            return;
        }
        const auto inside = scopes_.inside(function.symbol);
        for (const auto& parameter : function.parameters) {
            if (parameter.passing != passing_style::move || is_this_parameter(parameter)) continue;
            parameter_ = &parameter;
            that_uses_ = parameter.name == "that" ? plan_.that_last_uses_in(function.symbol)
                                                  : nullptr;
            const auto* type = parameter.name == "that" ? scopes_.enclosing_type(inside)
                : parameter.type.is_wildcard() ? nullptr
                : scopes_.lookup(parameter.type.text, inside);
            type_ = type != nullptr && type->kind == scope_model::entry_kind::type ? type
                                                                                   : nullptr;
            // Moving a member in a leading `member = value;` statement that a later
            // generated initializer copies would leave that copy reading a
            // moved-from value.
            generated_reads_.clear();
            if (parameter.name == "that") {
                for (const auto* list :
                     {&function.member_initializers, &function.that_assignments}) {
                    for (auto initializer = list->begin(); initializer != list->end();
                         ++initializer) {
                        if (!initializer->source) continue;
                        auto& reads = generated_reads_[*initializer->source];
                        for (auto later = initializer + 1; later != list->end(); ++later) {
                            if (!later->source && !later->is_base) {
                                reads.insert(later->value);
                            }
                        }
                    }
                }
            }
            check(function.function_statements->statements);
        }
    }

private:
    const scope_model& scopes_;
    const initialization_plan& plan_;
    std::vector<diagnostic>& diagnostics_;
    const parameter* parameter_ = nullptr;
    const last_use_map* that_uses_ = nullptr;
    const scope_model::entry* type_ = nullptr;
    std::unordered_map<statement_id, std::set<std::string, std::less<>>> generated_reads_;

    // Moving a read in a loop would leave the member moved-from on the next iteration.
    auto check(const statement_list& statements) -> void {
        for (const auto& value : statements) {
            if (value->kind == statement_kind::while_statement ||
                value->kind == statement_kind::do_statement ||
                value->kind == statement_kind::for_statement) {
                continue;
            }
            check(*value);
            for_each_nested_list(*value, [&](const auto& nested) { check(nested); });
        }
    }

    auto check(const statement& value) -> void {
        switch (value.kind) {
        case statement_kind::expression:
        case statement_kind::discard_statement:
            check_text(value, statement_key(value),
                       value.as<statement::expression_form>().text.tree(), false);
            break;
        case statement_kind::return_statement:
            check_text(value, statement_key(value),
                       value.as<statement::expression_form>().text.tree(), true);
            break;
        case statement_kind::declaration_statement: {
            const auto& form = value.as<statement::declaration_form>();
            if (form.declaration && form.declaration->kind == declaration_kind::object &&
                !form.declaration->is_alias && !form.declaration->initializer.empty()) {
                const auto& initializer = form.declaration->initializer;
                check_text(value, statement_key(value, text_key::part::initializer),
                           initializer.tree(), true);
            }
            break;
        }
        case statement_kind::destructuring_statement:
            check_text(value, statement_key(value, text_key::part::initializer),
                       value.as<statement::destructuring_form>().initializer.tree(), true);
            break;
        case statement_kind::block:
        case statement_kind::try_statement:
        case statement_kind::if_statement:
        case statement_kind::while_statement:
        case statement_kind::do_statement:
        case statement_kind::for_statement:
        case statement_kind::break_statement:
        case statement_kind::continue_statement:
        case statement_kind::using_declaration:
        case statement_kind::contract_statement:
        case statement_kind::defer_statement:
        case statement_kind::unsafe_block:
            break;
        }
    }

    auto check_text(const statement& value, const text_key& key, const expression_text& parsed,
                    bool whole_value) -> void {
        const auto* uses = plan_.last_uses_in(key);
        if (that_uses_ != nullptr) {
            if (const auto found = that_uses_->find(key); found != that_uses_->end()) {
                uses = &found->second;
            }
        }
        const auto& tokens = parsed.tokens;
        const auto visit = [&](const auto& self, const expression& node, bool whole,
                               bool compile_time) -> void {
            if (whole && node.kind == expression_kind::member &&
                is_single_name(node.children[0]) && is_single_name(node.children[1]) &&
                tokens[node.first].text == parameter_->name && !compile_time) {
                const auto& member = tokens[node.children[1].first].text;
                const auto last = uses != nullptr && std::any_of(uses->begin(), uses->end(),
                    [&](const last_use& use) {
                        return use.path.size() == 1 && use.path.front() == node.first &&
                            use.name == parameter_->name;
                    });
                const auto reads = generated_reads_.find(value.id);
                if (!last && !copies_cheaply(member) &&
                    (reads == generated_reads_.end() ||
                     !reads->second.contains("that." + cpp1_identifier(member)))) {
                    report(value, parameter_->name + "." + member);
                }
                return;
            }
            for (auto child = std::size_t{0}; child < node.children.size(); ++child) {
                self(self, node.children[child],
                     node.kind == expression_kind::assignment && child == 1 &&
                         tokens[node.op].text == "=",
                     compile_time || compile_time_child(parsed, node, child));
            }
        };
        if (const auto* whole = whole_value ? whole_expression(parsed) : nullptr) {
            visit(visit, *whole, true, false);
        }
        else {
            for (const auto& child : parsed.root.children) visit(visit, child, false, false);
        }
    }

    // Whether the member `name` of the parameter's type has a fundamental, pointer
    // or lvalue reference type, which a move copies.
    [[nodiscard]] auto copies_cheaply(std::string_view name) const -> bool {
        const auto* member = type_ != nullptr ? scopes_.member(*type_, name) : nullptr;
        if (member == nullptr || member->kind != scope_model::entry_kind::object) return false;
        return member->type.kind == type_kind::fundamental || pointer_depth(member->type) > 0 ||
            (member->type.kind == type_kind::reference && !member->type.is_rvalue);
    }

    auto report(const statement& value, const std::string& member) -> void {
        const auto& name = parameter_->name;
        auto message = "'" + member + "' is copied, not moved: only the last use of '" + name +
            "' moves; write '(move " + member + ")' to move it";
        const auto duplicate = std::any_of(diagnostics_.begin(), diagnostics_.end(),
            [&](const diagnostic& issue) {
                return issue.span.begin.line == value.span.begin.line &&
                    issue.span.begin.column == value.span.begin.column &&
                    issue.message == message;
            });
        if (!duplicate) {
            diagnostics_.push_back({"member-copied-not-moved", diagnostic_severity::warning,
                                    value.span, std::move(message)});
        }
    }
};

// Whether a fundamental integer type is signed; nullopt for any other type,
// including `char`, whose signedness is implementation-defined.
auto integer_signedness(const type_id& type) -> std::optional<bool> {
    static const auto signedness = std::unordered_map<std::string_view, bool>{
        {"i8", true}, {"i16", true}, {"i32", true}, {"i64", true}, {"short", true},
        {"int", true}, {"long", true}, {"longlong", true}, {"_schar", true},
        {"signed char", true}, {"long long", true}, {"std::int8_t", true},
        {"std::int16_t", true}, {"std::int32_t", true}, {"std::int64_t", true},
        {"std::ptrdiff_t", true}, {"std::intmax_t", true}, {"std::intptr_t", true},
        {"u8", false}, {"u16", false}, {"u32", false}, {"u64", false}, {"ushort", false},
        {"uint", false}, {"ulong", false}, {"ulonglong", false}, {"_uchar", false},
        {"unsigned", false}, {"unsigned char", false}, {"unsigned short", false},
        {"unsigned int", false}, {"unsigned long", false}, {"unsigned long long", false},
        {"size_t", false}, {"std::size_t", false}, {"std::uint8_t", false},
        {"std::uint16_t", false}, {"std::uint32_t", false}, {"std::uint64_t", false},
        {"std::uintmax_t", false}, {"std::uintptr_t", false},
    };
    const auto* core = &type;
    while (core->kind == type_kind::constant && !core->children.empty()) {
        core = &core->children.front();
    }
    const auto found = signedness.find(core->text);
    if (found == signedness.end()) return std::nullopt;
    return found->second;
}

// safety.md: `<`, `<=`, `>` and `>=` reject signed/unsigned integer operands.
// An operand's signedness is known for an unqualified name declared with an
// integer type and for an integer literal; `cpp2::impl::cmp_*` rejects the rest
// in the Cpp1 compile. With experiment `nonnegative-literal-comparisons`, a
// decimal literal without a `u` suffix compares with either signedness. With
// experiment `newtype`, also `x as T` from one distinct value type to another.
class comparison_checker final {
public:
    comparison_checker(const scope_model& scopes, bool check_signedness,
                       const experiment_set& experiments,
                       const std::map<symbol_id, std::string>& distinct_types,
                       std::vector<diagnostic>& diagnostics)
        : scopes_{scopes}, check_signedness_{check_signedness},
          literals_either_sign_{experiments.enabled("nonnegative-literal-comparisons")},
          distinct_types_{distinct_types}, diagnostics_{diagnostics} {}

    // With `unchecked`, inside an `unsafe` block, as with `-no-comparison-checks`,
    // a mixed-sign comparison is not rejected.
    auto check(const parsed_expression& text, source_span span, scope_point point,
               bool unchecked) -> void {
        const auto saved = std::exchange(check_signedness_, check_signedness_ && !unchecked);
        located_ = &text;
        check(text.tree(), text.tree().root, span, point);
        located_ = nullptr;
        check_signedness_ = saved;
    }

private:
    auto signedness(const expression_text& text, const expression& node, scope_point point) const
        -> std::optional<bool> {
        if (node.last != node.first + 1) return std::nullopt;
        const auto& value = text.tokens[node.first];
        if (node.kind == expression_kind::literal && value.kind == expression_token_kind::number) {
            const auto& digits = value.text;
            if (digits.find_first_of(".eEpP") != std::string::npos) return std::nullopt;
            if (digits.find_first_of("uU") != std::string::npos) return false;
            // An unsuffixed decimal literal is signed; a hexadecimal, octal or
            // binary one may be unsigned.
            if (digits.size() > 1 && digits.front() == '0') return std::nullopt;
            if (literals_either_sign_) return std::nullopt;
            return true;
        }
        if (node.kind != expression_kind::name || value.kind != expression_token_kind::identifier ||
            !point.valid()) {
            return std::nullopt;
        }
        using kind = scope_model::entry_kind;
        const auto* found = scopes_.lookup(value.text, point);
        if (found != nullptr) found = scopes_.resolve(*found);
        if (found == nullptr || (found->kind != kind::object && found->kind != kind::parameter &&
                                 found->kind != kind::statement_parameter)) {
            return std::nullopt;
        }
        return integer_signedness(found->type);
    }

    auto check(const expression_text& text, const expression& node, source_span span,
               scope_point point) -> void {
        if (node.kind == expression_kind::binary && node.children.size() > 1) {
            auto kinds = std::string{};
            for (auto index = std::size_t{1}; index < node.children.size(); ++index) {
                const auto op = binary_operator(text.tokens, node, index);
                kinds += comparison_kind(op);
                if (check_signedness_ && (kinds.back() == 'l' || kinds.back() == 'g')) {
                    check_signedness(text, node.children[index - 1], node.children[index], op,
                                     span, point);
                }
            }
            const auto has = [&](char kind) { return kinds.find(kind) != std::string::npos; };
            const auto* message = kinds.size() < 2 ? nullptr
                : has('n') ? "!= comparisons cannot appear in a comparison chain"
                : kinds.find_first_not_of(kinds.front()) != std::string::npos
                ? "a comparison chain must be all < and <=, all > and >=, or all ==" : nullptr;
            if (!has('\0') && message != nullptr) {
                diagnostics_.push_back({"comparison-chain", diagnostic_severity::error,
                    located_ != nullptr ? token_span(*located_, node.first, span) : span,
                    message});
            }
        }
        if (node.kind == expression_kind::is_as && text.tokens[node.op].text == "as") {
            check_distinct_conversion(text, node, span, point);
        }
        if (node.kind == expression_kind::is_as && text.tokens[node.op].text == "is") {
            check_type_subject(text, node.children[0], node.children[1], span, point);
        }
        if (node.kind == expression_kind::inspect) {
            for (auto index = std::size_t{2}; index < node.children.size(); ++index) {
                check_type_subject(text, node.children[0], node.children[index].children[0],
                                   span, point);
            }
        }
        if (node.kind == expression_kind::literal) {
            for (auto index = node.first; index < node.last; ++index) {
                if (text.tokens[index].kind != expression_token_kind::string) continue;
                // A comparison in an interpolation is reported at the string.
                const auto* located = std::exchange(located_, nullptr);
                const auto at = located != nullptr ? token_span(*located, index, span) : span;
                for (const auto& nested : interpolated_expressions(text.tokens[index].text)) {
                    const auto interpolated = parse_expression_text(nested);
                    check(interpolated, interpolated.root, at, point);
                }
                located_ = located;
            }
        }
        for (const auto& child : node.children) check(text, child, span, point);
    }

    // The `@newtype`, `@quantity` or `@unit` type that a deduced local
    // initialized with `x as T` or `T(...)` has, or null.
    auto deduced_distinct_type(const scope_entry& local) const -> const scope_entry* {
        const auto* declaring = local.declaring_statement;
        const auto* form = declaring == nullptr
            ? nullptr : std::get_if<statement::declaration_form>(&declaring->form);
        if (form == nullptr || !form->declaration) return nullptr;
        const auto& text = form->declaration->initializer.tree();
        const auto* value = whole_expression(text);
        if (value == nullptr) return nullptr;
        if (value->kind == expression_kind::is_as && value->children.size() == 2 &&
            text.tokens[value->op].text == "as") {
            return distinct_type(text, value->children[1], scopes_.at(*declaring), false);
        }
        return value->kind == expression_kind::call
            ? distinct_type(text, value->children.front(), scopes_.at(*declaring), false)
            : nullptr;
    }

    // The `@newtype`, `@quantity` or `@unit` type that an unqualified name declared
    // at `point` has, or that it names, or null.
    auto distinct_type(const expression_text& text, const expression& node, scope_point point,
                       bool declared_type) const -> const scope_entry* {
        if (node.last != node.first + 1 || !point.valid() ||
            text.tokens[node.first].kind != expression_token_kind::identifier) {
            return nullptr;
        }
        using kind = scope_model::entry_kind;
        const auto* found = scopes_.lookup(text.tokens[node.first].text, point);
        if (found != nullptr) found = scopes_.resolve(*found);
        if (found != nullptr && declared_type) {
            if (found->kind != kind::object && found->kind != kind::parameter &&
                found->kind != kind::statement_parameter) {
                return nullptr;
            }
            if (found->type.is_wildcard()) return deduced_distinct_type(*found);
            found = scopes_.lookup(found->type.text, point);
            if (found != nullptr) found = scopes_.resolve(*found);
        }
        return found != nullptr && found->kind == kind::type && distinct_types_.contains(found->symbol)
            ? found : nullptr;
    }

    // `T is 0`: a value after `is` whose subject is visibly a type.
    auto check_type_subject(const expression_text& text, const expression& subject,
                            const expression& pattern, source_span span, scope_point point)
        -> void {
        if (pattern.kind == expression_kind::sequence || !point.valid() ||
            !is_type_subject(text.tokens, subject, scopes_, point)) {
            return;
        }
        diagnostics_.push_back({"type-subject-value", diagnostic_severity::error,
            located_ != nullptr ? token_span(*located_, pattern.first, span) : span,
            "'" + text.tokens[subject.first].text + "' is a type, so it cannot be tested "
            "against a value - after 'is', write a type, a template or a concept"});
    }

    // `x as T` between two distinct value types: neither converts to the other.
    auto check_distinct_conversion(const expression_text& text, const expression& node,
                                   source_span span, scope_point point) -> void {
        const auto* source = distinct_type(text, node.children[0], point, true);
        const auto* target = distinct_type(text, node.children[1], point, false);
        if (source == nullptr || target == nullptr || source->symbol == target->symbol) return;
        diagnostics_.push_back({"newtype-conversion", diagnostic_severity::error, span,
            "no conversion between distinct types " + source->name + " and " + target->name +
            " - convert through the underlying type, as in '(" +
            text.tokens[node.children[0].first].text + " as " +
            distinct_types_.find(source->symbol)->second + ") as " + target->name + "'"});
    }

    auto check_signedness(const expression_text& text, const expression& left,
                          const expression& right, const std::string& op, source_span span,
                          scope_point point) -> void {
        const auto lhs = signedness(text, left, point);
        const auto rhs = signedness(text, right, point);
        if (!lhs || !rhs || *lhs == *rhs) return;
        const auto* opt_out = op == "<" ? "unchecked_cmp_less" : op == "<=" ? "unchecked_cmp_less_eq"
            : op == ">" ? "unchecked_cmp_greater" : "unchecked_cmp_greater_eq";
        // A signed literal operand, as `0` in `s > 0`, is made unsigned with a `u` suffix.
        const auto& literal = *lhs ? left : right;
        const auto suffix = literal.kind == expression_kind::literal &&
                literal.last == literal.first + 1 &&
                text.tokens[literal.first].kind == expression_token_kind::number
            ? "write '" + text.tokens[literal.first].text + "u' for an unsigned literal, "
            : std::string{};
        diagnostics_.push_back({"mixed-sign-comparison", diagnostic_severity::error,
            located_ != nullptr ? token_span(*located_, left.first, span) : span,
            "'" + text.tokens[left.first].text + " " + op + " " + text.tokens[right.first].text +
            "' is a mixed signed/unsigned comparison, which is unsafe - " + suffix +
            "use 'as' to change the signedness of one operand, or " + opt_out + " to opt out"});
    }

    const scope_model& scopes_;
    bool check_signedness_;
    bool literals_either_sign_;
    const std::map<symbol_id, std::string>& distinct_types_;
    std::vector<diagnostic>& diagnostics_;
    // The text being checked, which locates its tokens; null in an interpolation.
    const parsed_expression* located_ = nullptr;
};

// The `@newtype`, `@quantity` and `@unit` types, which do not convert to each
// other, each with its underlying type.
auto distinct_types_of(const ir_program& program) -> std::map<symbol_id, std::string> {
    auto result = std::map<symbol_id, std::string>{};
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
        if (declaration.kind != declaration_kind::type) return;
        for (const auto* metaclass : {"newtype", "quantity", "unit"}) {
            if (const auto* arguments = metaclass_arguments_of(declaration, metaclass)) {
                result.emplace(declaration.symbol,
                               arguments->empty() ? std::string{} : arguments->front());
            }
        }
    });
    return result;
}

auto check_comparisons(const ir_program& program, const scope_model& scopes,
                       bool check_signedness, const experiment_set& experiments,
                       std::vector<diagnostic>& diagnostics) -> void {
    const auto distinct_types = experiments.enabled("newtype")
        ? distinct_types_of(program) : std::map<symbol_id, std::string>{};
    auto checker = comparison_checker{scopes, check_signedness, experiments, distinct_types,
                                      diagnostics};
    auto point = scope_point{};
    // Everything lexically inside an `unsafe` block, function expressions too.
    const auto unsafe_blocks = experiments.enabled("unsafe")
        ? ir_detail::unsafe_spans(program.declarations) : std::vector<source_span>{};
    const auto check = [&](const parsed_expression& text, source_span span) {
        checker.check(text, span, point, ir_detail::inside_any(unsafe_blocks, span));
    };
    const auto check_contract = [&](const contract_clause& clause, source_span span) {
        for (const auto& flag : clause.flags) check(flag, span);
        check(clause.condition, span);
        check(clause.message, span);
    };
    const auto check_statement = [&](const statement& value) {
        point = scopes.at(value);
        std::visit([&](const auto& form) {
            using form_type = std::remove_cvref_t<decltype(form)>;
            if constexpr (std::is_same_v<form_type, statement::block_form>) {
                for (const auto& parameter : form.parameters) {
                    point = scopes.at(parameter);
                    check(parameter.initializer, value.span);
                }
            }
            else if constexpr (std::is_same_v<form_type, statement::expression_form>) {
                check(form.text, value.span);
            }
            else if constexpr (std::is_same_v<form_type, statement::declaration_form>) {
                // The initializer knows where it begins, unlike the statement's text.
                const auto initialized = form.declaration && !form.declaration->initializer.empty();
                check(initialized ? form.declaration->initializer : form.text, value.span);
            }
            else if constexpr (std::is_same_v<form_type, statement::destructuring_form>) {
                check(form.initializer, value.span);
            }
            else if constexpr (std::is_same_v<form_type, statement::if_form>) {
                check(form.condition, value.span);
            }
            else if constexpr (std::is_same_v<form_type, statement::loop_form>) {
                check(form.condition, value.span);
                check(form.next, value.span);
            }
            else if constexpr (std::is_same_v<form_type, statement::contract_form>) {
                check_contract(form, value.span);
            }
        }, value.form);
    };
    // A generated member may share its source member's body, which is checked once.
    auto checked = std::set<const function_body*>{};
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
        if (!declaration.is_generated) {
            for (const auto& contract : declaration.contracts) {
                point = scopes.inside(declaration.symbol);
                check_contract(contract, declaration.span);
            }
        }
        if (declaration.function_statements) {
            if (checked.insert(declaration.function_statements.get()).second) {
                ir_detail::visit_statements(declaration.function_statements->statements,
                                            check_statement);
            }
        }
        else if (!declaration.is_generated && !declaration.body.empty() &&
                 (declaration.kind == declaration_kind::object ||
                  declaration.kind == declaration_kind::function)) {
            point = scopes.inside(declaration.symbol);
            check(expression_body_text(declaration.body), declaration.span);
        }
    });
}

// A constructor initializes its bases and data members in declaration order,
// a member with a default initializer included, after checking its
// preconditions. A read of a data member that is not yet initialized, as `y`
// or `this.y`, in a precondition or a leading `member = value;` statement is
// `use-before-initialization`.
auto check_member_reads(const ir_program& program, const scope_model& scopes,
                        std::vector<diagnostic>& diagnostics) -> void {
    auto analyzed = std::set<const function_body*>{};
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& type) {
        if (type.kind != declaration_kind::type) return;
        auto members = std::vector<const ir_declaration*>{};
        for (const auto& member : type.members) {
            if (member.kind == declaration_kind::object && member.alias.empty() &&
                !member.is_lazy && member.name != "this") {
                members.push_back(&member);
            }
        }
        for (const auto& function : type.members) {
            if (!is_constructor(function)) continue;
            auto initialized = std::set<const ir_declaration*>{};
            auto reported = std::set<const ir_declaration*>{};
            // The reads in `text` after its token `from`.
            const auto check = [&](const parsed_expression& text, std::size_t from,
                                   scope_point point, source_span span) {
                if (text.empty()) return;
                for_each_mention(text.tree(), [&](const mention& value) {
                    const auto token = value.path.front();
                    if (value.compile_time || value.function_name || token < from) return;
                    const auto& inner = value.text->tokens;
                    const auto through_this = value.name() == "this";
                    const auto* entry = through_this ? nullptr
                                                     : scopes.lookup(value.name(), point);
                    const auto read = std::find_if(members.begin(), members.end(),
                        [&](const ir_declaration* member) {
                            return through_this
                                ? value.token + 2 < inner.size() &&
                                    inner[value.token + 1].text == "." &&
                                    inner[value.token + 2].text == member->name
                                : entry != nullptr && entry->symbol == member->symbol;
                        });
                    if (read == members.end() || initialized.contains(*read) ||
                        !reported.insert(*read).second) {
                        return;
                    }
                    diagnostics.push_back({"use-before-initialization",
                        diagnostic_severity::error, token_span(text, token, span),
                        "member " + (*read)->name + " is used before it was initialized "
                        "(members are initialized in the order they are declared)"});
                }, &program.function_expressions);
            };
            if (!function.is_generated) {
                for (const auto& contract : function.contracts) {
                    if (contract.is_post) continue;
                    check(contract.condition, 0, scopes.inside(function.symbol), function.span);
                    check(contract.message, 0, scopes.inside(function.symbol), function.span);
                }
            }
            if (function.initializer_statements == 0 ||
                !analyzed.insert(function.function_statements.get()).second) {
                continue;
            }
            for (const auto* member : members) {
                const auto initializer = std::find_if(function.member_initializers.begin(),
                    function.member_initializers.end(), [&](const auto& value) {
                        return value.member == member->name ||
                            (!member->base_helper.empty() && value.member == member->base_helper);
                    });
                const auto* source = initializer == function.member_initializers.end()
                    ? nullptr : initializer_statement(function, *initializer);
                if (source != nullptr) {
                    const auto& text = source->as<statement::expression_form>().text;
                    const auto& tokens = text.tree().tokens;
                    const auto assigned = static_cast<std::size_t>(
                        std::find_if(tokens.begin(), tokens.end(),
                            [](const auto& token) { return token.text == "="; }) -
                        tokens.begin());
                    check(text, assigned + 1, scopes.at(*source), source->span);
                }
                initialized.insert(member);
            }
        }
    });
}

// Rejects an object, local or statement parameter whose initializer uses its
// own name: the mention finds the declaration being initialized.
auto check_own_initializers(const ir_program& program, const scope_model& scopes,
                            std::vector<diagnostic>& diagnostics) -> void {
    const auto check = [&](const parsed_expression& initializer, const scope_entry* entry,
                           scope_point point, source_span span) {
        if (entry == nullptr || initializer.empty()) return;
        auto reported = false;
        for_each_mention(initializer.tree(), [&](const mention& value) {
            if (reported || value.compile_time || value.function_name ||
                scopes.lookup(value.name(), point) != entry) {
                return;
            }
            reported = true;
            diagnostics.push_back({"own-initializer", diagnostic_severity::error, span,
                "'" + value.name() + "' cannot be used in its own initializer"});
        }, &program.function_expressions);
    };
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
        if (declaration.kind != declaration_kind::object || declaration.is_generated ||
            declaration.function_statements) {
            return;
        }
        const auto point = scopes.inside(declaration.symbol);
        const auto* entry = point.valid() ? scopes.lookup(declaration.name, point) : nullptr;
        check(declaration.body, entry, point, declaration.span);
    }, [&](const statement& value) {
        if (const auto* form = std::get_if<statement::declaration_form>(&value.form);
            form != nullptr && form->declaration &&
            form->declaration->kind == declaration_kind::object && !form->declaration->is_alias) {
            check(form->declaration->initializer, scopes.declared_by(value), scopes.at(value),
                  value.span);
        }
        if (const auto* block = std::get_if<statement::block_form>(&value.form)) {
            for (const auto& parameter : block->parameters) {
                check(parameter.initializer, scopes.declared_as(parameter.value),
                      scopes.at(parameter), value.span);
            }
        }
    });
}

// Whether a declared type is visibly a Cpp1 reference: `cpp1_ref<T>`,
// `cpp1_rvalue_ref<T>`, `const` of one, or an alias of this translation unit
// for one.
auto is_reference_type(const type_id& type, const scope_model& scopes, scope_point point,
                       int depth = 0) -> bool {
    if (depth > 8) return false;
    if (type.kind == type_kind::reference) return true;
    if (type.kind == type_kind::constant) {
        return !type.children.empty() &&
            is_reference_type(type.children.front(), scopes, point, depth);
    }
    if (type.kind != type_kind::name || type.components.empty()) return false;
    auto name = std::string{type.is_global ? "::" : ""};
    for (auto index = std::size_t{0}; index < type.components.size(); ++index) {
        if (index + 1 < type.components.size() && type.components[index].has_arguments) {
            return false;
        }
        if (index != 0) name += "::";
        name += type.components[index].identifier;
    }
    const auto* entry = scopes.lookup(name, point);
    if (entry != nullptr) entry = scopes.resolve(*entry);
    if (entry == nullptr) {
        if (name.starts_with("::")) name.erase(0, 2);
        if (name.starts_with("cpp2::")) name.erase(0, 6);
        return name == "cpp1_ref" || name == "cpp1_rvalue_ref";
    }
    return entry->kind == scope_model::entry_kind::type && !entry->alias.empty() &&
        is_reference_type(parse_type(entry->alias), scopes, {entry->scope, entry->ordinal},
                          depth + 1);
}

// Rejects a local without an initializer whose type is visibly a reference,
// which must be bound where it is declared.
auto check_deferred_references(const ir_program& program, const scope_model& scopes,
                               std::vector<diagnostic>& diagnostics) -> void {
    ir_detail::visit_declarations(program.declarations, [](const auto&) {},
        [&](const statement& value) {
            const auto* form = std::get_if<statement::declaration_form>(&value.form);
            if (form == nullptr || !form->declaration) return;
            const auto& data = *form->declaration;
            if (data.kind == declaration_kind::object && !data.is_alias &&
                data.initializer.empty() &&
                is_reference_type(data.type, scopes, scopes.at(value))) {
                diagnostics.push_back({"uninitialized-reference", diagnostic_severity::error,
                    value.span, "'" + data.name + "' has a reference type, so it must be "
                    "initialized where it is declared"});
            }
        });
}

}

auto initialization_plan::constructions_in(statement_id value) const
    -> const std::vector<construction>* {
    const auto found = constructions.find(value);
    return found == constructions.end() ? nullptr : &found->second;
}

auto initialization_plan::last_uses_in(const text_key& text) const
    -> const std::vector<last_use>* {
    const auto found = last_uses.find(text);
    return found == last_uses.end() ? nullptr : &found->second;
}

auto initialization_plan::that_last_uses_in(symbol_id function) const -> const last_use_map* {
    const auto found = that_last_uses.find(function);
    return found == that_last_uses.end() ? nullptr : &found->second;
}

auto analyze_safety(const ir_program& program, bool check_comparisons,
                    const experiment_set& experiments) -> safety_result {
    return analyze_safety(program, scope_model{program}, check_comparisons, experiments);
}

auto analyze_safety(const ir_program& program, const scope_model& scopes,
                    bool check_comparisons, const experiment_set& experiments) -> safety_result {
    auto output = safety_result{};
    auto& diagnostics = output.diagnostics;
    const auto use_after_move = experiments.enabled("use-after-move");
    // The `@linear` types, whose values the flow analysis follows (experiment `linear`).
    auto linear_types = std::set<symbol_id>{};
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
        if (experiments.enabled("linear") && declaration.kind == declaration_kind::type &&
            std::find(declaration.metaclasses.begin(), declaration.metaclasses.end(),
                      "linear") != declaration.metaclasses.end()) {
            linear_types.insert(declaration.symbol);
        }
    });
    // The definite last uses come first: the translator moves a linear value there.
    const auto types = known_types{program, scopes};
    const auto callees = known_callees{types};
    find_last_uses(program, scopes, callees, output.plan, experiments);
    // A generated member may share its source member's body, which is analyzed once.
    auto analyzed = std::set<const function_body*>{};
    check_own_initializers(program, scopes, diagnostics);
    check_deferred_references(program, scopes, diagnostics);
    check_member_reads(program, scopes, diagnostics);
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
        if (declaration.kind == declaration_kind::object &&
            declaration.scope == declaration_scope::namespace_scope &&
            declaration.alias.empty() && declaration.body.empty()) {
            diagnostics.push_back({
                "uninitialized-namespace-object", diagnostic_severity::error,
                declaration.span, "namespace-scope object must have an initializer"});
        }
        const auto expression_body = declaration.kind == declaration_kind::function &&
            !declaration.function_statements && !declaration.body.empty() &&
            !declaration.is_generated;
        // An expression body returns `-> forward _` by default (functions.md).
        const auto reference = !declaration.is_generated &&
            (declaration.deduction == return_deduction::by_forwarding_ref ||
             declaration.deduction == return_deduction::by_forwarding_typed);
        const auto forwards = reference || (!declaration.is_generated &&
            (declaration.deduction == return_deduction::by_forwarding ||
             (expression_body && return_form_of(declaration) == return_form::implicit_deduced)));
        // A discard `_ = value` returns nothing.
        const auto discards = expression_body &&
            expression_body_text(declaration.body).source() != declaration.body.source();
        if (forwards && expression_body && !discards &&
            refers_into_function(returned_name(declaration.body), declaration, scopes,
                                 nullptr, reference)) {
            if (reference) {
                diagnostics.push_back({"forward-return-reference", diagnostic_severity::error,
                    token_span(declaration.body, 0, declaration.span),
                    std::string{forward_return_message}});
            }
            else {
                output.plan.value_returns.insert({declaration.symbol, text_key::part::body});
            }
        }
        if (!expression_body && (!declaration.function_statements ||
                                 !analyzed.insert(declaration.function_statements.get()).second)) {
            return;
        }
        if (forwards && !expression_body) {
            check_forward_returns(declaration.function_statements->statements, declaration,
                                  scopes, reference, output.plan, diagnostics);
        }

        auto loops = std::vector<loop_frame>{};
        auto defers = std::vector<std::pair<const statement*, int>>{};
        auto reported = std::set<flow_report>{};
        const auto* that = that_parameter(declaration);
        const auto copies_that = has_receiver(declaration, passing_style::out) &&
            that != nullptr && that->passing == passing_style::in;
        const auto context = analysis_context{diagnostics, output.plan, scopes,
            program.function_expressions, callees, use_after_move,
            experiments.enabled("try-operator"), &loops, &defers, &linear_types, &reported,
            nullptr, copies_that ? scopes.declared_as(*that) : nullptr, declaration.span};
        auto state = analysis_state{};
        for (const auto& parameter : declaration.parameters) {
            const auto* entry = scopes.declared_as(parameter);
            if (entry != nullptr && !is_this_parameter(parameter) && parameter.name != "_") {
                declare(state, *entry, 0, context);
            }
        }
        check_entry_contracts(declaration, state, context);
        if (expression_body) {
            analyze_expression_body(declaration, state, context);
            report_uninitialized(state, diagnostics);
            return;
        }
        // The closing brace of the body.
        auto brace = declaration.span.end;
        if (brace.column > 1) --brace.column;
        const auto end = source_span{brace, brace};
        analyze_statements(declaration.function_statements->statements, state, context, 0,
                           nullptr, &end);
        report_uninitialized(state, diagnostics);
    });
    cppfrontier::check_comparisons(program, scopes, check_comparisons, experiments, diagnostics);
    auto copies = member_copy_checker{scopes, output.plan, diagnostics};
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
        copies.check(declaration);
    });
    return output;
}

auto check_pointer_safety(const ir_program& program, const token_list& tokens)
    -> std::vector<diagnostic> {
    return pointer_checker{program, tokens}.check();
}

}
