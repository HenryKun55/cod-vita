#!/usr/bin/env python3
"""Decompilation progress: how much of the game's code runs as hand-written native C.

Total = the generated functions (dispatch_table.c of a build), each sized up to the
next function's start. Native = the guest code the NATIVE and BLOCK_HOOKS tables of
gen.py reproduce (their pieces' sizes). With call counts (calls.txt of an
FF_CALLCOUNT build, see runtime/src/profile.c) it also gives the share of calls that
enter a native function, and lists the hottest functions still recompiled.

usage: tools/recomp/status.py [--gen build-vita/gen] [--calls calls.txt ...]
                              [--json docs/decomp_status.json] [--top 40]
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen  # noqa: E402  (the NATIVE / BLOCK_HOOKS tables)


def functions(gen_dir):
    with open(os.path.join(gen_dir, 'dispatch_table.c')) as f:
        starts = sorted({int(m, 16) for m in re.findall(r'GUEST_WRAPPER\(([0-9A-F]{8})\)', f.read())})
    sizes = {}
    for a, b in zip(starts, starts[1:] + [starts[-1] + 16]):
        sizes[a] = b - a
    return sizes


def load_calls(paths):
    """Max count per function over the dumps (each dump is one scene)."""
    calls = {}
    for p in paths:
        with open(p) as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2:
                    a, n = int(parts[0], 16), int(parts[1])
                    calls[a] = max(calls.get(a, 0), n)
    return calls


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default='build-vita/gen')
    ap.add_argument('--calls', nargs='*', default=[])
    ap.add_argument('--json')
    ap.add_argument('--top', type=int, default=40)
    args = ap.parse_args()

    sizes = functions(args.gen)
    total_bytes = sum(sizes.values())
    whole = {e: v[0] for e, v in gen.NATIVE.items()}
    native_bytes = sum(n for _, pieces in gen.NATIVE.items() for _, n, _ in pieces[1])
    native_bytes += sum(n for _, (_, _, pieces) in gen.BLOCK_HOOKS.items() for _, n, _ in pieces)
    # Functions a block hook makes partly native (counted by bytes only).
    def owner(a):
        best = None
        for s in sizes:
            if s <= a and (best is None or s > best):
                best = s
        return best
    partial = sorted({owner(a) for a in gen.BLOCK_HOOKS} - set(whole))

    print('functions: %d (%d bytes of code)' % (len(sizes), total_bytes))
    print('native: %d whole functions + %d with native loops; %d bytes = %.2f%% of the code' % (
        len(whole), len(partial), native_bytes, 100.0 * native_bytes / total_bytes))
    out = {
        'functions': len(sizes), 'code_bytes': total_bytes, 'native_bytes': native_bytes,
        'native_functions': {'%08X' % e: n for e, n in sorted(whole.items())},
        'native_loops': {'%08X' % a: v[0] for a, v in sorted(gen.BLOCK_HOOKS.items())},
    }
    if args.calls:
        calls = load_calls(args.calls)
        all_calls = sum(calls.values())
        native_calls = sum(n for a, n in calls.items() if a in whole)
        called = len(calls)
        print('calls: %d to %d functions; %.2f%% of calls enter a native function' % (
            all_calls, called, 100.0 * native_calls / max(all_calls, 1)))
        for lim in (1000, 10000, 100000):
            hot = [a for a, n in calls.items() if n >= lim]
            print('  %6d functions called >= %d times (%d bytes of code)' % (
                len(hot), lim, sum(sizes.get(a, 0) for a in hot)))
        print('hottest still recompiled (calls, size):')
        rest = sorted(((n, a) for a, n in calls.items() if a not in whole), reverse=True)
        for n, a in rest[:args.top]:
            print('  %08X %10d %6d%s' % (a, n, sizes.get(a, 0), '  (native loop)' if a in partial else ''))
        out.update(calls=all_calls, called_functions=called, native_calls=native_calls)
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(out, f, indent=1)
            f.write('\n')


if __name__ == '__main__':
    main()
