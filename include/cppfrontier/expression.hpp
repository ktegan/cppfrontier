#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <cppfrontier/source.hpp>

namespace cppfrontier {

enum class expression_token_kind { identifier, number, string, character, punctuation };

struct expression_token final {
    expression_token_kind kind = expression_token_kind::punctuation;
    std::string text;
    // The source text when it differs from `text`, as `not` for `!`.
    std::string spelling;
    // Whitespace and comments between the previous token and this one.
    std::string leading;
};

enum class expression_kind {
    sequence,     // tokens that do not form one expression; parsed parts are children
    name,         // possibly qualified; template argument lists are children
    literal,
    group,        // `(items)`
    typed_value,  // `: type = value`; children are the type and the value
    call,         // children are the callee and the arguments; `op` is the open token
    subscript,
    member,       // children are the object and the member name
    postfix,      // `op` follows the operand
    prefix,       // operators precede the operand
    argument,     // `move x`, `out x`; `op` is the passing keyword
    binary,       // a chain of terms at one precedence level
    assignment,
    conditional,
    is_as,        // children are the operand and the type
    range,
    inspect,      // `op` is `inspect`; children are the subject, the result type and alternatives
    // `op` is `is` or `as`; children are the pattern and the value. An `is`
    // alternative with the `patterns` experiment's forms, `is T(a, _) | U as c
    // if g = value`, has the slots pattern, destructuring, a sequence of the
    // `|` patterns, binding, guard and value; an absent slot is an empty sequence.
    alternative,
    // `(pack op ...)`, `(... op pack)`, or `(a op ... op b)` where one of `a`
    // and `b` names a pack; `op` is `...`, and the operator is the tokens
    // between `...` and an operand. Children are the operands.
    fold,
    // `requires (parameters) { requirements }` or `requires { requirements }`;
    // children are the parameters, then the requirements.
    requires_expression,
    // A requires-expression parameter `passing? name: type`; `op` is the name
    // and the child is the type. A parameter in Cpp1 syntax is a sequence.
    parameter,
    // `expression;`, `{ expression } noexcept? -> type;`, `typename type;` or
    // `requires expression;`, told apart by the first token `op`; children
    // are the expression and the type. One that is none of these holds a
    // sequence.
    requirement,
    // `for range do (name) [if guard] yield value` or `... for ...` (experiment
    // `comprehension`); children are the range, the name, the guard when
    // written, and the value or inner comprehension; `op` is `yield` or `for`.
    comprehension,
    // `subject is pattern` with bindings (experiment `condition-bindings`):
    // `v is T as n`, `v is T(a, _)` or `v is (a, b)`. `op` is `is`; children
    // are the subject and an `alternative` whose `|`, guard and value slots
    // are empty.
    pattern_test,
};

struct expression final {
    expression_kind kind = expression_kind::sequence;
    // Tokens [first, last).
    std::size_t first = 0;
    std::size_t last = 0;
    std::size_t op = 0;
    // Ordered, non-overlapping token subranges.
    std::vector<expression> children;
};

struct expression_text final {
    std::vector<expression_token> tokens;
    std::string trailing;
    expression root;
};

enum class last_use_kind : std::uint8_t {
    move,         // `cpp2::move(x)`
    forward,      // `CPP2_FORWARD(x)`
    this_member,  // `cpp2::move(*this).x`, a member named without `this.`
};

// A definite last use of a local or parameter, which lowering moves or forwards.
struct last_use final {
    // Token indices, descending through string interpolations: a string
    // token's index, then the interpolation's ordinal, then indices within it.
    std::vector<std::size_t> path;
    std::string name;
    last_use_kind kind = last_use_kind::move;
};

// An assignment `name = value` whose target is token `position` of a statement's
// text, which constructs the deferred local `name`.
struct construction final {
    std::size_t position = 0;
    std::string name;
};

// The parts of an inspect `alternative` under the `patterns` experiment.
struct inspect_alternative final {
    // The pattern and each `| pattern`: a type-id (a sequence) or a value.
    // None for a tuple pattern.
    std::vector<const expression*> patterns;
    // `(a, _)` after a pattern, or a tuple pattern.
    const expression* destructuring = nullptr;
    // The name after `as`.
    const expression* binding = nullptr;
    const expression* guard = nullptr;
    const expression* value = nullptr;
    // The one pattern is `_`.
    bool wildcard = false;

