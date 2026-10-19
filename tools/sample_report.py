"""sample_report.py [dir=build/sample] [--top N] [--threads N]: the report of a LARECOMP_SAMPLE run (runtime/hle/sampler.c).

CPU milliseconds per presented frame for each thread (1 sample = 1 ms of that thread's CPU time), then the hottest
functions overall and in the busiest threads. Host pcs are resolved with nm in the runtime and the game library (game
functions are f_<guest address>); other libraries are shown by name only (+ nm -D symbols when it has them).
"""
import bisect
import collections
import os
import struct
import subprocess
import sys


def load_symbols(path, dynamic=False):
    try:
        out = subprocess.run(['nm', '-n'] + (['-D'] if dynamic else []) + ['--defined-only', path], capture_output=True,
                             text=True).stdout
    except OSError:
        return [], []
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in 'tTwWiI':
            addrs.append(int(p[0], 16))
            names.append(p[2])
    return addrs, names


def main():
    args, it = [], iter(sys.argv[1:])
    for a in it:
        if a.startswith('--'): next(it, None)
        else: args.append(a)
    d = args[0] if args else 'build/sample'
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 40
    nthr = int(sys.argv[sys.argv.index('--threads') + 1]) if '--threads' in sys.argv else 6
    frames, wall = open(os.path.join(d, 'frames.txt')).read().split()
    frames, wall = int(frames), int(wall)
    names = dict(l.split(None, 1) for l in open(os.path.join(d, 'threads.txt')).read().splitlines() if l.strip())
    maps = []  # (start, end, base, path)
    bases = {}
    for line in open(os.path.join(d, 'maps.txt')):
        p = line.split()
        if len(p) < 6 or 'x' not in p[1]:
            if len(p) >= 6 and int(p[2], 16) == 0: bases.setdefault(p[5], int(p[0].split('-')[0], 16))
            continue
        s, e = (int(x, 16) for x in p[0].split('-'))
        maps.append((s, e, p[5]))
        if int(p[2], 16) == 0: bases.setdefault(p[5], s)
    for line in open(os.path.join(d, 'maps.txt')):
        p = line.split()
        if len(p) >= 6 and p[5] not in bases: bases[p[5]] = int(p[0].split('-')[0], 16) - int(p[2], 16)
    maps.sort()
    starts = [m[0] for m in maps]
    syms = {}

    def resolve(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i < 0 or pc >= maps[i][1]: return '?'
        path = maps[i][2]
        mod = os.path.basename(path)
        if path not in syms:
            a, n = load_symbols(path)
            if not a: a, n = load_symbols(path, dynamic=True)
            syms[path] = (a, n)
        a, n = syms[path]
        va = pc - bases.get(path, maps[i][0])
        j = bisect.bisect_right(a, va) - 1
        if j < 0: return mod
        return n[j] if mod in ('lasrecomp', 'lasgame.so') else '%s (%s)' % (n[j], mod)

    raw = open(os.path.join(d, 'samples.bin'), 'rb').read()
    per_thread = collections.Counter()
    per_fn = collections.Counter()
    per_thread_fn = collections.defaultdict(collections.Counter)
    cache = {}
    for tid, _, pc in struct.iter_unpack('<IIQ', raw):
        f = cache.get(pc)
        if f is None:
            f = cache[pc] = resolve(pc)
        per_thread[tid] += 1
        per_fn[f] += 1
        per_thread_fn[tid][f] += 1
    total = sum(per_thread.values())
    print('%d frames in %.1f s (%.1f fps); %d samples = %.1f CPU ms per frame' % (frames, wall / 1e9, frames / (wall / 1e9),
                                                                                  total, total / frames))
    print('\nCPU ms per frame by thread:')
    for tid, c in per_thread.most_common(20):
        print('  %7.2f  %s (%d)' % (c / frames, names.get(str(tid), '?'), tid))
    print('\nhottest functions (all threads):')
    for f, c in per_fn.most_common(top):
        print('  %5.1f%%  %6.2f ms/frame  %s' % (100.0 * c / total, c / frames, f))
    for tid, c in per_thread.most_common(nthr):
        print('\n%s (%d): %.2f ms/frame' % (names.get(str(tid), '?'), tid, c / frames))
        for f, k in per_thread_fn[tid].most_common(12):
            print('  %5.1f%%  %s' % (100.0 * k / c, f))


if __name__ == '__main__':
    main()
