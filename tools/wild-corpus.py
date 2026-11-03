#!/usr/bin/env python3
"""Translate Cpp2 code written outside this project with cppfrontier and with
upstream cppfront, compile the Cpp1 that both produce, and classify each file.

tools/wild-corpus.json lists the repositories: a URL, a pinned commit, and
`paths` (Cpp2 sources) or `markdown` (documents whose ``` cpp blocks are
translated one by one) as glob patterns, with optional `exclude` patterns.
Each repository is fetched at its commit into a fresh scratch directory
(a blob-filtered `git fetch` and a checkout of the selected sources and of
Cpp1 headers beside them); nothing in it is built or run. When both
translators accept a file, its Cpp1 is compiled with `g++ -std=c++20
-fsyntax-only` (and `-DCPP2_INCLUDE_STD=Yes` when upstream reports the file
as pure Cpp2), against runtime/ for ours and upstream's include/ for
upstream's. Each file is classified as:

  both-accept            both translate, and the Cpp1 compiles (or fails) in both
  both-reject            both translators reject it
  only-ours-rejects      upstream translates it, cppfrontier does not
  only-upstream-rejects  cppfrontier translates it, upstream does not
  ours-cpp1-fails        both translate; only upstream's Cpp1 compiles
  upstream-cpp1-fails    both translate; only our Cpp1 compiles
  ours-crash             cppfrontier crashed or timed out

The run fails on an `ours-crash`, and on an `only-ours-rejects` or
`ours-cpp1-fails` file that the KNOWN list below does not explain. It writes
summary.txt, details.txt (one line per file that is not both-accept) and
details.json (every file) to --out. --check-manifest only validates the
manifest and needs no network.

usage: tools/wild-corpus.py [--check-manifest] [--repo SUBSTRING] [--reuse]
                            [--bin PATH] [--upstream-bin PATH] [--upstream DIR]
                            [--out DIR] [--jobs N]
"""
import argparse, concurrent.futures, fnmatch, json, os, pathlib, re, shutil, subprocess

repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
parser = argparse.ArgumentParser()
parser.add_argument('--manifest', default=os.path.join(repo, 'tools', 'wild-corpus.json'))
parser.add_argument('--check-manifest', action='store_true')
parser.add_argument('--repo', help='only repositories whose URL contains this text')
parser.add_argument('--reuse', action='store_true',
                    help='reuse checkouts from an earlier run instead of fetching again')
parser.add_argument('--bin', default=os.path.join(repo, 'build', 'cppfrontier'))
parser.add_argument('--upstream-bin', help="upstream's cppfront; by default built from --upstream")
parser.add_argument('--upstream', default=os.path.join(repo, '..', 'cppfront'))
parser.add_argument('--cxx', default='g++')
parser.add_argument('--out', default=os.path.join(repo, 'build', 'corpus', 'wild'))
parser.add_argument('--jobs', type=int, default=3)
args = parser.parse_args()

