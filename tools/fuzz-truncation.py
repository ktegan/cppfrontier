#!/usr/bin/env python3
"""Translate every prefix (in 5% steps) of each upstream regression test and
report inputs that crash the translator.

usage: tools/fuzz-truncation.py [--bin PATH] [--upstream DIR] [--out DIR]
"""
import argparse, glob, os, subprocess

repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
parser = argparse.ArgumentParser()
parser.add_argument('--bin', default=os.path.join(repo, 'build', 'cppfrontier'))
parser.add_argument('--upstream', default=os.path.join(repo, '..', 'cppfront'))
parser.add_argument('--out', default=os.path.join(repo, 'build', 'corpus', 'fuzz'))
args = parser.parse_args()
os.makedirs(args.out, exist_ok=True)
files = sorted(glob.glob(os.path.join(args.upstream, 'regression-tests', '*.cpp2')))
source, output = os.path.join(args.out, 't.cpp2'), os.path.join(args.out, 't.cpp')
crashes = []
for path in files:
    data = open(path, 'rb').read()
    for step in range(1, 21):
        size = len(data) * step // 20
        open(source, 'wb').write(data[:size])
        try:
            result = subprocess.run([args.bin, source, '-o', output], capture_output=True,
                                    timeout=30)
        except subprocess.TimeoutExpired:
            crashes.append((path, size, 'timeout', ''))
            break
        if (result.returncode < 0 or result.returncode >= 128 or
                b'Sanitizer' in result.stderr or b'runtime error' in result.stderr or
                b'Assertion' in result.stderr):
            crashes.append((path, size, result.returncode, result.stderr.decode(errors='replace')))
            break
print(len(files), 'files;', len(crashes), 'crashing or hanging')
for path, size, code, stderr in crashes[:15]:
    print(os.path.basename(path), size, code, stderr.replace('\n', ' | ')[-250:])
raise SystemExit(1 if crashes else 0)
