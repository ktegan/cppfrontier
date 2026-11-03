#!/usr/bin/env python3
"""Differential fuzzer: generate random valid Cpp2 programs that print their
results, translate each with cppfrontier and with upstream cppfront, compile
both with g++, run both, and compare translate/compile/run status and output.

The generator is grammar-driven, in the spirit of Csmith and YARPGen: it keeps
an environment of typed variables, functions and types, so every program is
well-formed, and tracks a bound on every integer expression, so no program
overflows, divides by zero or indexes out of range. Programs are deterministic
for a given seed. When a pair differs, the program is reduced by deleting lines
and brace-matched blocks while the difference persists; the original and the
reduced program are saved to the output directory.

Constructs listed in docs/UPSTREAM_DIFFERENCES.md, and features upstream
lacks, are not generated; KNOWN lists the remaining tolerated differences.

usage: tools/diff-fuzz.py [--seeds A:B | --seed N] [--jobs N] [--out DIR]
                          [--bin PATH] [--upstream-bin PATH] [--upstream DIR]
                          [--print] [--no-reduce] [--keep] [--file PROGRAM]
"""
import argparse, collections, concurrent.futures, hashlib, os, random, re, shutil, subprocess, sys

repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CAP = 2_000_000_000          # every integer expression stays within +-CAP (fits int32)
WORDS = ['ab', 'cd', 'xyz', 'q', 'hello', 'w0', 'zz', '']


def strify(e):
    """A string expression as a std::string operand."""
    return f'std::string({e})' if e.startswith('"') else f'({e})'


def bare(e):
    """`e` without parentheses around all of it: upstream lowers `_ = (e);` to
    `static_cast<void>({ e })`, which does not compile."""
    while e.startswith('(') and e.endswith(')'):
        depth = 0
        for i, c in enumerate(e):
            depth += (c == '(') - (c == ')')
            if depth == 0 and i < len(e) - 1:
                return e
        if e[1:].startswith('inspect'):
            return e        # upstream rejects an `inspect` that is a whole initializer
        e = e[1:-1]
    return e


class Var:
    def __init__(self, name, ty, mut=True, **info):
        self.name, self.ty, self.mut, self.info = name, ty, mut, info


class Func:
    """A generated function. kind selects how calls are written."""
    def __init__(self, name, kind, params, ret, **info):
        self.name, self.kind, self.params, self.ret, self.info = name, kind, params, ret, info


class TypeInfo:
    def __init__(self, name, kind, **info):
        self.name, self.kind, self.info = name, kind, info
        self.int_getters = []    # (format with {o}, bound)
        self.str_getters = []    # format with {o}
        self.mutators = []       # callables (gen, env, o) -> statement
        self.ctors = []          # callables (gen, env) -> initializer text after '='
        self.eq = self.ordered = False
        self.copyable = True
        self.tracer = False      # copies and moves print


class Env:
    def __init__(self, parent=None):
        self.parent, self.vars = parent, []

    def all(self):
        e, out = self, []
        while e:
            out.extend(e.vars)
            e = e.parent
        return out


