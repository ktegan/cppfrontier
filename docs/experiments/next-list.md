# `next-list`

Enable with `-experiment=next-list`.
Explores: [`LANGUAGE.md` "Statements"](../LANGUAGE.md#statements), which keeps a `next` clause one expression.

## What it enables

A loop's `next` clause may be a list `next (e1, e2, ...)` of two or more
expressions. At the end of each iteration, including one ended by
`continue`, they run in order, `e1` first, in `while`, `do` and range `for`
loops alike.

## Why it is useful

Loops that walk two things at once are common: two indices meeting in the
middle, an index and a running total, an iterator and a counter. With one
`next` expression, the second step goes at the end of the body, where a
`continue` skips it, or the loop is rewritten around a `for` range. A list
keeps all the steps in the loop header and runs them on every path to the
next iteration.

Without the experiment, `next (a += 1, b++)` is an error that names the
rule, since Cpp2 has no comma operator. Upstream accepts the list only
because it emits it as a Cpp1 comma operator, which an overloaded
`operator,` takes over.

## Prior art

- **C and C++** write the update of a `for` loop as one expression, so two
  steps use the comma operator: `for (i = 0, j = n - 1; i < j; i++, j--)`.
  In C++ the operator can be overloaded
  ([cppreference](https://en.cppreference.com/w/cpp/language/operator_other#Built-in_comma_operator)),
  and then the update calls it.
- **Java** has no comma operator; the update of a basic `for` is a
  *StatementExpressionList*, a list of expression statements run in order
  ([JLS 14.14.1](https://docs.oracle.com/javase/specs/jls/se21/html/jls-14.html#jls-14.14.1)).
  C#'s *for_iterator* is the same
  ([C# spec 13.9.4](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/statements#1394-the-for-statement)).
  This experiment follows them: a list of statements, not an operator.
- **JavaScript** follows C, with its comma operator in the update
  ([MDN](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Operators/Comma_operator)).
- **Go** has neither: its post statement is one simple statement, and
  `++` is a statement, so two steps are a parallel assignment
  `i, j = i+1, j-1`
  ([spec](https://go.dev/ref/spec#For_statements),
  [Effective Go](https://go.dev/doc/effective_go#for)). Cpp2 has no
  parallel assignment, so a list is the way to write two steps.

## Example

```cpp
#include <iostream>
#include <string>

is_palindrome: (s: std::string) -> bool = {
    i := 0;
    j := s.ssize() - 1;
    while i < j next (i++, j--) {
        if s[i] != s[j] { return false; }
    }
    return true;
}

main: () = {
    std::cout << is_palindrome("level") << is_palindrome("levels") << "\n";
    k := 0;
    sum := 0;
    while k < 6 next (k++, sum += k) {
        if k % 2 == 0 { continue; }
        std::cout << "odd " << k << "\n";
    }
    std::cout << "sum " << sum << "\n";
}
```

Output:

```
10
odd 1
odd 3
odd 5
sum 21
```

## Lowering

Each item is one Cpp1 statement, so the order is the written order and no
`operator,` is called. A `while` loop runs them in a lambda in the Cpp1
`for` increment:

```cpp
for (; cpp2::impl::cmp_less(i,j); [&]{ ++i; --j; }()) {
```

A `do` loop runs them in the lambda that its single `next` expression uses,
before the condition:

```cpp
} while ([&]{ n += 1; static_cast<void>(n); return true; }() && cpp2::impl::cmp_less(n,2));
```

A range `for` loop runs them as statements after the body, where a single
`next` expression goes:

```cpp
for (auto const& x : { 1, 2, 3 }) {
    do {
        ...
    } while (false);
    add(t, -1);
    add(t, -2);
}
```

## Grammar

```
next-clause:
    'next' assignment-expression
    'next' '(' next-item ',' next-item-list ','? ')'      // added

next-item-list:
    next-item
    next-item-list ',' next-item

next-item:
    assignment-expression
```

The list is the whole clause: `next (a, b) + 1` is one expression, and a
list there is the usual `comma-operator` error. `next (x)` and `next (x,)`
are one parenthesized expression, as `(x)` is grouping everywhere
(`LANGUAGE.md` "Expressions"). There is no lexing change. The statement parser
(`check_next` in `src/statement_parser.cpp`) recognizes a clause whose
expression is one list (`whole_list`) and checks each item as it checks a
single `next` expression.

## Rules and diagnostics

- An item is checked as a `next` clause: it may be one assignment, in
  parentheses or not, but not a chained assignment (`chained-assignment`)
  or a list (`comma-operator`).
- `_ = x` as an item discards `x`, as a whole `next _ = x` does.
- The items are mentions like any other `next` expression for the safety
  analyses: they run on every iteration, so nothing in them is a definite
  last use moved out of the loop.
- Without the experiment, a list is `next-clause-list`:
  `a 'next' clause is one expression, not a list '(a, b)' -
  '-experiment=next-list' runs each item in order`.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`next_list.cpp2`](../../tests/ports/experiments/next_list.cpp2),
[`next_list_error.cpp2`](../../tests/ports/experiments/next_list_error.cpp2),
[`next_list_default.cpp2`](../../tests/ports/experiments/next_list_default.cpp2);
[`experiment_next_list_test.cpp`](../../tests/unit/experiment_next_list_test.cpp).
