#!/usr/bin/perl -0
# Rewrites upstream tests where cppfrontier's rules differ
# (docs/UPSTREAM_AUDIT.md), keeping line numbers:
# - `for (expr) do` / `for (expr) next` with a single expression becomes
#   `for (std::views::single(expr))`: upstream iterates such a range once,
#   while cppfrontier reads `(expr)` as grouping.
# - An assignment used as a value becomes the assignment alone:
#   `return a = b;` and `_: T = a = b;` become `a = b;` (the test only uses
#   them to move `b`).
my $paren = qr/(\((?:[^()]++|(?-1))*\))/;
local $/; $_ = <>;
s{for \s*$paren(?=\s*(?:do|next)\b)}{
  my $inner = substr($1, 1, -1);
  (my $flat = $inner) =~ s/$paren//g;
  $flat =~ /,/ ? "for ($inner)" : "for (std::views::single($inner))"
}ge;
s{^(\s*)(?:return |_: \w+ = )(\S+ = [^;=]+;)}{$1$2}mg;
print;
