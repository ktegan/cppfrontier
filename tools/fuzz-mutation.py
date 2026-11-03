#!/usr/bin/env python3
"""Translate randomly mutated copies of each upstream regression test and
report inputs that crash or hang the translator.

Each mutation deletes, duplicates, or swaps a short span of the source, or
inserts a token. The run is deterministic for a given --seed; failing inputs
are saved to the output directory.

usage: tools/fuzz-mutation.py [--bin PATH] [--upstream DIR] [--out DIR]
                              [--seed N] [--per-file N] [--jobs N]
"""
import argparse, concurrent.futures, glob, os, random, subprocess

repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
parser = argparse.ArgumentParser()
parser.add_argument('--bin', default=os.path.join(repo, 'build', 'cppfrontier'))
parser.add_argument('--upstream', default=os.path.join(repo, '..', 'cppfront'))
parser.add_argument('--out', default=os.path.join(repo, 'build', 'corpus', 'fuzz-mutation'))
parser.add_argument('--seed', type=int, default=1)
parser.add_argument('--per-file', type=int, default=20)
parser.add_argument('--jobs', type=int, default=os.cpu_count())
args = parser.parse_args()
os.makedirs(args.out, exist_ok=True)

TOKENS = [b'(', b')', b'{', b'}', b'<', b'>', b'[', b']', b':', b';', b',', b'=', b'==', b':=',
          b'$', b'@', b'*', b'&', b'.', b'..', b'->', b'"', b"'", b'R"(', b'/*', b'//', b'\n',
          b'::', b'_', b'this', b'that', b'type', b'namespace', b'is', b'as', b'inspect',
          b'in', b'out', b'inout', b'move', b'forward', b'next', b'pre', b'post', b'requires']


def mutate(data, rng):
    data = bytearray(data)
    for _ in range(rng.randint(1, 3)):
        at = rng.randrange(len(data) + 1)
        length = rng.randint(1, 16)
        choice = rng.randrange(4)
        if choice == 0:
            del data[at:at + length]
        elif choice == 1:
            data[at:at] = data[at:at + length]
        elif choice == 2:
            other = rng.randrange(len(data) + 1)
            a, b = sorted((at, other))
            first, second = data[a:a + length], data[b:b + length]
            if a + length <= b:
                data[b:b + length] = first
                data[a:a + length] = second
        else:
            data[at:at] = rng.choice(TOKENS)
    return bytes(data)


def check(job):
    path, index = job
    rng = random.Random(f'{args.seed}:{os.path.basename(path)}:{index}')
    data = mutate(open(path, 'rb').read(), rng)
    stem = f'{os.path.basename(path)[:-5]}-{args.seed}-{index}'
    source, output = os.path.join(args.out, stem + '.cpp2'), os.path.join(args.out, stem + '.cpp')
    open(source, 'wb').write(data)
    try:
        result = subprocess.run([args.bin, source, '-o', output], capture_output=True, timeout=20)
        failed = (result.returncode < 0 or result.returncode >= 128 or
                  b'Sanitizer' in result.stderr or b'runtime error' in result.stderr or
                  b'Assertion' in result.stderr)
        detail = result.stderr.decode(errors='replace').replace('\n', ' | ')[-250:]
    except subprocess.TimeoutExpired:
        failed, detail = True, 'hang (over 20 s)'
    if os.path.exists(output):
        os.remove(output)
    if not failed:
        os.remove(source)
        return None
    return source, detail


files = sorted(glob.glob(os.path.join(args.upstream, 'regression-tests', '*.cpp2')))
jobs = [(path, index) for path in files for index in range(args.per_file)]
with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
    failures = [failure for failure in pool.map(check, jobs) if failure]
print(len(jobs), 'inputs;', len(failures), 'failing')
for source, detail in failures[:20]:
    print(source, detail)
raise SystemExit(1 if failures else 0)