# Files whose `only-ours-rejects` or `ours-cpp1-fails` result is explained, as
# (glob over `owner/repo:path`, reason). Markdown blocks are `path#N`.
KNOWN = [
    # Deliberate differences (docs/UPSTREAM_DIFFERENCES.md).
    ('hsutter/cppfront:docs/cpp2/safety.md#1',
     'deliberate: a mixed-sign comparison is rejected during translation; upstream '
     'rejects it when the Cpp1 is compiled'),
    ('isidorostsa/RayTrayCpp2:src/inc/vec3.h2',
     'deliberate: an assignment is never a value (`return this *= 1/t;`)'),
    ('williamgrantuk/make-a-lisp-cpp2:src/step0_repl.cpp2',
     'deliberate: an assignment is never a value (`while (line = f()).has_value()`)'),
    *[(f'Lathentar/aoc_2024:day{day}/aoc_2024.cpp2',
       'deliberate: an assignment is never a value (`next (pt.y++, pt.x = 0)`)')
      for day in (12, 20, 24)],
    ('neumannt/cppfront-exp:tests/parameter-out3.cpp2',
     'deliberate: `return;` before an `out` parameter is set is rejected; upstream '
     'ignores the return (the file expects the error)'),
    ('ProgrammingRainbow/Beginners-Guide-to-SDL2-in-Cpp2:*main.cpp2',
     'deliberate: `[]` on a `*T` data member is pointer arithmetic; upstream checks '
     'only locals and parameters'),
    ('ishani/cppfront-msbuild:src/geom/prims.h2',
     'deliberate: a `final this` function is virtual, so `@basic_value` rejects it '
     '(metafunctions.md: not polymorphic); upstream checks only `virtual this`'),
    ('anton-nikan/cpp2-explore:src/example-5.cpp2',
     'deliberate: the generated default constructor uses the base initializer '
     '`this: shape = ("circle")` (#422), and the base has no such constructor; '
     'upstream drops the initializer'),
]

HEADERS = ('.h', '.hh', '.hpp', '.hxx', '.inl', '.ipp', '.tpp', '.h2')
CATEGORIES = ['both-accept', 'both-reject', 'only-ours-rejects', 'only-upstream-rejects',
              'ours-cpp1-fails', 'upstream-cpp1-fails', 'ours-crash']
FAILING = {'only-ours-rejects', 'ours-cpp1-fails'}


def repo_name(url):
    return '/'.join(url.rstrip('/').split('/')[-2:])


def check_manifest(path):
    problems = []
    try:
        data = json.load(open(path))
    except (OSError, ValueError) as error:
        return [f'{path}: {error}']
    entries = data.get('repositories') if isinstance(data, dict) else None
    if not isinstance(entries, list) or not entries:
        return [f'{path}: expected an object with a non-empty "repositories" list']
    names = set()
    for i, entry in enumerate(entries):
        where = f'repositories[{i}]'
        if not isinstance(entry, dict):
            problems.append(f'{where}: not an object'); continue
        extra = set(entry) - {'url', 'commit', 'paths', 'markdown', 'exclude', 'note'}
        if extra:
            problems.append(f'{where}: unknown keys {sorted(extra)}')
        url = entry.get('url')
        if not isinstance(url, str) or not re.fullmatch(r'https://[\w.-]+(/[\w.-]+)+', url):
            problems.append(f'{where}: "url" must be an https repository URL'); continue
        name = repo_name(url)
        if name in names:
            problems.append(f'{where}: {name} is listed twice')
        names.add(name)
        if not isinstance(entry.get('commit'), str) or not re.fullmatch(r'[0-9a-f]{40}', entry['commit']):
            problems.append(f'{where} ({name}): "commit" must be a full 40-digit SHA')
        if not entry.get('paths') and not entry.get('markdown'):
            problems.append(f'{where} ({name}): needs "paths" or "markdown"')
        for key in ('paths', 'markdown', 'exclude'):
            value = entry.get(key, [])
            if not isinstance(value, list) or not all(isinstance(p, str) and p for p in value):
                problems.append(f'{where} ({name}): "{key}" must be a list of glob strings')
    for pattern, reason in KNOWN:
        if pattern.split(':')[0] not in names:
            problems.append(f'KNOWN entry {pattern!r} names no manifest repository')
        if not reason:
            problems.append(f'KNOWN entry {pattern!r} has no reason')
    return problems


problems = check_manifest(args.manifest)
if problems or args.check_manifest:
    print('\n'.join(problems) if problems else 'manifest ok')
    raise SystemExit(1 if problems else 0)

entries = json.load(open(args.manifest))['repositories']
if args.repo:
    entries = [e for e in entries if args.repo in e['url']]
