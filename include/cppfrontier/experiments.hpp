#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <optional>
#include <string_view>

// Opt-in language experiments; docs/EXPERIMENTS.md describes how to add and
// remove one.
namespace cppfrontier {

struct experiment final {
    std::string_view name;
    std::string_view description;
    // The docs/LANGUAGE.md topic or docs/OPEN_QUESTIONS.md entry that the experiment explores.
    std::string_view decision;
    // The experiments this one builds on; enabling it enables them too.
    std::array<std::string_view, 2> builds_on{};
};

// Every experiment, each off by default. With none, this is `std::array<experiment, 0>{}`.
inline constexpr auto all_experiments = std::array{
    experiment{"nonnegative-literal-comparisons",
               "allows a decimal integer literal without a 'u' suffix in a mixed-sign <, <=, > "
               "or >=, so 's > 0' is accepted for an unsigned 's'",
               "LANGUAGE.md Comparisons"},
    experiment{"move-only-to-known-callees",
               "moves a definite last use that is a call argument only when the callee is a "
               "function of this program taking it by 'copy', 'move' or 'forward'",
               "LANGUAGE.md Moves and last use"},
    experiment{"defer",
               "adds 'defer statement;' and 'defer { ... }', which run when the enclosing block "
               "exits",
               "none"},
    experiment{"unsafe",
               "adds 'unsafe { ... }', a block whose subscript, null, division and comparison "
               "checks are not emitted",
               "none"},
    experiment{"destructuring",
               "adds '(a, b) := value;', which binds the elements of a tuple-like value to "
               "deduced locals",
               "none"},
    experiment{"trait",
               "adds the @trait and @impl metafunctions: a trait becomes a concept, an archetype "
               "and a dyn_ type-erased value",
               "none"},
    experiment{"given",
               "adds the @given metafunction, which implements a @trait for a type outside "
               "that type",
               "none",
               {"trait"}},
    experiment{"adt",
               "adds the @adt metafunction, a sum type whose cases are nested types with fields, "
               "held in a std::variant",
               "none"},
    experiment{"patterns",
               "adds destructuring 'is T(a, _)', alternatives 'is A | B', bindings 'is T as c' "
               "and guards 'if g' to inspect, and exhaustive @adt and @enum inspects",
               "none"},
    experiment{"try-operator",
               "adds the postfix '?' operator, which unwraps a std::expected or std::optional or "
               "returns its failure from the function",
               "none"},
    experiment{"newtype",
               "adds the @newtype, @quantity and @unit metafunctions: distinct value types over "
               "an underlying type that do not convert implicitly",
               "none"},
    experiment{"checked-arithmetic",
               "lowers +, -, *, unary -, +=, -=, *=, ++ and -- on known integers to "
               "cpp2::checked_* helpers that terminate on overflow",
               "none"},
    experiment{"delegate",
               "adds the @delegate<member> metafunction, which forwards the public functions of "
               "a data member's type",
               "none"},
    experiment{"use-after-move",
               "diagnoses a mention of a local or parameter after an explicit move on every path "
               "(use-after-move), some path (use-after-maybe-move) or an earlier loop iteration "
               "(move-in-loop)",
               "none"},
    experiment{"linear",
               "adds the @linear metafunction: a move-only type whose values must be consumed "
               "exactly once on every path (linear-not-consumed, linear-consumed-twice)",
               "none"},
    experiment{"exclusivity",
               "rejects a call that passes an object inout or out while another argument "
               "mentions it (exclusivity-violation)",
               "none"},
    experiment{"interface-defaults",
               "lets an @interface function have a body, which makes it virtual with that body "
               "as its default instead of pure virtual",
               "none"},
    experiment{"sealed",
               "adds the @sealed metafunction, which closes a polymorphic hierarchy to this "
               "translation unit so an inspect covering every derived type needs no 'is _'",
               "none",
               {"patterns"}},
    experiment{"lazy",
               "adds 'name: lazy T = init;' at namespace or type scope, an object whose "
               "initializer runs on its first use, exactly once",
               "none"},
    experiment{"comprehension",
               "adds the for-comprehension 'for range do (x) [if guard] yield value', a lazy "
               "view over a range or a monadic chain over an optional or expected",
               "none"},
    experiment{"type-operators",
               "makes 'A | B' a union type lowered to std::variant, and 'A & B' and 'A | B' "
               "a conjunction or disjunction of concepts in a constraint",
               "none"},
    experiment{"condition-bindings",
               "lets an operand of an if or while condition's top-level && chain be a pattern "
               "test that binds, 'v is T as n', 'v is T(a, b)' or 'v is (a, b)', in scope in "
               "the later operands and the guarded body",
               "none",
               {"patterns"}},
    experiment{"let-else",
               "adds 'name := value as T else { ... }', which declares 'name' when 'value is T' "
               "and otherwise runs a block that must not complete normally",
               "none"},
    experiment{"next-list",
               "lets a loop's next clause be a list 'next (a += 1, b++)', whose items run in "
               "order at the end of each iteration",
               "LANGUAGE.md Statements"},
};

[[nodiscard]] constexpr auto find_experiment(std::string_view name) -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < all_experiments.size(); ++index) {
        if (all_experiments[index].name == name) return index;
    }
    return std::nullopt;
}

// Each experiment builds only on experiments listed before it.
[[nodiscard]] consteval auto bases_precede() -> bool {
    for (std::size_t index = 0; index < all_experiments.size(); ++index) {
        for (const auto base : all_experiments[index].builds_on) {
            if (base.empty()) continue;
            const auto found = find_experiment(base);
            if (!found || *found >= index) return false;
        }
    }
    return true;
}
static_assert(bases_precede(), "an experiment builds on an unknown or later experiment");

// An experiment named in the source of a stage; an unknown name does not compile.
class experiment_id final {
public:
    consteval experiment_id(const char* name) : index_{checked(name)} {}
    [[nodiscard]] constexpr auto index() const noexcept -> std::size_t { return index_; }

private:
    static consteval auto checked(std::string_view name) -> std::size_t {
        const auto found = find_experiment(name);
        if (!found) throw "unknown experiment name";
        return *found;
    }

    std::size_t index_;
};

class experiment_set final {
public:
    // Enables experiment `name` and those it builds on, or every one for "all";
    // false if there is none.
    auto enable(std::string_view name) -> bool {
        if (name == "all") {
            enabled_.set();
            return true;
        }
        const auto found = find_experiment(name);
        if (!found) return false;
        enabled_.set(*found);
        for (const auto base : all_experiments[*found].builds_on) {
            if (!base.empty()) enable(base);
        }
        return true;
    }
    [[nodiscard]] auto enabled(experiment_id id) const -> bool { return enabled_.test(id.index()); }

private:
    std::bitset<all_experiments.size()> enabled_;
};

}
