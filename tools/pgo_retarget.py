#!/usr/bin/env python3
"""Retargets the runtime's PGO profile (data/pgo/runtime.proftext) to another clang / standard library.

The profile is trained once with the host clang on Linux (tools/make_pgo_profile.sh). A release compiler of another
version or for another target looks functions up by its own names and hashes, which differ in two ways:
  * names: libc++ (llvm-mingw) mangles std::vector as std::__1::vector, size_t is unsigned long long on Windows;
  * hashes: clang versions encode the top 4 bits of the 64-bit function hash differently (the low 60 bits, which
    describe the function's control flow, are the same for the same source).
The target compiler lists its own (name, hash, counters) from instrumented IR (see tools/pgo_target_names.sh); every
profiled function is matched to a target function by demangled signature (normalized for the differences above) or,
failing that, by base name + low 60 hash bits + counter count, and written under the target's name and hash. Functions
whose control flow differs (low 60 bits) are left out: clang ignores them then, as it would ignore stale data.

  pgo_retarget.py <profile.proftext> <target names file> <out.proftext>
"""
import collections
import subprocess
import sys

MASK60 = (1 << 60) - 1


def read_profile(path):
    entries = []
    blocks = open(path).read().split('\n\n')
    for blk in blocks:
        b = [l for l in blk.strip().split('\n') if l != '']
        if len(b) < 6 or b[1] != '# Func Hash:' or b[3] != '# Num Counters:' or b[5] != '# Counter Values:':
            continue
        entries.append({'name': b[0], 'hash': int(b[2]), 'n': int(b[4]), 'values': b[6:6 + int(b[4])]})
    return entries


def read_targets(path):
    out = []
    for line in open(path):
        p = line.split()
        if len(p) == 3:
            out.append({'name': p[0], 'hash': int(p[1]) & ((1 << 64) - 1), 'n': int(p[2])})
    return out


def split_prefix(name):  # "file.cpp:_Z..." (internal linkage) -> ("file.cpp", "_Z...")
    if ':' in name and not name.startswith('_Z'):
        f, m = name.split(':', 1)
        return f, m
    return '', name


def demangle(names):
    try:
        r = subprocess.run(['c++filt'], input='\n'.join(names) + '\n', capture_output=True, text=True, check=True)
        out = r.stdout.split('\n')
        if len(out) >= len(names):
            return out[:len(names)]
    except (OSError, subprocess.CalledProcessError):
        pass
    return list(names)


def normalize(s):
    for a, b in (('std::__1::', 'std::'), ('std::__cxx11::', 'std::'), ('unsigned long long', 'unsigned long'),
                 ('long long', 'long'), ('_IO_FILE', 'FILE'), ('_iobuf', 'FILE')):
        s = s.replace(a, b)
    return s


def base_name(dem):  # qualified name without the parameter list
    depth = 0
    for i, ch in enumerate(dem):
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        elif ch == '(' and depth == 0:
            return dem[:i]
    return dem


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    prof, targets = read_profile(sys.argv[1]), read_targets(sys.argv[2])
    pd = demangle([split_prefix(e['name'])[1] for e in prof])
    td = demangle([split_prefix(t['name'])[1] for t in targets])
    by_sig = collections.defaultdict(list)
    by_base = collections.defaultdict(list)
    for t, d in zip(targets, td):
        f = split_prefix(t['name'])[0]
        by_sig[(f, normalize(d))].append(t)
        by_base[(f, normalize(base_name(d)), t['hash'] & MASK60, t['n'])].append(t)
    total = matched = 0
    out = []
    for e, d in zip(prof, pd):
        count = sum(int(v) for v in e['values'])
        total += count
        f = split_prefix(e['name'])[0]
        cands = [t for t in by_sig.get((f, normalize(d)), []) if (t['hash'] & MASK60) == (e['hash'] & MASK60) and t['n'] == e['n']]
        if len(cands) != 1:
            cands = by_base.get((f, normalize(base_name(d)), e['hash'] & MASK60, e['n']), [])
        if len(cands) != 1:
            continue
        t = cands[0]
        matched += count
        out.append(f"{t['name']}\n# Func Hash:\n{t['hash']}\n# Num Counters:\n{t['n']}\n# Counter Values:\n" + '\n'.join(e['values']) + '\n')
    with open(sys.argv[3], 'w') as o:
        o.write('\n'.join(out))
    print(f'pgo_retarget: {len(out)} of {len(prof)} functions, {100.0 * matched / max(total, 1):.1f}% of the counts')
    return 0


if __name__ == '__main__':
    sys.exit(main())