os.makedirs(args.out, exist_ok=True)
args.out = os.path.realpath(args.out)
if not args.upstream_bin:
    # As in strict-compile.sh, upstream's translator is built once beside the output.
    args.upstream_bin = os.path.join(os.path.dirname(args.out), 'upstream-cppfront')
    if not os.path.exists(args.upstream_bin):
        subprocess.run([args.cxx, '-std=c++20', '-O2',
                        os.path.join(args.upstream, 'source', 'cppfront.cpp'),
                        '-o', args.upstream_bin], check=True, timeout=600)
GIT = ['git', '-c', 'core.hooksPath=/dev/null', '-c', 'core.symlinks=false',
       '-c', 'protocol.file.allow=never', '-c', 'submodule.recurse=false']
GIT_ENV = dict(os.environ, GIT_TERMINAL_PROMPT='0', GIT_LFS_SKIP_SMUDGE='1',
               GIT_CONFIG_NOSYSTEM='1')


def matches(path, patterns):
    return any(pathlib.PurePosixPath(path).full_match(p) for p in patterns)


def git(*command, cwd, **kwargs):
    return subprocess.run(GIT + list(command), cwd=cwd, env=GIT_ENV, check=True,
                          capture_output=True, timeout=600, **kwargs)


def fetch(entry, dest):
    """Check out the selected files of `entry` at its commit into `dest`, and
    return the selected source paths."""
    stamp = os.path.join(dest, '.git', 'wild-corpus-files')
    if args.reuse and os.path.exists(stamp):
        head = git('rev-parse', 'FETCH_HEAD', cwd=dest, text=True).stdout.strip()
        if head == entry['commit']:
            return json.load(open(stamp))
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    git('init', '-q', cwd=dest)
    git('fetch', '-q', '--depth', '1', '--filter=blob:none', '--no-tags', entry['url'],
        entry['commit'], cwd=dest)
    listing = git('ls-tree', '-r', '-z', 'FETCH_HEAD', cwd=dest).stdout.split(b'\0')
    files = []
    for line in listing:
        if not line:
            continue
        mode_type, _, path = line.partition(b'\t')
        if mode_type.startswith(b'100'):  # regular files only, not links or submodules
            files.append(path.decode(errors='replace'))
    exclude = entry.get('exclude', [])
    sources = [f for f in files if not matches(f, exclude) and
               (matches(f, entry.get('paths', [])) or matches(f, entry.get('markdown', [])))]
    tops = {s.split('/')[0] if '/' in s else '' for s in sources}
    headers = [f for f in files if f.endswith(HEADERS) and f not in sources and
               (f.split('/')[0] if '/' in f else '') in tops]
    if sources:
        git('checkout', '-q', 'FETCH_HEAD', '--pathspec-from-file=-', '--pathspec-file-nul',
            cwd=dest, input=b'\0'.join(p.encode() for p in sources + headers))
    json.dump(sources, open(stamp, 'w'))
    return sources


FENCE = re.compile(r'^(\s*)```\s*cpp\b')


def markdown_blocks(text):
    """The ``` cpp code blocks of a Markdown document, dedented."""
    blocks, lines = [], text.splitlines()
    i = 0
    while i < len(lines):
        start = FENCE.match(lines[i])
        i += 1
        if not start:
            continue
        indent, body = len(start.group(1)), []
        while i < len(lines) and lines[i].strip() != '```':
            body.append(lines[i][indent:] if not lines[i][:indent].strip() else lines[i].lstrip())
            i += 1
        i += 1
        blocks.append('\n'.join(body) + '\n')
    return blocks


def first_error(text):
    for line in text.splitlines():
        if 'error' in line:
            return line.strip()[:300]
    lines = [l for l in text.splitlines() if l.strip()]
    return lines[-1].strip()[:300] if lines else ''


