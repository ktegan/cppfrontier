#!/usr/bin/env python3
"""Translate large generated inputs, each of which once took time quadratic or
worse in its size, and report any that fail or exceed the time limit.

usage: tools/scaling.py [--bin PATH] [--out DIR] [--timeout SECONDS]
"""
import argparse, os, subprocess, time

repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
parser = argparse.ArgumentParser()
parser.add_argument('--bin', default=os.path.join(repo, 'build', 'cppfrontier'))
parser.add_argument('--out', default=os.path.join(repo, 'build', 'scaling'))
parser.add_argument('--timeout', type=float, default=120)
args = parser.parse_args()
os.makedirs(args.out, exist_ok=True)

def body(lines):
    return 'main: () = {\n' + ''.join('    ' + line + '\n' for line in lines) + '}\n'

inputs = {
    # Scope lookup per identifier.
    'functions': ''.join(f'f{i}: (a: int) -> int = {{ b: int = a + {i}; return b * 2; }}\n'
                         for i in range(4000)) + 'main: () -> int = { return f0(1); }\n',
    # Last-use search per local.
    'initialized': body([f'x{i}: int = {i};' for i in range(3000)] +
                        [f'std::cout << x{i};' for i in range(3000)]),
    # Initialization analysis and wrapped-local rendering per deferred local.
    'deferred': body([f'x{i}: int;' for i in range(1500)] + [f'x{i} = {i};' for i in range(1500)] +
                     [f'std::cout << x{i};' for i in range(1500)]),
    # Function-expression extraction per interpolation.
    'interpolations': body(['x := 1;'] + ['std::cout << "(x:>5)$";'] * 8000),
    # Template argument scan per `<`, in the expression parser and validation.
    'comparisons': 'f: (xs...: bool) = { }\n' + body(['a := 1;', 'b := 2;',
                                                      'f(' + ', '.join(['a<b'] * 8000) + ');']),
    # Type parsing per `*`.
    'pointers': 'f: (p: ' + '*' * 8000 + 'int) = { }\nmain: () = { }\n',
    # Moving a later type ahead of each type that names it.
    'type-chain': ''.join(f'T{i}: type = {{ public p: *T{i + 1} = unchecked_cast<*T{i + 1}>(nullptr); '
                          f'public q: *T{max(i - 1, 0)} = unchecked_cast<*T{max(i - 1, 0)}>(nullptr); }}\n'
                          for i in range(2000)) + 'T2000: type = { }\nmain: () = { }\n',
}
failures = []
for name, text in inputs.items():
    source, output = os.path.join(args.out, name + '.cpp2'), os.path.join(args.out, name + '.cpp')
    open(source, 'w').write(text)
    start = time.monotonic()
    try:
        result = subprocess.run([args.bin, source, '-o', output], capture_output=True,
                                timeout=args.timeout)
        status = 'exit %d' % result.returncode if result.returncode != 0 else None
    except subprocess.TimeoutExpired:
        status = 'timeout'
    elapsed = time.monotonic() - start
    print('%-16s %6.2fs %s' % (name, elapsed, status or 'ok'))
    if status: failures.append(name)
raise SystemExit(1 if failures else 0)