class Ctx:
    """Statement context: environment, nesting, enclosing loops and function."""
    def __init__(self, env, depth, loops=(), func=None, pure=False):
        self.env, self.depth, self.loops, self.func, self.pure = env, depth, loops, func, pure

    def child(self, loop=None):
        loops = self.loops + ((loop,) if loop else ())
        return Ctx(Env(self.env), self.depth + 1, loops, self.func, self.pure)


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.n = 0
        self.decls = []          # namespace-scope declarations, each a list of lines
        self.funcs = []
        self.types = {}
        self.globals = Env()
        self.prints = 0
        self.noufcs = 0          # inside an inspect: no member or UFCS calls
        self.noufcs = 0          # inside an inspect: no member access or UFCS calls (both translators fail to compile some)
    # ---- helpers
    def fresh(self, prefix):
        self.n += 1
        return f'{prefix}{self.n}'

    def chance(self, p):
        return self.rng.random() < p

    def pick(self, seq):
        return self.rng.choice(seq)

    def vars_of(self, env, pred):
        return [v for v in env.all() if pred(v)]

    @staticmethod
    def wrap(e):
        text, bound = e
        return (f'(({text}) % 1000)', 999) if bound > 999 else e

    # ---- expressions: each returns (text, bound) for integers, text otherwise
    def lit(self):
        v = self.rng.randint(-20, 100) if self.chance(0.8) else self.rng.randint(-999, 999)
        return (f'({v})' if v < 0 else str(v)), abs(v)

    def int_expr(self, env, depth, simple=False):
        r = self.rng
        opts = [('lit', 3)]
        ints = self.vars_of(env, lambda v: v.ty == 'int')
        if ints: opts.append(('var', 6))
        if depth > 0:
            opts += [('bin', 6), ('div', 1)]
            if self.int_funcs(simple): opts.append(('call', 4))
            if [f for f in self.funcs if f.ret == 'nr'] and not self.noufcs: opts.append(('nrcall', 1))
            if not simple: opts += [('inspect', 1), ('range', 1)]
        objs = [v for v in env.all() if v.ty in self.types and self.getters(v.ty, simple)]
        if objs: opts.append(('getter', 3))
        if self.vars_of(env, lambda v: v.ty == 'vec'): opts.append(('vec', 2))
        if self.vars_of(env, lambda v: v.ty == 'str'): opts.append(('strlen', 1))
        if self.vars_of(env, lambda v: v.ty == 'fn'): opts.append(('lambda', 2))
        if self.vars_of(env, lambda v: v.ty == 'nr') and not self.noufcs: opts.append(('field', 2))
        if self.vars_of(env, lambda v: v.ty in ('opt', 'variant')) and not simple: opts.append(('as', 1))
        kind = self.weighted(opts)
        if kind == 'lit':
            return self.lit()
        if kind == 'var':
            v = self.pick(ints)
            return v.name, v.info.get('bound', 999)
        if kind == 'bin':
            op = self.pick(['+', '-', '*', '+', '-'])
            a, b = self.int_expr(env, depth - 1, simple), self.int_expr(env, depth - 1, simple)
            if op == '*':
                if a[1] * b[1] > CAP:
                    a, b = self.wrap(a), self.wrap(b)
                bound = a[1] * b[1]
            else:
                if a[1] + b[1] > CAP:
                    a, b = self.wrap(a), self.wrap(b)
                bound = a[1] + b[1]
            return f'({a[0]} {op} {b[0]})', bound
        if kind == 'div':
            a = self.int_expr(env, depth - 1, simple)
            op = self.pick(['/', '%'])
            if self.chance(0.6):
                d = r.randint(1, 9)
                return f'({a[0]} {op} {d})', (a[1] if op == '/' else min(a[1], d - 1))
            b = self.int_expr(env, depth - 1, simple)
            divisor = f'({b[0]} % 7 + 8)'
            return f'({a[0]} {op} {divisor})', (a[1] // 2 if op == '/' else min(a[1], 13))
        if kind == 'call':
            return self.call(env, depth, self.pick(self.int_funcs(simple)), simple)
        if kind == 'nrcall':
            f = self.pick([f for f in self.funcs if f.ret == 'nr'])
            text, _ = self.call(env, depth, f, simple)
            return f'{text}.{self.pick(f.info["fields"])}', 999
        if kind == 'getter':
            o = self.pick(objs)
            fmt, bound = self.pick(self.getters(o.ty, simple))
            return fmt.format(o=o.name), bound
        if kind == 'vec':
            v = self.pick(self.vars_of(env, lambda v: v.ty == 'vec'))
            n = v.info['size']
            c = r.randrange(4)
            if c == 0:
                return (f'std::ssize({v.name})' if self.noufcs else f'{v.name}.ssize()'), n
            if c == 1:
                return f'{v.name}[{r.randrange(n)}]', 999
            if c == 2 and depth > 0:
                i = self.int_expr(env, depth - 1, simple)
                return f'{v.name}[({i[0]} % {n} + {n}) % {n}]', 999
            return (f'{v.name}[0]' if self.noufcs else f'{v.name}.front()'), 999
        if kind == 'strlen':
            v = self.pick(self.vars_of(env, lambda v: v.ty == 'str'))
            return (f'{v.name}.ssize()' if self.chance(0.7) and not self.noufcs else f'std::ssize({v.name})'), 40
        if kind == 'lambda':
            v = self.pick(self.vars_of(env, lambda v: v.ty == 'fn'))
            a = self.int_expr(env, max(depth - 1, 0), simple)
            return f'{v.name}({a[0]})', 999
        if kind == 'field':
            v = self.pick(self.vars_of(env, lambda v: v.ty == 'nr'))
            return f'{v.name}.{self.pick(v.info["fields"])}', 999
        if kind == 'as':
            v = self.pick(self.vars_of(env, lambda v: v.ty in ('opt', 'variant')))
            if v.ty == 'opt':
                return (f'(inspect {v.name} -> i64 {{ is i64 = {v.name} as i64; is _ = 0; }})'
                        if self.chance(0.5) or not v.info.get('set') else f'({v.name} as i64)'), 999
            return f'(inspect {v.name} -> i64 {{ is i64 = {v.name} as i64; is _ = {self.lit()[0]}; }})', 999
        if kind == 'inspect':
            # upstream's inspect does not compile with a UFCS call inside
            self.noufcs += 1
            s = self.int_expr(env, depth - 1, True)
            alts, bound = [], 0
            for _ in range(r.randint(1, 3)):
                val = self.int_expr(env, depth - 1, True)
                bound = max(bound, val[1])
                pat = self.pick([self.pattern(), f'(:(q) = q < {r.randint(-5, 50)};)',
                                 f'({self.lit()[0]})'])
                alts.append(f'is {pat} = {val[0]};')
            last = self.int_expr(env, depth - 1, True)
            self.noufcs -= 1
            # parenthesized: upstream takes only an inspect expression that is not a whole initializer
            return f'(inspect {s[0]} -> i64 {{ {" ".join(alts)} is _ = {last[0]}; }})', max(bound, last[1])
        if kind == 'range':
            a = r.randint(-5, 10)
            b = a + r.randint(0, 12)
            op = self.pick(['..<', '..='])
            return f'({self.num(a)} {op} {b}).sum()', 300
        raise AssertionError(kind)

    def int_funcs(self, simple):
        # an interpolation cannot contain a string literal
        return [f for f in self.funcs if f.ret in ('int', 'nr1') and not (simple and ('str' in f.params or 'fstr' in f.params))]

    def getters(self, ty, simple):
        return [g for g in self.types[ty].int_getters
                if not (simple and 'inspect' in g[0]) and not (self.noufcs and ('(' in g[0] or '.' in g[0]))]

    @staticmethod
    def num(v):
        # upstream reads '-5 ..< 2' as '-(5 ..< 2)'
        return f'({v})' if v < 0 else str(v)

    def pattern(self):
        v = self.rng.randint(-3, 10)
        return f'({v})' if v < 0 else str(v)

    def weighted(self, opts):
        total = sum(w for _, w in opts)
        x = self.rng.uniform(0, total)
        for k, w in opts:
            x -= w
            if x <= 0:
                return k
        return opts[-1][0]

    def arg(self, env, depth, ty, simple):
        if ty == 'int':
            return self.wrap(self.int_expr(env, depth - 1, simple))[0]
        if ty == 'str':
            return self.str_expr(env, depth - 1, simple)
        if ty == 'fstr':
            return strify(self.str_expr(env, depth - 1, simple))
        if ty in self.types:
            objs = self.vars_of(env, lambda v: v.ty == ty)
            return self.pick(objs).name if objs else None
        raise AssertionError(ty)

    def call(self, env, depth, f, simple=False):
        """A call to an integer-valued function."""
        if f.kind == 'pack':
            args = [self.wrap(self.int_expr(env, depth - 1, simple))[0] for _ in range(self.rng.randint(0, 3))]
            return f'{f.name}({", ".join(args)})', f.info['bound'](len(args))
        if f.kind == 'count':
            args = [self.str_expr(env, 0) if self.chance(0.3) and not simple else self.lit()[0]
                    for _ in range(self.rng.randint(0, 4))]
            return f'{f.name}({", ".join(args)})', 4
        args = []
        for ty in f.params:
            a = self.arg(env, depth, ty, simple)
            if a is None:
                return self.lit()
            args.append(a)
        targs = ''
        if f.kind == 'nttp':
            targs = f'<{self.rng.randint(0, 9)}>'
        elif f.kind == 'tmpl':
            targs = '<i64>'
        name = f.name
        if (args and f.info.get('ufcs') and not targs and self.chance(0.4) and not self.noufcs
                and re.fullmatch(r'[A-Za-z_]\w*|\(.*\)', args[0])):
            return f'{args[0]}.{name}({", ".join(args[1:])})', f.info['bound']
        return f'{name}{targs}({", ".join(args)})', f.info['bound']

    def bool_expr(self, env, depth, simple=False):
        r = self.rng
        opts = [('cmp', 6)]
        if depth > 0: opts += [('and', 2), ('not', 1)]
        if depth > 0 and not simple: opts += [('chain', 1), ('is', 1), ('contains', 1)]
        bools = self.vars_of(env, lambda v: v.ty == 'bool')
        if bools: opts.append(('var', 2))
        eqs = [v for v in env.all() if v.ty in self.types and self.types[v.ty].eq]
        if eqs: opts.append(('eq', 2))
        if self.vars_of(env, lambda v: v.ty in ('str', 'vec')): opts.append(('empty', 1))
        if self.vars_of(env, lambda v: v.ty in ('opt', 'variant')) and not simple: opts.append(('isvar', 1))
        enums = [v for v in env.all() if v.ty in self.types and self.types[v.ty].kind == 'enum']
        if enums: opts.append(('enum', 2))
        kind = self.weighted(opts)
        if kind == 'cmp':
            a, b = self.int_expr(env, depth, simple), self.int_expr(env, depth, simple)
            return f'({a[0]} {self.pick(["<", "<=", ">", ">=", "==", "!="])} {b[0]})'
        if kind == 'chain':
            ops = self.pick([['<', '<='], ['>', '>='], ['==']])
            es = [self.int_expr(env, depth - 1, simple)[0] for _ in range(3)]
            return f'({es[0]} {self.pick(ops)} {es[1]} {self.pick(ops)} {es[2]})'
        if kind == 'and':
            return f'({self.bool_expr(env, depth - 1, simple)} {self.pick(["&&", "||"])} {self.bool_expr(env, depth - 1, simple)})'
        if kind == 'not':
            return f'!{self.bool_expr(env, depth - 1, simple)}'
        if kind == 'var':
            return self.pick(bools).name
        if kind == 'is':
            a = self.int_expr(env, depth - 1, True)
            return f'({a[0]} is {self.pick([self.pattern(), "(:(q) = q > 3;)", "i64", "int"])})'
        if kind == 'contains':
            a = r.randint(-5, 10)
            b = self.int_expr(env, depth - 1, simple)
            return f'({self.num(a)} {self.pick(["..<", "..="])} {a + r.randint(0, 9)}).contains({b[0]})'
        if kind == 'eq':
            o = self.pick(eqs)
            others = self.vars_of(env, lambda v: v.ty == o.ty)
            op = self.pick(['==', '!='] + (['<', '>=', '>'] if self.types[o.ty].ordered else []))
            return f'({o.name} {op} {self.pick(others).name})'
        if kind == 'empty':
            v = self.pick(self.vars_of(env, lambda v: v.ty in ('str', 'vec')))
            return f'{v.name}.empty()'
        if kind == 'isvar':
            v = self.pick(self.vars_of(env, lambda v: v.ty in ('opt', 'variant')))
            if v.ty == 'opt':
                return f'({v.name} is i64)'
            return f'({v.name} is {self.pick(["i64", "std::string"])})'
        if kind == 'enum':
            v = self.pick(enums)
            t = self.types[v.ty]
            return f'({v.name} {self.pick(["==", "!="])} {t.name}::{self.pick(t.info["values"])})'
        raise AssertionError(kind)

    def str_expr(self, env, depth, simple=False):
        r = self.rng
        opts = [('lit', 3)]
        strs = self.vars_of(env, lambda v: v.ty == 'str')
        if strs: opts.append(('var', 5))
        if depth > 0: opts += [('cat', 2), ('tostr', 1)]
        if depth > 0 and not simple: opts += [('interp', 3), ('inspect', 1)]
        getters = [v for v in env.all() if v.ty in self.types and self.types[v.ty].str_getters]
        if getters: opts.append(('getter', 2))
        sfuncs = [f for f in self.funcs if f.ret == 'str']
        if sfuncs and depth > 0: opts.append(('call', 2))
        kind = self.weighted(opts)
        if kind == 'lit':
            return f'"{self.pick(WORDS)}"' if not simple else f'std::string("{self.pick(WORDS)}")'
        if kind == 'var':
            return self.pick(strs).name
        if kind == 'cat':
            left = self.str_expr(env, depth - 1, simple)
            left = strify(left) if left.startswith('"') else left
            return f'({left} + {self.str_expr(env, depth - 1, simple)})'
        if kind == 'tostr':
            return f'std::to_string({self.int_expr(env, depth - 1, simple)[0]})'
        if kind == 'interp':
            return self.interp(env, depth)
        if kind == 'inspect':
            self.noufcs += 1
            s = self.int_expr(env, depth - 1, True)
            self.noufcs -= 1
            alts = ' '.join(f'is {self.pattern()} = "{self.pick(WORDS)}";' for _ in range(r.randint(1, 3)))
            return f'(inspect {s[0]} -> std::string {{ {alts} is _ = "{self.pick(WORDS)}"; }})'
        if kind == 'getter':
            o = self.pick(getters)
            return self.pick(self.types[o.ty].str_getters).format(o=o.name)
        if kind == 'call':
            f = self.pick(sfuncs)
            args = [self.arg(env, depth, ty, simple) for ty in f.params]
            if f.kind == 'move':
                args[0] = self.move_arg(env, depth)
            return f'{f.name}({", ".join(args)})'
        raise AssertionError(kind)

    def move_arg(self, env, depth):
        """An argument for a move parameter: an rvalue, or a local moved explicitly."""
        strs = self.vars_of(env, lambda v: v.ty == 'str' and v.mut and not v.info.get('global'))
        if strs and self.chance(0.4):
            return 'move ' + self.pick(strs).name
        return f'({strify(self.str_expr(env, depth - 1))} + "{self.pick(WORDS)}")'

    def interp(self, env, depth):
        parts = []
        for _ in range(self.rng.randint(1, 3)):
            if self.chance(0.3):
                e = self.bool_expr(env, max(depth - 1, 0), True)
            else:
                e = self.int_expr(env, max(depth - 1, 0), True)[0]
                if self.chance(0.2):
                    e += ':' + self.pick(['>4', '<3', '^5', '04', '+'])
            parts.append(f'{self.pick(["", "v", "-", " "])}({e})$')
        return '"' + ' '.join(parts) + '"'

    # ---- statements: each returns a list of lines without indentation
    def block(self, ctx, n):
        lines = []
        for _ in range(n):
            lines += self.stmt(ctx)
        # a modified value may not be discarded silently, so read each one again
        for v in ctx.env.vars:
            if v.info.get('modified'):
                if ctx.pure or v.ty != 'int':
                    lines.append(f'_ = {v.name};' if v.ty == 'int' else f'_ = {bare(self.reread(v))};')
                else:
                    self.prints += 1
                    lines.append(f'std::cout << "p{self.prints}: (({v.name}))$\\n";')
                v.info['modified'] = False
        return lines

    def reread(self, v):
        getters = self.types[v.ty].int_getters if v.ty in self.types else []
        return getters[0][0].format(o=v.name) if getters else v.name

    def modified(self, env, name):
        for v in env.all():
            if v.name == name:
                v.info['modified'] = True
                return

    def body(self, lines):
        return ['    ' + l for l in lines]

    def stmt(self, ctx):
        env, r = ctx.env, self.rng
        mut_ints = self.mut_ints(ctx)
        opts = [('decl', 5), ('assign', 4 if mut_ints else 0)]
        if not ctx.pure: opts.append(('print', 5))
        if ctx.depth < 3:
            opts += [('if', 3), ('while', 1.5), ('do', 1), ('for', 2), ('block', 0.5)]
            if ctx.loops: opts.append(('jump', 1.5))
        if self.out_ints(ctx) and [f for f in self.funcs if f.kind in ('inout', 'out')]: opts.append(('outcall', 3))
        if [f for f in self.funcs if f.kind == 'void'] and not ctx.pure: opts.append(('voidcall', 2))
        objs = [v for v in env.all() if v.ty in self.types and v.mut and self.types[v.ty].mutators]
        if objs: opts.append(('mutate', 3))
        if self.vars_of(env, lambda v: v.ty == 'vec' and v.mut): opts.append(('vecset', 1))
        if self.vars_of(env, lambda v: v.ty == 'str' and v.mut): opts.append(('strset', 1))
        if [t for t in self.types.values() if t.tracer] and not ctx.pure: opts.append(('trace', 2))
        opts += [('assert', 0.5), ('discard', 0.5), ('stmtparam', 0.5)]
        if not ctx.pure and ctx.depth < 3: opts.append(('try', 0.5))
        if ctx.func and ctx.func.get('ret') == 'int' and ctx.depth > 1: opts.append(('return', 1))
        kind = self.weighted(opts)
        return getattr(self, 's_' + kind)(ctx)

    def mut_ints(self, ctx):
        # a value-returning function does not assign globals, so calls have no side effects
        return self.vars_of(ctx.env, lambda v: v.ty == 'int' and v.mut and not (ctx.pure and v.info.get('global')))

    def out_ints(self, ctx):
        return [v for v in self.mut_ints(ctx) if not v.info.get('i32')]

    def s_decl(self, ctx):
        env, r = ctx.env, self.rng
        c = self.weighted([('int', 6), ('deferred', 2), ('str', 2), ('vec', 1), ('bool', 1), ('obj', 3),
                           ('lambda', 2), ('nr', 1), ('opt', 0.5), ('variant', 0.5)])
        if c == 'int':
            name = self.fresh('x')
            e = self.wrap(self.int_expr(env, 2))
            form = self.rng.randrange(3)
            # a deduced object may be an int, which an i64 out or inout parameter does not take
            env.vars.append(Var(name, 'int', mut=True, i32=(form == 2)))
            return [[f'{name}: i64 = {e[0]};', f'{name}: i64 = ({e[0]});', f'{name} := {e[0]};'][form]]
        if c == 'deferred':
            name = self.fresh('d')
            lines = [f'{name}: i64;']
            outs = [f for f in self.funcs if f.kind == 'out']
            if outs and self.chance(0.4):
                f = self.pick(outs)
                args = ''.join(', ' + self.wrap(self.int_expr(env, 1))[0] for _ in f.params[1:])
                lines.append(f'{f.name}(out {name}{args});')
            elif self.chance(0.5):
                lines += [f'if {self.bool_expr(env, 1)} {{',
                          f'    {name} = {self.wrap(self.int_expr(env, 1))[0]};',
                          '}', 'else {',
                          f'    {name} = {self.wrap(self.int_expr(env, 1))[0]};', '}']
            else:
                lines.append(f'{name} = {self.wrap(self.int_expr(env, 1))[0]};')
            env.vars.append(Var(name, 'int', modified=True))
            return lines
        if c == 'str':
            name = self.fresh('s')
            e = self.str_expr(env, 2)
            env.vars.append(Var(name, 'str'))
            return [self.pick([f'{name}: std::string = {e};', f'{name}: std::string = {strify(e)}.substr(0, 20);'])]
        if c == 'vec':
            name = self.fresh('v')
            n = r.randint(1, 5)
            # upstream miscompiles an inspect in a list
            elems = [self.wrap(self.int_expr(env, 1, simple=True))[0] for _ in range(n)]
            env.vars.append(Var(name, 'vec', size=n))
            if n == 1:
                return [f'{name}: std::vector<i64> = ();', f'{name}.push_back({elems[0]});']
            return [f'{name}: std::vector<i64> = ({", ".join(elems)});']
        if c == 'bool':
            name = self.fresh('b')
            e = self.bool_expr(env, 2)
            env.vars.append(Var(name, 'bool'))
            return [f'{name} := {e};']
        if c == 'obj':
            ts = [t for t in self.types.values() if t.ctors]
            if not ts:
                return self.s_decl(ctx)
            t = self.pick(ts)
            name = self.fresh('o')
            init = self.pick(t.ctors)(self, env)
            env.vars.append(Var(name, t.name, mut=self.chance(0.85)))
            lines = [f'{name}: {t.info.get("spelling", t.name)} = {init};']
            if t.kind == 'union':
                env.vars[-1].info['modified'] = True
                lines.append(f'{name}.set_{t.info["alt"]}({self.wrap(self.int_expr(env, 1))[0]});')
            return lines
        if c == 'lambda':
            name = self.fresh('l')
            inner = Env()
            inner.vars.append(Var('a', 'int', mut=False))
            caps = self.vars_of(env, lambda v: v.ty == 'int')
            e = self.int_expr(inner, 2, simple=True)
            if caps and self.chance(0.7):
                cap = self.pick(caps).name + '$'
                e = (f'({e[0]} + {cap})', e[1] + 999)
            e = self.wrap(e)
            env.vars.append(Var(name, 'fn'))
            form = r.randrange(3)
            if form == 0:
                return [f'{name} := :(a: i64) -> i64 = {e[0]};']
            if form == 1:
                return [f'{name} := :(a) -> i64 = {{ return {e[0]}; }};']
            return [f'{name} := :(a: i64) -> i64 = {{', f'    return {e[0]};', '};']
        if c == 'nr':
            fs = [f for f in self.funcs if f.ret == 'nr']
            if not fs:
                return self.s_decl(ctx)
            f = self.pick(fs)
            name = self.fresh('r')
            text, _ = self.call(env, 2, f)
            env.vars.append(Var(name, 'nr', mut=False, fields=f.info['fields']))
            return [f'{name} := {text};']
        if c == 'opt':
            name = self.fresh('op')
            if self.chance(0.6):
                init = self.wrap(self.int_expr(env, 1))[0]
                env.vars.append(Var(name, 'opt', mut=False, set=True))
                return [f'{name}: std::optional<i64> = {init};']
            env.vars.append(Var(name, 'opt', mut=False, set=False))
            return [f'{name}: std::optional<i64> = ();']
        if c == 'variant':
            name = self.fresh('vr')
            val = self.wrap(self.int_expr(env, 1))[0] if self.chance(0.5) else f'std::string("{self.pick(WORDS)}")'
            env.vars.append(Var(name, 'variant', mut=False))
            return [f'{name}: std::variant<i64, std::string> = {val};']
        raise AssertionError(c)

    def s_assign(self, ctx):
        env = ctx.env
        v = self.pick(self.mut_ints(ctx))
        c = self.rng.randrange(4)
        if c == 0:
            return [f'{v.name} = {self.wrap(self.int_expr(env, 2))[0]};']
        if c == 1:
            e = self.int_expr(env, 1)
            e = (f'({v.name} + {e[0]})', e[1] + 999)
            return [f'{v.name} = {self.wrap(e)[0]};']
        if c == 2:
            return [f'{v.name} {self.pick(["+=", "-=", "*="])} {self.rng.randint(0, 9)};', f'{v.name} %= 1000;']
        return [f'{v.name}++;' if self.chance(0.5) else f'{v.name}--;', f'{v.name} %= 1000;']

    def s_print(self, ctx):
        env = ctx.env
        self.prints += 1
        label = f'p{self.prints}'
        c = self.rng.randrange(5)
        if c == 0:
            return [f'std::cout << "{label}: " << {self.int_expr(env, 3)[0]} << "\\n";']
        if c == 1:
            return [f'std::cout << "{label}: " << {self.bool_expr(env, 2)} << "\\n";']
        if c == 2:
            return [f'std::cout << "{label}: " << {self.str_expr(env, 2)} << "\\n";']
        return [f'std::cout << "{label}: {self.interp(env, 3)[1:-1]}\\n";']

    def s_if(self, ctx):
        lines = [f'if {self.bool_expr(ctx.env, 2)} {{'] + self.body(self.block(ctx.child(), self.rng.randint(1, 3)))
        c = self.rng.randrange(3)
        if c == 1:
            lines += ['}', 'else {'] + self.body(self.block(ctx.child(), self.rng.randint(1, 3)))
        elif c == 2:
            lines += ['}', f'else if {self.bool_expr(ctx.env, 1)} {{'] + self.body(self.block(ctx.child(), self.rng.randint(1, 2)))
        return lines + ['}']

    def label(self):
        return self.fresh('lb') if self.chance(0.4) else None

    def counter_loop(self, ctx, kind):
        r = self.rng
        i = self.fresh('ct')
        k = r.randint(0, 4)
        local = kind == 'while' and self.chance(0.5)
        loop = {'label': None if local else self.label(), 'kind': kind, 'next': True}
        inner = ctx.child(loop)
        inner.env.vars.append(Var(i, 'int', mut=False, bound=4))
        body = self.body(self.block(inner, r.randint(1, 3)))
        pre = f'{loop["label"]}: ' if loop.get('used') else ''
        if kind == 'while':
            if local:
                return [f'(copy {i} := 0)', f'while {i} < {k} next {i}++ {{'] + body + ['}']
            ctx.env.vars.append(Var(i, 'int', mut=False, bound=5))
            return [f'{i} := 0;', f'{pre}while {i} < {k}', f'next {i}++ {{'] + body + ['}']
        ctx.env.vars.append(Var(i, 'int', mut=False, bound=5))
        return [f'{i} := 0;', f'{pre}do {{'] + body + ['}', f'next {i}++ while {i} < {max(k, 1)};']

    def s_while(self, ctx):
        return self.counter_loop(ctx, 'while')

    def s_do(self, ctx):
        return self.counter_loop(ctx, 'do')

    def s_for(self, ctx):
        env, r = ctx.env, self.rng
        e = self.fresh('e')
        vecs = self.vars_of(env, lambda v: v.ty == 'vec')
        c = r.randrange(4)
        nxt = None
        if c == 0 and vecs:
            v = self.pick(vecs)
            mode = self.pick(['', 'copy ', 'inout '] if v.mut else ['', 'copy '])
            head = f'for {v.name}'
        elif c == 1:
            a = r.randint(-3, 5)
            head = f'for {self.num(a)} {self.pick(["..<", "..="])} {a + r.randint(0, 4)}'
            mode = self.pick(['', 'copy '])
        elif c == 2 and vecs:
            v = self.pick(vecs)
            head = f'for {v.name}.begin() ..< {v.name}.end()'
            mode = 'it'
        else:
            n = self.fresh('j')
            env.vars.append(Var(n, 'int', mut=False, bound=10))
            v = self.pick(vecs) if vecs else None
            head = f'for {v.name}' if v else f'for 0 ..< {r.randint(1, 4)}'
            mode = ''
            nxt = n
        loop = {'label': self.label(), 'kind': 'for', 'next': nxt is not None}
        inner = ctx.child(loop)
        if mode == 'it':
            inner.env.vars.append(Var(e + 'p', 'iter', mut=False))
            body = [f'{e}: i64 = {e}p*;']
            inner.env.vars.append(Var(e, 'int', mut=False))
            param = e + 'p'
        else:
            inner.env.vars.append(Var(e, 'int', mut=(mode != ''), i32=True))
            body = []
            param = f'{mode}{e}'
        body += self.block(inner, r.randint(1, 3))
        if mode == 'inout ':
            body.append(f'{e} = ({e} + {r.randint(1, 9)}) % 1000;')
        pre = f'{loop["label"]}: ' if loop.get('used') else ''
        lines = []
        if nxt:
            lines.append(f'{nxt} := 0;')
            lines += [f'{pre}{head}', f'next {nxt}++', f'do ({param}) {{']
        else:
            lines += [f'{pre}{head} do ({param}) {{']
        return lines + self.body(body) + ['}']

    def s_block(self, ctx):
        return ['{'] + self.body(self.block(ctx.child(), self.rng.randint(1, 3))) + ['}']

    def s_jump(self, ctx):
        loop = ctx.loops[-1]
        targets = [l for l in ctx.loops if l['label']]
        word = self.pick(['break', 'continue'])
        target = None
        if targets and self.chance(0.6):
            target = self.pick(targets)
        t = target or loop
        # break skips a range for's next clause, and a labeled continue runs it,
        # only in cppfrontier (docs/UPSTREAM_DIFFERENCES.md)
        if t['kind'] == 'for' and t['next'] and (word == 'break' or target):
            word = 'continue'
            if t is not loop:
                return []
            target = None
        if target:
            target['used'] = True
        return [f'if {self.bool_expr(ctx.env, 1)} {{', f'    {word}{" " + t["label"] if target else ""};', '}']

    def s_outcall(self, ctx):
        env = ctx.env
        f = self.pick([f for f in self.funcs if f.kind in ('inout', 'out')])
        v = self.pick(self.out_ints(ctx))
        args = [self.wrap(self.int_expr(env, 1))[0] for _ in f.params[1:]]
        self.modified(env, v.name)
        if f.kind == 'out':
            return [f'{f.name}(out {v.name}{"".join(", " + a for a in args)});']
        if self.chance(0.4) and '::' not in f.name:
            return [f'{v.name}.{f.name}({", ".join(args)});']
        return [f'{f.name}({v.name}{"".join(", " + a for a in args)});']

    def s_voidcall(self, ctx):
        f = self.pick([f for f in self.funcs if f.kind == 'void'])
        args = [self.arg(ctx.env, 2, ty, False) for ty in f.params]
        if None in args:
            return []
        return [f'{f.name}({", ".join(args)});']

    def s_mutate(self, ctx):
        env = ctx.env
        o = self.pick([v for v in env.all() if v.ty in self.types and v.mut and self.types[v.ty].mutators])
        o.info['modified'] = True
        return [self.pick(self.types[o.ty].mutators)(self, env, o.name)]

    def s_vecset(self, ctx):
        v = self.pick(self.vars_of(ctx.env, lambda v: v.ty == 'vec' and v.mut))
        return [f'{v.name}[{self.rng.randrange(v.info["size"])}] = {self.wrap(self.int_expr(ctx.env, 2))[0]};']

    def s_strset(self, ctx):
        v = self.pick(self.vars_of(ctx.env, lambda v: v.ty == 'str' and v.mut))
        if self.chance(0.3):
            return [f'{v.name} += "{self.pick(WORDS)}";', f'{v.name} = {v.name}.substr(0, 20);']
        return [f'{v.name} = {strify(self.str_expr(ctx.env, 2))}.substr(0, 20);']

    def s_trace(self, ctx):
        """Copies and moves of a type whose copy and move operations print."""
        env, r = ctx.env, self.rng
        t = self.pick([t for t in self.types.values() if t.tracer])
        objs = self.vars_of(env, lambda v: v.ty == t.name)
        name = self.fresh('t')
        if not objs:
            init = self.wrap(self.int_expr(env, 1))[0]
            env.vars.append(Var(name, t.name))
            return [f'{name}: {t.name} = ({init});']
        o = self.pick(objs)
        c = r.randrange(5)
        fs = t.info['funcs']
        if c == 0:
            env.vars.append(Var(name, t.name))
            return [f'{name} := {o.name};']
        if c == 1 and o.mut and t.info['assign']:
            return [f'{o.name} = {self.pick(objs).name};'] if len(objs) > 1 else []
        if c == 2:
            f = self.pick([f for f in fs if f.kind not in ('tmove', 'tmake')])
            return [f'std::cout << "{t.name}: " << {f.name}({o.name}) << "\\n";']
        if c == 3:
            f = self.pick([f for f in fs if f.kind == 'tmove'])
            return [f'std::cout << "{t.name}: " << {f.name}(move {o.name}) << "\\n";']
        f = self.pick([f for f in fs if f.kind == 'tmake'])
        init = self.wrap(self.int_expr(env, 1))[0]
        env.vars.append(Var(name, t.name))
        return [f'{name} := {f.name}({init});']

    def s_assert(self, ctx):
        e = self.int_expr(ctx.env, 2)
        return [f'assert({e[0]} <= {e[1]});' if self.chance(0.5) else f'assert({e[0]} >= -{e[1]});']

    def s_discard(self, ctx):
        fs = [f for f in self.funcs if f.ret == 'int']
        if not fs:
            return []
        return [f'_ = {bare(self.call(ctx.env, 2, self.pick(fs))[0])};']

    def s_stmtparam(self, ctx):
        name = self.fresh('sp')
        e = self.wrap(self.int_expr(ctx.env, 2))
        inner = ctx.child()
        inner.env.vars.append(Var(name, 'int', mut=False))
        return [f'({name} := {e[0]}) {{'] + self.body(self.block(inner, self.rng.randint(1, 2))) + ['}']

    def s_try(self, ctx):
        r = self.rng
        body = self.block(ctx.child(), r.randint(1, 2))
        c = r.randrange(3)
        if c == 0:
            body.append(f'if {self.bool_expr(ctx.env, 1)} {{')
            body.append(f'    throw({r.randint(0, 9)});')
            body.append('}')
        elif c == 1:
            body.append(f'throw(std::string("{self.pick(WORDS)}"));')
        lines = ['try {'] + self.body(body)
        ex = self.fresh('ex')
        lines += ['}', f'catch ({ex}: int) {{', f'    std::cout << "caught int (({ex}))$\\n";']
        if self.chance(0.6):
            lines += ['}', f'catch ({ex}: std::string) {{', f'    std::cout << "caught string {ex}\\n";']
        return lines + ['}', 'catch (_) {', '    std::cout << "caught\\n";', '}']

    def s_return(self, ctx):
        return [f'if {self.bool_expr(ctx.env, 1)} {{', f'    return {self.wrap(self.int_expr(ctx.env, 2))[0]};', '}']

    # ---- namespace-scope declarations
    def g_globals(self):
        r = self.rng
        for _ in range(r.randint(0, 3)):
            name = self.fresh('g')
            v = self.lit()
            if self.chance(0.5):
                self.decls.append([f'{name}: i64 == {v[0]};'])
                self.globals.vars.append(Var(name, 'int', mut=False))
            else:
                self.decls.append([f'{name}: i64 = {v[0]};'])
                self.globals.vars.append(Var(name, 'int', mut=True, **{'global': True}))
        if self.chance(0.3):
            name = self.fresh('gs')
            self.decls.append([f'{name}: std::string = "{self.pick(WORDS)}";'])
            self.globals.vars.append(Var(name, 'str', mut=False, **{'global': True}))

    def g_function(self, ns=None):
        r = self.rng
        kind = self.weighted([('expr', 4), ('block', 4), ('nr', 2), ('inout', 2), ('out', 2), ('tmpl', 1.5),
                              ('generic', 1.5), ('nttp', 1), ('pack', 1), ('count', 0.5), ('rec', 1),
                              ('void', 1.5), ('str', 1.5), ('constexpr', 1), ('contract', 1.5)])
        name = self.fresh('f')
        qual = f'{ns}::{name}' if ns else name
        nparams = r.randint(1, 3)
        pnames = ['a', 'b', 'c'][:nparams]
        env = Env(self.globals)
        styles = []
        for p in pnames:
            style = self.pick(['', '', 'in ', 'copy '])
            styles.append(style)
            env.vars.append(Var(p, 'int', mut=(style == 'copy ')))
        params = ', '.join(f'{s}{p}: i64' for s, p in zip(styles, pnames))
        ufcs = ns is None
        if kind == 'expr':
            e = self.wrap(self.int_expr(env, 3))
            ret = self.pick(['-> i64 ', '-> i64 ', '-> _ ', ''])
            if not ret and not e[0].startswith('('):
                # without '->' the function returns 'forward _', which cannot return a parameter
                e = (f'({e[0]} + 0)', e[1])
            self.decls.append([f'{name}: ({params}) {ret}= {e[0]};'])
            self.funcs.append(Func(qual, 'plain', ['int'] * nparams, 'int', bound=e[1], ufcs=ufcs))
        elif kind == 'block' or kind == 'contract':
            ctx = Ctx(env, 1, func={'ret': 'int'}, pure=True)
            body = self.block(ctx, r.randint(1, 5))
            e = self.wrap(self.int_expr(env, 2))
            head = f'{name}: ({params}) -> i64'
            if kind == 'contract':
                head += f' pre({pnames[0]} >= -999) pre<cpp2::bounds_safety>({pnames[-1]} <= 999, "in range")'
            self.decls.append([f'{head} = {{'] + self.body(body + [f'return {e[0]};']) + ['}'])
            self.funcs.append(Func(qual, 'plain', ['int'] * nparams, 'int', bound=999, ufcs=ufcs))
        elif kind == 'nr':
            fields = ['r', 's'][:r.randint(1, 2)]
            ctx = Ctx(env, 1, pure=True)
            body = self.block(ctx, r.randint(0, 3))
            assigns = [f'{f} = {self.wrap(self.int_expr(env, 2))[0]};' for f in fields]
            if self.chance(0.4):
                # initialized on both branches; upstream does not see a 'return' as leaving
                f = fields[0]
                assigns = [f'if {self.bool_expr(env, 1)} {{', f'    {f} = {self.wrap(self.int_expr(env, 1))[0]};',
                           '}', 'else {', f'    {f} = {self.wrap(self.int_expr(env, 1))[0]};', '}'] + assigns[1:]
            # no postcondition: upstream reports a named return in one as uninitialized
            outs = ', '.join(f'{f}: i64' for f in fields)
            self.decls.append([f'{name}: ({params}) -> ({outs}) = {{'] + self.body(body + assigns) + ['}'])
            if len(fields) == 1:
                self.funcs.append(Func(qual, 'plain', ['int'] * nparams, 'nr1', bound=999, ufcs=ufcs))
            else:
                self.funcs.append(Func(qual, 'plain', ['int'] * nparams, 'nr', fields=fields, bound=999))
        elif kind in ('inout', 'out'):
            env.vars[0] = Var('a', 'int', mut=True)
            first = f'{kind} a: i64'
            rest = ''.join(f', {s}{p}: i64' for s, p in zip(styles[1:], pnames[1:]))
            if kind == 'out':
                env.vars[0] = Var('a', 'int', mut=True, uninit=True)
                tmp = Env(self.globals)
                tmp.vars = env.vars[1:]
                body = [f'a = {self.wrap(self.int_expr(tmp, 2))[0]};']
                if self.chance(0.4):
                    body = [f'if {self.bool_expr(tmp, 1)} {{', f'    a = {self.wrap(self.int_expr(tmp, 1))[0]};',
                            '}', 'else {', '    ' + body[0], '}']
                env.vars[0] = Var('a', 'int', mut=True)
                body += self.block(Ctx(env, 1, pure=True), r.randint(0, 2))
            else:
                body = self.block(Ctx(env, 1, pure=True), r.randint(1, 3))
                body.append(f'a = {self.wrap((f"(a + {self.int_expr(env, 1)[0]})", 1999))[0]};')
            post = ''
            if kind == 'inout' and self.chance(0.3):
                post = self.pick([' post(a <= 999)', ' post(a$ <= 999)', ' post(a == a)'])
            self.decls.append([f'{name}: ({first}{rest}){post} = {{'] + self.body(body) + ['}'])
            self.funcs.append(Func(qual, kind, ['int'] * nparams, 'void'))
        elif kind in ('tmpl', 'generic'):
            if kind == 'tmpl':
                ps = ', '.join(f'{p}: T' for p in pnames)
                req = ' requires std::integral<T>' if self.chance(0.3) else ''
                head = f'{name}: <T> ({ps}) -> T{req}'
            else:
                ps = ', '.join(self.pick([f'{p}', f'{p}: _', f'{p}: _ is std::integral']) for p in pnames)
                head = f'{name}: ({ps}) -> i64'
            # a deduced parameter may be int, so keep within int range
            e = self.wrap(self.int_expr(env, 2))
            self.decls.append([f'{head} = {e[0]};'])
            self.funcs.append(Func(qual, kind, ['int'] * nparams, 'int', bound=e[1], ufcs=ufcs and kind == 'generic'))
        elif kind == 'nttp':
            env.vars.append(Var('N', 'int', mut=False, bound=9))
            e = self.wrap(self.int_expr(env, 2))
            self.decls.append([f'{name}: <N: i64> ({params}) -> i64 = {e[0]};'])
            self.funcs.append(Func(qual, 'nttp', ['int'] * nparams, 'int', bound=e[1]))
        elif kind == 'pack':
            op = self.pick(['+', '-'])
            form = r.randrange(3)
            if form == 0:
                self.decls.append([f'{name}: <Ts...: type> (args...: Ts) -> i64 = (0 {op} ... {op} args);'])
            elif form == 1:
                self.decls.append([f'{name}: (args...) -> i64 = (args {op} ... {op} 0);'])
            else:
                self.decls.append([f'{name}: <Ts...> (forward args...: Ts) -> i64 = {{', '    s: i64 = (0 + ... + args);',
                                   '    return s;', '}'])
            self.funcs.append(Func(qual, 'pack', [], 'int', bound=lambda n: 999 * n))
        elif kind == 'count':
            self.decls.append([f'{name}: (args...) -> i64 = unchecked_narrow<i64>(sizeof...(args));'])
            self.funcs.append(Func(qual, 'count', [], 'int', bound=4))
        elif kind == 'rec':
            e = self.wrap(self.int_expr(env, 1))
            rest = ''.join(f', {p}' for p in pnames[1:])
            self.decls.append([f'{name}: ({params}) -> i64 = {{',
                               f'    if a <= 0 {{ return {e[0]}; }}',
                               f'    return ({name}((a - 1) % 6{rest}) + {self.lit()[0]}) % 1000;', '}'])
            self.funcs.append(Func(qual, 'plain', ['int'] * nparams, 'int', bound=999, ufcs=ufcs))
        elif kind == 'void':
            body = self.block(Ctx(env, 1), r.randint(1, 3))
            self.decls.append([f'{name}: ({params}) = {{'] + self.body(body) + ['}'])
            self.funcs.append(Func(qual, 'void', ['int'] * nparams, 'void'))
        elif kind == 'str':
            c = r.randrange(3)
            if c == 0:
                # 'a' is mentioned once, so its last use is unambiguous
                del env.vars[0]
                e = self.str_expr(env, 2)
                rest = ''.join(f', {s}{p}: i64' for s, p in zip(styles[1:], pnames[1:]))
                self.decls.append([f'{name}: (move a: std::string{rest}) -> std::string = ({e} + a).substr(0, 20);'])
                self.funcs.append(Func(qual, 'move', ['str'] + ['int'] * (nparams - 1), 'str'))
            elif c == 1:
                env.vars[0] = Var('a', 'str', mut=False)
                e = self.wrap(self.int_expr(env, 2))
                p0 = self.pick(['forward ', '', 'in '])
                rest = ''.join(f', {s}{p}: i64' for s, p in zip(styles[1:], pnames[1:]))
                self.decls.append([f'{name}: ({p0}a: std::string{rest}) -> i64 = (a.ssize() + {e[0]}) % 1000;'])
                # a typed `forward` parameter binds a literal as a char array
                self.funcs.append(Func(qual, 'plain', ['fstr' if p0 == 'forward ' else 'str'] + ['int'] * (nparams - 1),
                                       'int', bound=999))
            else:
                e = self.str_expr(env, 2)
                self.decls.append([f'{name}: ({params}) -> std::string = {{', f'    s: std::string = {e};',
                                   '    return s.substr(0, 20);', '}'])
                self.funcs.append(Func(qual, 'plain', ['int'] * nparams, 'str'))
        elif kind == 'constexpr':
            e = self.wrap(self.const_expr(pnames))
            self.decls.append([f'{name}: ({params}) -> i64 == {e[0]};'])
            self.funcs.append(Func(qual, 'plain', ['int'] * nparams, 'int', bound=e[1], ufcs=ufcs))

    def const_expr(self, pnames):
        a, b = self.pick(pnames), self.pick(pnames)
        return self.pick([(f'({a} * 2 + {b})', 2997), (f'({a} - {b} % 7)', 1005), (f'({a} / 3)', 333)])

    def g_namespace(self):
        ns = self.fresh('ns')
        before = len(self.decls)
        for _ in range(self.rng.randint(1, 2)):
            self.g_function(ns)
        inner = [l for d in self.decls[before:] for l in d]
        del self.decls[before:]
        self.decls.append([f'{ns}: namespace = {{'] + self.body(inner) + ['}'])

    def g_type(self):
        kind = self.weighted([('plain', 4), ('tracer', 2), ('value', 2), ('struct', 2), ('enum', 2), ('flag', 1),
                              ('union', 1.5), ('iface', 1.5), ('base', 1.5), ('box', 1.5), ('ordered', 1)])
        getattr(self, 't_' + kind)()

    def int_ctor(self, args):
        def make(gen, env):
            vals = [gen.wrap(gen.int_expr(env, 1))[0] for _ in range(args)]
            if args == 1 and gen.chance(0.4):
                return vals[0]
            return f'({", ".join(vals)})'
        return make

    def mutator_add(self, method):
        return lambda gen, env, o: f'{o}.{method}({gen.wrap(gen.int_expr(env, 1))[0]});'

    def t_plain(self, tracer=False):
        r = self.rng
        name = self.fresh('T')
        t = TypeInfo(name, 'plain')
        m = self.fresh('m')
        lines = [f'{name}: type = {{', f'    {m}: i64 = {r.randint(0, 9)};']
        sm = None
        if self.chance(0.4):
            sm = self.fresh('m')
            lines.append(f'    {sm}: std::string = "{self.pick(WORDS)}";')
        default = self.chance(0.6)
        if default:
            lines.append('    operator=: (out this) = { }')
            t.ctors.append(lambda gen, env: '()')
        imp = self.pick(['', 'implicit '])
        lines.append(f'    operator=: ({imp}out this, v: i64) = {{')
        lines.append(f'        {m} = v % 1000;')
        if sm and self.chance(0.5):
            lines.append(f'        {sm} = "{self.pick(WORDS)}";')
        lines.append('    }')
        t.ctors.append(self.int_ctor(1))
        t.mutators.append(lambda gen, env, o: f'{o} = {gen.wrap(gen.int_expr(env, 1))[0]};')
        if tracer:
            t.tracer = True
            lines += ['    operator=: (out this, that) = {', f'        {m} = that.{m};',
                      f'        std::cout << "{name} copy (that.{m})$\\n";', '    }']
            # with a move constructor written, copy assignment needs its own operator=
            t.info['assign'] = True
            if self.chance(0.7):
                lines += ['    operator=: (out this, move that) = {', f'        {m} = that.{m};',
                          f'        std::cout << "{name} move (that.{m})$\\n";', '    }']
                t.info['assign'] = False
            if self.chance(0.4):
                t.info['assign'] = True
                lines += ['    operator=: (inout this, that) = {', f'        {m} = that.{m};',
                          f'        std::cout << "{name} assign\\n";', '    }']
        elif self.chance(0.4):
            lines += ['    operator=: (out this, that) = {', f'        {m} = that.{m};'] + \
                     ([f'        {sm} = that.{sm} + "c";'] if sm else []) + ['    }']
        get = self.fresh('get')
        lines.append(f'    {get}: (this) -> i64 = {self.pick([m, "this." + m])};')
        t.int_getters.append((f'{{o}}.{get}()', 999))
        add = self.fresh('add')
        lines.append(f'    {add}: (inout this, d: i64) = {{ {m} = ({m} + d) % 1000; }}')
        t.mutators.append(self.mutator_add(add))
        if self.chance(0.5):
            tw = self.fresh('tw')
            lines.append(f'    {tw}: (this, k: i64) -> i64 = ({get}() * 2 + k) % 1000;')
            t.int_getters.append((f'{{o}}.{tw}({self.rng.randint(0, 9)})', 999))
        if sm:
            sg = self.fresh('sg')
            lines.append(f'    {sg}: (this) -> std::string = {sm};')
            t.str_getters.append(f'{{o}}.{sg}()')
        if not tracer and self.chance(0.5):
            lines.append(f'    operator==: (this, that) -> bool = {m} == that.{m};')
            t.eq = True
            if self.chance(0.5):
                lines.append(f'    operator<=>: (this, that) -> std::strong_ordering = {m} <=> that.{m};')
                t.ordered = True
        lines.append('}')
        self.decls.append(lines)
        self.types[name] = t
        if tracer:
            t.info['funcs'] = fs = []
            for k, spec in (('tcopy', 'copy'), ('tin', 'in'), ('tfwd', 'forward'), ('tmove', 'move')):
                fn = self.fresh('h')
                self.decls.append([f'{fn}: ({spec} x: {name}) -> i64 = x.{get}();'])
                fs.append(Func(fn, k, [name], 'int'))
            if self.chance(0.5):
                fn = self.fresh('h')
                self.decls.append([f'{fn}: ({self.pick(["copy", "forward"])} x: {name}) -> i64 = {fs[0].name}(x);'])
                fs.append(Func(fn, 'tpass', [name], 'int'))
            fn = self.fresh('mk')
            c = r.randrange(3)
            if c == 0:
                self.decls.append([f'{fn}: (v: i64) -> {name} = {{', f'    t: {name} = (v);', '    return t;', '}'])
            elif c == 1:
                self.decls.append([f'{fn}: (v: i64) -> (t: {name}) = {{', '    t = (v);', '}'])
            else:
                self.decls.append([f'{fn}: (copy x: {name}) -> {name} = x;'])
                mk = self.fresh('mk')
                self.decls.append([f'{mk}: (v: i64) -> {name} = {fn}((v));'])
                fn = mk
            fs.append(Func(fn, 'tmake', ['int'], name))
        return t

    def t_tracer(self):
        self.t_plain(tracer=True)

    def t_value(self):
        name = self.fresh('V')
        meta = self.pick(['@value', '@ordered', '@weakly_ordered_value', '@copyable'])
        t = TypeInfo(name, 'value')
        m = self.fresh('m')
        lines = [f'{name}: {meta} type = {{', f'    {m}: i64 = {self.rng.randint(0, 9)};']
        if meta in ('@ordered', '@copyable'):
            lines.append('    operator=: (out this) = { }')
        lines += [f'    operator=: (out this, v: i64) = {{ {m} = v % 1000; }}',
                  f'    get: (this) -> i64 = {m};',
                  f'    set: (inout this, v: i64) = {{ {m} = v % 1000; }}', '}']
        t.ctors += [lambda gen, env: '()', self.int_ctor(1)]
        t.int_getters.append(('{o}.get()', 999))
        t.mutators += [self.mutator_add('set'), lambda gen, env, o: f'{o} = {gen.wrap(gen.int_expr(env, 1))[0]};']
        t.eq = meta not in ('@copyable',)
        t.ordered = t.eq
        t.copyable = meta != '@ordered'
        self.decls.append(lines)
        self.types[name] = t

    def t_ordered(self):
        self.t_value()

    def t_struct(self):
        name = self.fresh('S')
        t = TypeInfo(name, 'struct')
        n = self.rng.randint(1, 3)
        fields = [self.fresh('f') for _ in range(n)]
        lines = [f'{name}: @struct type = {{'] + [f'    {f}: i64 = {self.rng.randint(0, 9)};' for f in fields]
        if self.chance(0.4):
            lines.append(f'    sum: (this) -> i64 = ({" + ".join(fields)}) % 1000;')
            t.int_getters.append(('{o}.sum()', 999))
        lines.append('}')
        t.ctors.append(self.int_ctor(n))
        for f in fields:
            t.int_getters.append((f'{{o}}.{f}', 999))
            t.mutators.append(lambda gen, env, o, f=f: f'{o}.{f} = {gen.wrap(gen.int_expr(env, 1))[0]};')
        self.decls.append(lines)
        self.types[name] = t

    def t_enum(self, flag=False):
        name = self.fresh('E')
        t = TypeInfo(name, 'enum' if not flag else 'flag')
        values = [self.fresh('k') for _ in range(self.rng.randint(1, 4))]
        t.info['values'] = values
        lines = [f'{name}: {"@flag_enum" if flag else "@enum"} type = {{']
        for i, v in enumerate(values):
            if not flag and i > 0 and self.chance(0.3):
                lines.append(f'    {v} := {10 + i * 3};')
            else:
                lines.append(f'    {v};')
        lines.append('}')
        self.decls.append(lines)
        self.types[name] = t
        if flag:
            t.ctors.append(lambda gen, env: ' | '.join(f'{name}::{gen.pick(values)}' for _ in range(gen.rng.randint(1, 2))))
            t.mutators.append(lambda gen, env, o: f'{o}.set({name}::{gen.pick(values)});')
            t.mutators.append(lambda gen, env, o: f'{o}.clear({name}::{gen.pick(values)});')
            t.int_getters.append((f'({{o}}.has({name}::{values[0]}) as i64)', 1))
        else:
            t.ctors.append(lambda gen, env: f'{name}::{gen.pick(values)}')
            t.mutators.append(lambda gen, env, o: f'{o} = {name}::{gen.pick(values)};')
            t.int_getters.append(('({o}.get_raw_value() as i64)', 100))
            t.int_getters.append((f'(inspect {{o}} -> i64 {{{{ is ({name}::{values[0]}) = 1; is _ = 2; }}}})', 2))
            t.eq = True
        t.str_getters.append('{o}.to_string()')

    def t_flag(self):
        self.t_enum(flag=True)

    def t_union(self):
        name = self.fresh('U')
        t = TypeInfo(name, 'union')
        alt = self.fresh('n')
        salt = self.fresh('w')
        t.info['alt'] = alt
        self.decls.append([f'{name}: @union type = {{', f'    {alt}: i64;', f'    {salt}: std::string;', '}'])
        t.ctors.append(lambda gen, env: '()')
        t.int_getters.append((f'{{o}}.{alt}()', 999))
        t.int_getters.append((f'({{o}}.is_{salt}() as i64)', 1))
        t.mutators.append(lambda gen, env, o: f'{o}.set_{alt}({gen.wrap(gen.int_expr(env, 1))[0]});')
        t.copyable = True
        self.types[name] = t

    def t_iface(self):
        r = self.rng
        name = self.fresh('I')
        meta = self.pick(['@interface', '@polymorphic_base'])
        fn = self.fresh('vf')
        if meta == '@interface':
            self.decls.append([f'{name}: @interface type = {{', f'    {fn}: (this, k: i64) -> i64;', '}'])
        else:
            self.decls.append([f'{name}: @polymorphic_base type = {{',
                               f'    {fn}: (virtual this, k: i64) -> i64 = k % 10;', '}'])
        use = self.fresh('use')
        self.decls.append([f'{use}: (x: {name}, k: i64) -> i64 = x.{fn}(k);'])
        for _ in range(r.randint(1, 2)):
            impl = self.fresh('C')
            t = TypeInfo(impl, 'impl')
            m = self.fresh('m')
            e = self.pick([f'({m} + k) % 1000', f'({m} * 3 - k) % 1000', 'k'])
            spec = self.pick(['override', 'final']) if meta == '@polymorphic_base' else self.pick(['override', 'virtual'])
            if spec == 'virtual' and meta == '@interface':
                spec = 'override'
            lines = [f'{impl}: type = {{', f'    this: {name} = ();', f'    {m}: i64 = {r.randint(0, 9)};',
                     f'    operator=: (out this, v: i64) = {{ {m} = v % 1000; }}',
                     f'    {fn}: ({spec} this, k: i64) -> i64 = {e};', '}']
            self.decls.append(lines)
            t.ctors.append(self.int_ctor(1))
            t.int_getters.append((f'{use}({{o}}, 7)', 999))
            t.int_getters.append((f'{{o}}.{fn}(2)', 999))
            t.copyable = False
            self.types[impl] = t

    def t_base(self):
        r = self.rng
        base = self.fresh('B')
        bm = self.fresh('m')
        vf = self.fresh('vf')
        bget = self.fresh('bget')
        self.decls.append([f'{base}: type = {{', f'    {bm}: i64 = {r.randint(0, 9)};',
                           f'    operator=: (out this, v: i64) = {{ {bm} = v % 1000; }}',
                           '    operator=: (virtual move this) = { }',
                           f'    {bget}: (this) -> i64 = {bm};',
                           f'    {vf}: (virtual this) -> i64 = {bm};', '}'])
        tb = TypeInfo(base, 'base')
        tb.ctors.append(self.int_ctor(1))
        call = self.fresh('callv')
        self.decls.append([f'{call}: (b: {base}) -> i64 = b.{vf}();'])
        tb.int_getters += [(f'{call}({{o}})', 999), (f'{{o}}.{bget}()', 999)]
        tb.copyable = False
        self.types[base] = tb
        der = self.fresh('D')
        dm = self.fresh('m')
        k = r.randint(0, 9)
        init = self.chance(0.5)
        self.decls.append([f'{der}: type = {{', f'    this: {base} = ({k});', f'    {dm}: i64 = 0;',
                           f'    operator=: (out this, v: i64) = {{'] +
                          ([f'        {base} = (v + {k});'] if init else []) +
                          [f'        {dm} = v % 1000;', '    }',
                           f'    {vf}: (override this) -> i64 = ({dm} + {self.pick([bget + "()", "this." + bget + "()"])}) % 1000;',
                           '}'])
        td = TypeInfo(der, 'derived')
        td.ctors.append(self.int_ctor(1))
        td.int_getters += [(f'{call}({{o}})', 999), (f'{{o}}.{vf}()', 999), (f'{{o}}.{bget}()', 999)]
        td.copyable = False
        self.types[der] = td

    def t_box(self):
        name = self.fresh('X')
        self.decls.append([f'{name}: <T> type = {{', '    val: T = ();',
                           '    operator=: (out this) = { }',
                           '    operator=: (out this, v: T) = { val = v; }',
                           '    get: (this) -> T = val;',
                           '    put: (inout this, v: T) = { val = v; }', '}'])
        t = TypeInfo(name + '_i', 'box', spelling=f'{name}<i64>')
        t.ctors += [lambda gen, env: '()', lambda gen, env: f'({gen.wrap(gen.int_expr(env, 1))[0]})']
        t.int_getters.append(('{o}.get()', 999))
        t.mutators.append(lambda gen, env, o: f'{o}.put({gen.wrap(gen.int_expr(env, 1))[0]});')
        self.types[t.name] = t
        ts = TypeInfo(name + '_s', 'box', spelling=f'{name}<std::string>')
        ts.ctors.append(lambda gen, env: f'(std::string("{gen.pick(WORDS)}"))')
        ts.str_getters.append('{o}.get()')
        ts.mutators.append(lambda gen, env, o: f'{o}.put({gen.str_expr(env, 1)});')
        self.types[ts.name] = ts

    # ---- whole program
    def program(self):
        r = self.rng
        self.g_globals()
        for _ in range(r.randint(0, 3)):
            self.g_type()
        for _ in range(r.randint(1, 6)):
            if self.chance(0.15):
                self.g_namespace()
            else:
                self.g_function()
        ctx = Ctx(Env(self.globals), 1)
        main = self.block(ctx, r.randint(8, 25))
        lines = []
        for d in self.decls:
            lines += d
        lines.append('main: () = {')
        lines += self.body(main)
        lines.append('}')
        return '\n'.join(lines) + '\n'


def generate(seed):
    return Gen(seed).program()


# ---- running
class Runner:
    def __init__(self, args):
        self.args = args
        self.pch = {}

    def sides(self):
        a = self.args
        return {'ours': (a.bin, a.runtime), 'upstream': (a.upstream_bin, os.path.join(a.upstream, 'include'))}

    def prepare(self):
        """Build a precompiled cpp2util.h for each runtime, so each compile takes a fraction of the time."""
        for side, (_, include) in self.sides().items():
            d = os.path.join(self.args.out, 'pch-' + side)
            gch = os.path.join(d, 'cpp2util.h.gch')
            header = os.path.join(include, 'cpp2util.h')
            os.makedirs(d, exist_ok=True)
            if not os.path.exists(gch) or os.path.getmtime(gch) < os.path.getmtime(header):
                subprocess.run(['timeout', '300', self.args.cxx, '-std=c++23', '-x', 'c++-header', header,
                                '-o', gch], check=True)
            self.pch[side] = d

    def run_side(self, side, source, work):
        binary, include = self.sides()[side]
        cpp, exe = os.path.join(work, side + '.cpp'), os.path.join(work, side)
        try:
            t = subprocess.run(['timeout', '60', binary, source, '-o', cpp], capture_output=True, cwd=work)
        except OSError as e:
            return ('translate', str(e))
        if t.returncode != 0 or not os.path.exists(cpp):
            msg = t.stdout.decode(errors='replace') + t.stderr.decode(errors='replace')
            kind = 'translate-crash' if t.returncode >= 124 or t.returncode < 0 else 'translate'
            return (kind, first_error(msg))
        c = subprocess.run(['timeout', '120', self.args.cxx, '-std=c++23', '-I', self.pch[side], '-I', include,
                            cpp, '-o', exe], capture_output=True, cwd=work)
        if c.returncode != 0:
            return ('compile', first_error(c.stderr.decode(errors='replace')))
        try:
            p = subprocess.run(['timeout', '10', exe], capture_output=True, cwd=work)
        except OSError as e:
            return ('run', str(e))
        return ('run', p.returncode, p.stdout.decode(errors='replace'))

    def run(self, text, work):
        os.makedirs(work, exist_ok=True)
        with open(os.path.join(work, 'fuzz.cpp2'), 'w') as f:
            f.write(text)
        return {side: self.run_side(side, 'fuzz.cpp2', work) for side in ('ours', 'upstream')}


def first_error(msg):
    """The first error, with the program line that leads to it when it is in a header."""
    lines = msg.splitlines()
    for line in lines:
        if 'error' in line:
            if 'fuzz.cpp2' not in line:
                where = next((l for l in lines if re.match(r'fuzz\.cpp2:\d+:\d+:', l)), '')
                line = where.split(' ', 1)[0] + ' ' + line.strip()
            return line.strip()[:300]
    return msg.strip()[-300:]


def normalize(msg):
    msg = re.sub(r'\S*fuzz\.cpp2[:(][\d:,)]*', '', msg)
    return re.sub(r'\d+', 'N', msg)


def same(res):
    """Both runs agree, or both programs fail (a generator problem, counted as invalid)."""
    return res['ours'] == res['upstream'] or (res['ours'][0] != 'run' and res['upstream'][0] != 'run')


def signature(res):
    """What a reduction must keep: each side's stage, its error, and whether the runs differ."""
    sig = []
    for side in ('ours', 'upstream'):
        r = res[side]
        sig.append(r[0] if r[0] == 'run' else (r[0], normalize(r[1])))
    if res['ours'][0] == res['upstream'][0] == 'run':
        sig.append(res['ours'][1] != res['upstream'][1])
        sig.append(res['ours'][2] != res['upstream'][2])
    return tuple(sig)


# Differences that are deliberate or upstream's, as (name, predicate on the result).
def upstream_fails(stage, pattern):
    def pred(text, res, work):
        up, ours = res['upstream'], res['ours']
        return up[0] == stage and ours[0] == 'run' and re.search(pattern, up[1]) is not None
    return pred


def upstream_moves_more(text, res, work):
    """Both run with different output, and upstream's Cpp1 moves some name more often."""
    if res['ours'][0] != 'run' or res['upstream'][0] != 'run':
        return False
    try:
        ours, up = (open(os.path.join(work, side + '.cpp')).read() for side in ('ours', 'upstream'))
    except OSError:
        return False
    moves = lambda cpp: collections.Counter(re.findall(r'cpp2::move\((\w+)\)', cpp))
    mine = moves(ours)
    return any(n > 1 and n > mine[name] for name, n in moves(up).items())


# Upstream failures on valid programs that cppfrontier runs: (name, predicate).
# Each predicate takes the program, both results and the work directory.
KNOWN = [
    # cppfront's sema.h `find_definite_last_uses` asserts when a local's last use is
    # an `if` condition in a block or `try` after a `do` loop:
    # `x := 1; do { } while c; { if x == 2 { } }`
    ('upstream-last-use-assert', upstream_fails('translate-crash', r'find_definite_last_uses')),
    # a capture `a$` of an `out` parameter inside a function expression with its
    # own parameter `a` is not read through `.value()`: `:(a: i64) -> i64 = a + a$`
    ('upstream-out-capture', upstream_fails('compile', r'cpp2::impl::out<')),
    # after a statement-parameter loop, a use in a nested block is not seen, so an
    # earlier use moves: `t := s; (copy i := 0) while i < 2 next i++ { } { f(s); }`
    ('upstream-moves-twice', upstream_moves_more),
]


def known(text, res, work):
    for name, pred in KNOWN:
        if pred(text, res, work):
            return name
    return None


def spans(lines):
    """Each deletable unit: a line, or a line that opens a brace through the line that closes it."""
    out = []
    for i, line in enumerate(lines):
        depth, j = 0, i
        while j < len(lines):
            s = re.sub(r'"(\\.|[^"\\])*"', '""', lines[j])
            depth += s.count('{') - s.count('}')
            if depth <= 0:
                break
            j += 1
        if depth > 0 or j == len(lines):
            j = i
        out.append((i, j + 1))
    return out


def reduce(runner, text, res, work):
    target = signature(res)
    cache = {}
    tries = 0

    def interesting(lines):
        nonlocal tries
        candidate = '\n'.join(lines) + '\n'
        key = hashlib.sha1(candidate.encode()).hexdigest()
        if key not in cache:
            tries += 1
            r = runner.run(candidate, work)
            cache[key] = not same(r) and signature(r) == target
        return cache[key]

    lines = text.splitlines()
    changed = True
    while changed and tries < runner.args.reduce_limit:
        changed = False
        units = spans(lines)
        # coarse to fine: delete groups of units, then single units
        size = max(1, len(units) // 4)
        while size >= 1 and tries < runner.args.reduce_limit:
            i = len(units) - 1
            while i >= 0 and tries < runner.args.reduce_limit:
                group = units[max(0, i - size + 1):i + 1]
                drop = set()
                for a, b in group:
                    drop.update(range(a, b))
                candidate = [l for k, l in enumerate(lines) if k not in drop]
                if candidate and interesting(candidate):
                    lines, changed = candidate, True
                    units = spans(lines)
                    i = min(i - size, len(units) - 1)
                else:
                    i -= size
            size //= 2
        # unwrap blocks: keep a block's body in place of the block
        for a, b in spans(lines):
            if b - a > 2 and tries < runner.args.reduce_limit:
                candidate = lines[:a] + [l[4:] if l.startswith('    ') else l for l in lines[a + 1:b - 1]] + lines[b:]
                if interesting(candidate):
                    lines, changed = candidate, True
                    break
    return '\n'.join(lines) + '\n'


def describe(res):
    out = []
    for side in ('ours', 'upstream'):
        r = res[side]
        if r[0] == 'run':
            out.append(f'{side}: exit {r[1]}, output:\n' + ''.join('    ' + l + '\n' for l in r[2].splitlines()[-40:]))
        else:
            out.append(f'{side}: {r[0]} failure: {r[1]}\n')
    return ''.join(out)


def check(runner, seed, text=None):
    args = runner.args
    text = text if text is not None else generate(seed)
    work = os.path.join(args.out, 'work', str(seed))
    res = runner.run(text, work)
    status = 'same'
    if same(res):
        if res['ours'][0] != 'run':
            status = 'invalid'      # both reject: a generator problem
    else:
        k = known(text, res, work)
        status = 'known:' + k if k else 'diff'
    if status in ('diff', 'invalid') or args.keep:
        stem = os.path.join(args.out, f'seed-{seed}')
        with open(stem + '.cpp2', 'w') as f:
            f.write(text)
        report = describe(res)
        if status == 'diff' and not args.no_reduce:
            small = reduce(runner, text, res, work)
            with open(stem + '.min.cpp2', 'w') as f:
                f.write(small)
            report += '\nreduced (' + stem + '.min.cpp2):\n' + small + '\n' + describe(runner.run(small, work))
        with open(stem + '.txt', 'w') as f:
            f.write(report)
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    return seed, status


def ensure_upstream(args):
    if os.path.exists(args.upstream_bin):
        return
    os.makedirs(os.path.dirname(args.upstream_bin), exist_ok=True)
    print('building upstream cppfront into', args.upstream_bin, flush=True)
    subprocess.run(['timeout', '600', args.cxx, '-std=c++20', '-O2',
                    os.path.join(args.upstream, 'source', 'cppfront.cpp'), '-o', args.upstream_bin], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('--bin', default=os.path.join(repo, 'build', 'cppfrontier'))
    p.add_argument('--upstream', default=os.path.join(repo, '..', 'cppfront'))
    p.add_argument('--upstream-bin', default=os.environ.get('UPSTREAM_BIN', os.path.join(repo, 'build', 'upstream', 'cppfront')),
                   help='upstream cppfront executable; built from --upstream if missing')
    p.add_argument('--runtime', default=os.path.join(repo, 'runtime'))
    p.add_argument('--cxx', default='g++')
    p.add_argument('--out', default=os.path.join(repo, 'build', 'corpus', 'diff-fuzz'))
    p.add_argument('--seeds', default='1:2001', help='half-open seed range A:B')
    p.add_argument('--seed', type=int, help='run one seed (with --print, only print its program)')
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--print', action='store_true', help='print the program for --seed and exit')
    p.add_argument('--no-reduce', action='store_true')
    p.add_argument('--reduce-limit', type=int, default=400, help='most attempts per reduction')
    p.add_argument('--keep', action='store_true', help='keep every program and work directory')
    p.add_argument('--file', help='check (and reduce) this program instead of generated ones')
    args = p.parse_args()
    if args.seed is not None and args.print:
        sys.stdout.write(generate(args.seed))
        return 0
    seeds = [0] if args.file else [args.seed] if args.seed is not None else range(*map(int, args.seeds.split(':')))
    text = open(args.file).read() if args.file else None
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    for f in os.listdir(args.out):
        if f.startswith('seed-'):
            os.remove(os.path.join(args.out, f))
    ensure_upstream(args)
    runner = Runner(args)
    runner.prepare()
    counts = {}
    findings = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for seed, status in pool.map(lambda s: check(runner, s, text), seeds):
            counts[status] = counts.get(status, 0) + 1
            if status in ('diff', 'invalid'):
                findings.append((seed, status))
                print(f'seed {seed}: {status} ({os.path.join(args.out, f"seed-{seed}.txt")})', flush=True)
    print(len(seeds), 'programs;', ', '.join(f'{k}={v}' for k, v in sorted(counts.items())))
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