def translate(binary, source, output):
    os.makedirs(os.path.dirname(output), exist_ok=True)
    try:
        result = subprocess.run([binary, os.path.basename(source), '-o', output],
                                cwd=os.path.dirname(source), capture_output=True, text=True,
                                errors='replace', timeout=60)
    except subprocess.TimeoutExpired:
        return 'crash', 'timed out after 60 s'
    text = result.stdout + result.stderr
    if result.returncode == 0 and os.path.exists(output):
        return 'ok', text
    if result.returncode == 1 and 'Sanitizer' not in text and 'runtime error' not in text:
        return 'reject', text
    return 'crash', f'status {result.returncode}: ' + text[-300:]


def compile_cpp1(runtime, cpp1, includes, pure):
    command = [args.cxx, '-std=c++20', '-fsyntax-only', '-I' + runtime]
    # As upstream's -include-std, which pure Cpp2 needs for the standard library.
    command += ['-DCPP2_INCLUDE_STD=Yes'] if pure else []
    command += ['-I' + d for d in includes] + ['-x', 'c++', cpp1]
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors='replace',
                                timeout=120)
    except subprocess.TimeoutExpired:
        return False, 'compiler timed out after 120 s'
    return result.returncode == 0, first_error(result.stderr)


def output_name(path):
    stem, extension = os.path.splitext(path)
    return stem + ('.h' if extension == '.h2' else '.cpp')


# Fetch every repository, and collect (id, repository, source file, checkout).
work = os.path.join(args.out, 'src')
if not args.reuse:
    shutil.rmtree(work, ignore_errors=True)
items, fetch_failures = [], []


def fetch_entry(entry):
    dest = os.path.join(work, repo_name(entry['url']).replace('/', '__'))
    try:
        return dest, fetch(entry, dest)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        return dest, (getattr(error, 'stderr', b'') or b'').decode(errors='replace').strip()[:200]


with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
    fetched = list(pool.map(fetch_entry, entries))
for entry, (dest, sources) in zip(entries, fetched):
    name = repo_name(entry['url'])
    if isinstance(sources, str):
        fetch_failures.append(f'{name}: {sources}')
        continue
    for path in sources:
        if path.endswith('.md'):
            text = open(os.path.join(dest, path), errors='replace').read()
            for n, block in enumerate(markdown_blocks(text), 1):
                block_path = f'{path[:-3]}.md-blocks/{n}.cpp2'
                os.makedirs(os.path.dirname(os.path.join(dest, block_path)), exist_ok=True)
                open(os.path.join(dest, block_path), 'w').write(block)
                items.append((f'{name}:{path}#{n}', name, block_path, dest))
        else:
            items.append((f'{name}:{path}', name, path, dest))
upstream_runtime = os.path.realpath(os.path.join(args.upstream, 'include'))
our_runtime = os.path.join(repo, 'runtime')


def translate_item(item):
    ident, name, path, dest = item
    record = {'id': ident, 'repository': name, 'path': path}
    source = os.path.join(dest, path)
    for side, binary in (('ours', args.bin), ('upstream', args.upstream_bin)):
        output = os.path.join(args.out, side, name.replace('/', '__'), output_name(path))
        status, text = translate(binary, source, output)
        record[side] = status
        record[side + '_message'] = first_error(text) if status != 'ok' else ''
        if side == 'upstream' and status == 'ok':
            record['pure'] = 'all Cpp2' in text
    return record


def compile_item(record):
    name, path = record['repository'], record['path']
    checkout = os.path.join(work, name.replace('/', '__'))
    for side, runtime in (('ours', our_runtime), ('upstream', upstream_runtime)):
        root = os.path.join(args.out, side, name.replace('/', '__'))
        cpp1 = os.path.join(root, output_name(path))
        includes = [os.path.dirname(cpp1), os.path.dirname(os.path.join(checkout, path)),
                    root, checkout]
        ok, message = compile_cpp1(runtime, cpp1, includes, record.get('pure', False))
        record[side + '_cpp1'] = 'ok' if ok else 'fail'
        record[side + '_cpp1_message'] = message
    return record