    // Without a guard, `_` and a tuple pattern match any value.
    [[nodiscard]] auto matches_anything() const -> bool {
        return guard == nullptr && (patterns.empty() || wildcard);
    }
};

[[nodiscard]] auto inspect_alternative_of(const std::vector<expression_token>& tokens,
                                          const expression& alternative) -> inspect_alternative;

// Whether `node` is a tuple pattern `(a, b)`: a group of two or more names.
[[nodiscard]] auto is_tuple_pattern(const expression& node) -> bool;

// Whether `node` is the pattern `_`.
[[nodiscard]] auto is_wildcard_pattern(const std::vector<expression_token>& tokens,
                                       const expression& node) -> bool;

// Parses statement or expression text. Tokens that do not form an expression
// stay in the root sequence, so rendering every token reproduces the input.
// With `try_operator`, `?` is a postfix operator (the `try-operator` experiment);
// with `comprehension`, `for` begins a for-comprehension (the `comprehension`
// experiment); with `bindings`, an `is` may be a `pattern_test` (the
// `condition-bindings` experiment).
[[nodiscard]] auto parse_expression_text(std::string_view text, bool try_operator = false,
                                         bool comprehension = false, bool bindings = false)
    -> expression_text;

// Parses `text` as one inspect alternative, `is pattern = value;`, the root;
// nullopt when it is not exactly one.
[[nodiscard]] auto parse_inspect_alternative(std::string_view text, bool try_operator = false,
                                             bool comprehension = false)
    -> std::optional<expression_text>;

// The index of the first token of an operand that directly follows another
// operand, as in `x y`; adjacent string literals are one literal.
[[nodiscard]] auto find_juxtaposed_operand(const expression_text& text)
    -> std::optional<std::size_t>;

// The first `for` token that does not begin a for-comprehension (experiment
// `comprehension`).
// The `{` of a Cpp1 braced list that begins an argument or list item.
[[nodiscard]] auto find_braced_list(const expression_text& text) -> std::optional<std::size_t>;

// The `(` of the first list `(a, b)` in `text` used where Cpp2, having no
// comma operator, takes one value: an operand, argument, condition or index.
// A list is a value after an assignment's or a typed value's `=`, as a list
// item, as a for range, as a tuple pattern, and as `text` itself when `list`.
[[nodiscard]] auto find_comma_list(const expression_text& text, bool list)
    -> std::optional<std::size_t>;

// The first postfix `*` or `&` in `text` that is spaced from its operand and
// followed by a token Cpp1 reads as a prefix operator, as in `a * -1`, which
// reads as `(a*) - 1`, or `a * *p`, which reads as `(a*) * p`.
[[nodiscard]] auto find_misleading_postfix(const expression_text& text)
    -> std::optional<std::size_t>;

// The operator of the first pointer arithmetic on an address `x&` in `text`:
// a binary `+` or `-` operand, `x&[i]`, `x&++` or `x&--`, also parenthesized.
[[nodiscard]] auto find_address_arithmetic(const expression_text& text)
    -> std::optional<std::size_t>;

// An operand of a binary or prefix operator that is a fundamental type name.
struct type_operand_use final {
    std::size_t token = 0;
    // The operand before it is an `is` test, as `double` in `v is int | double`.
    bool after_is = false;
};

// The first operand in `text` that is a fundamental type name.
[[nodiscard]] auto find_type_operand(const expression_text& text)
    -> std::optional<type_operand_use>;

// The operator of the first assignment in `text` that is used as a value,
// as in `a = b = 3` or `f(a = 3)`; when `statement`, `text` itself may be one.
[[nodiscard]] auto find_value_assignment(const expression_text& text, bool statement)
    -> std::optional<std::size_t>;

// The list `(a, b, ...)` of two or more items that is all of `text` apart
// from trailing `;`s, or null; `(x)` and `(x,)` are grouping.
[[nodiscard]] auto whole_list(const expression_text& text) -> const expression*;

// The root tokens that are part of no parsed expression, other than a leading
// `throw` and the trailing `;`s.
[[nodiscard]] auto find_unparsed_tokens(const expression_text& text) -> std::vector<std::size_t>;

[[nodiscard]] auto find_stray_for(const expression_text& text) -> std::optional<std::size_t>;

// Expression source text and its parse. Text taken from the source as written
// knows where it begins.
class parsed_expression final {
public:
    parsed_expression() = default;
    explicit parsed_expression(std::string source, bool try_operator = false,
                               bool comprehension = false, bool bindings = false,
                               std::optional<source_position> begin = std::nullopt)
        : source_{std::move(source)},
          tree_{parse_expression_text(source_, try_operator, comprehension, bindings)},
          begin_{begin} {}

    [[nodiscard]] auto source() const noexcept -> const std::string& { return source_; }
    [[nodiscard]] auto tree() const noexcept -> const expression_text& { return tree_; }
    [[nodiscard]] auto empty() const noexcept -> bool { return source_.empty(); }
    // The source position of token `index` of the tree, when the text knows where it begins.
    [[nodiscard]] auto position_of(std::size_t index) const -> std::optional<source_position>;

private:
    std::string source_;
    expression_text tree_;
    std::optional<source_position> begin_;
};

}