def classify(record):
    ours, upstream = record['ours'], record['upstream']
    if ours == 'crash':
        return 'ours-crash'
    if ours == 'reject':
        return 'both-reject' if upstream != 'ok' else 'only-ours-rejects'
    if upstream != 'ok':
        return 'only-upstream-rejects'
    ours_ok, upstream_ok = record['ours_cpp1'] == 'ok', record['upstream_cpp1'] == 'ok'
    if ours_ok == upstream_ok:
        return 'both-accept'
    return 'upstream-cpp1-fails' if ours_ok else 'ours-cpp1-fails'


shutil.rmtree(os.path.join(args.out, 'ours'), ignore_errors=True)
shutil.rmtree(os.path.join(args.out, 'upstream'), ignore_errors=True)
with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
    records = list(pool.map(translate_item, items))
    both = [r for r in records if r['ours'] == 'ok' and r['upstream'] == 'ok']
    list(pool.map(compile_item, both))
known_used = set()
rejected = {(r['repository'], output_name(r['path'])) for r in records if r['ours'] != 'ok'}
for record in records:
    record['category'] = classify(record)
    # Cpp1 that includes the header of a `.h2` file we reject fails for that reason.
    missing = re.search(r"fatal error: ([\w./-]+\.h): No such file",
                        record.get('ours_cpp1_message', ''))
    if record['category'] == 'ours-cpp1-fails' and missing and any(
            repository == record['repository'] and
            (path == missing.group(1) or path.endswith('/' + missing.group(1)))
            for repository, path in rejected):
        record['known'] = f'includes {missing.group(1)}2, which cppfrontier rejects'
    elif record['category'] in FAILING:
        for pattern, reason in KNOWN:
            if fnmatch.fnmatchcase(record['id'], pattern):
                record['known'] = reason
                known_used.add(pattern)
                break

counts = {c: sum(r['category'] == c for r in records) for c in CATEGORIES}
unexplained = [r for r in records if (r['category'] in FAILING and 'known' not in r) or
               r['category'] == 'ours-crash']
lines = [f'{len(entries)} repositories, {len(items)} files '
         f'({sum(r.get("pure", False) for r in records)} pure Cpp2 per upstream)']
lines += [f'  {c}: {counts[c]}' for c in CATEGORIES]
both_fail = sum(r['category'] == 'both-accept' and r.get('ours_cpp1') == 'fail' for r in records)
lines.append(f'  (both-accept includes {both_fail} files whose Cpp1 fails to compile in both)')
known_count = sum('known' in r for r in records)
lines.append(f'{known_count} explained by KNOWN, {len(unexplained)} unexplained')
lines += [f'fetch failed: {f}' for f in fetch_failures]
lines += [f'unexplained {r["category"]}: {r["id"]}: '
          f'{r.get("ours_message") or r.get("ours_cpp1_message", "")}' for r in unexplained]
stale = [p for p, _ in KNOWN if p not in known_used and
         any(p.split(':')[0] == repo_name(e['url']) for e in entries)]
lines += [f'KNOWN entry matched nothing: {p}' for p in stale]
open(os.path.join(args.out, 'summary.txt'), 'w').write('\n'.join(lines) + '\n')
with open(os.path.join(args.out, 'details.txt'), 'w') as details:
    for r in records:
        if r['category'] == 'both-accept':
            continue
        details.write(f'{r["category"]} {r["id"]}\n')
        for key in ('ours_message', 'upstream_message', 'ours_cpp1_message',
                    'upstream_cpp1_message', 'known'):
            if r.get(key):
                details.write(f'    {key}: {r[key]}\n')
json.dump(records, open(os.path.join(args.out, 'details.json'), 'w'), indent=1)
print('\n'.join(lines))
print('details in', args.out)
raise SystemExit(1 if unexplained or fetch_failures else 0)
