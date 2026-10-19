"""difftest.py: the lifter against a reference ARM emulator (Unicorn), one instruction at a time.

  difftest.py [--kind simd|int|mem|all] [--trials N] [--out DIR] <nso>...

Every distinct instruction form the modules use (registers renamed Rd=0 Rn=1 Rm=2 Ra=3; immediates kept) is lifted to
C by tools/lifter.py (simd='c', as on x86), compiled into a harness and run on random register states (FP specials,
FPCR FZ / DN / rounding variants; for loads / stores x1 points into a 1 MB buffer mapped at the same address on both
sides). The same instruction runs in Unicorn on the same states; registers, flags, sp and the buffer (crc32) must
match (NaN payloads aside). Differences go to DIR/mismatch_<kind>.txt, most used forms first.

Needs: pip install unicorn capstone (and lz4 for tools/nso.py), clang, zlib. 2026-10-19: no difference in Legends
Z-A's main + sdk and Legends: Arceus' main (1468 FP/SIMD, 114813 integer, 48615 load/store forms).
"""
import argparse
import collections
import os
import random
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from nso import Nso  # noqa: E402
from lifter import Lifter  # noqa: E402

PC = 0x7000001000
MEM = 0x40000000


def ren(w, pos, new):  # register field renamed, unless 31 (zr / sp)
    r = (w >> pos) & 31
    return w if r == 31 else (w & ~(31 << pos)) | (new << pos)


def form_simd(w):
    if ((w >> 25) & 7) != 7:
        return None
    if (w & 0x5F201C00) == 0x1E201000 or (w & 0x9FF80400) == 0x0F000400:  # FMOV imm, MOVI...: bits 5-9 are the immediate
        return w & ~0x1F
    w = (w & ~0x3FF) | (1 << 5)
    m = lambda w: (w & ~(31 << 16)) | (2 << 16)  # noqa: E731
    if (w & 0x9F200400) == 0x0E200400 or (w & 0x9F200C00) == 0x0E200000: return m(w)    # 3 same / 3 different
    if (w & 0xDF200400) == 0x5E200400 or (w & 0xDF200C00) == 0x5E200000: return m(w)    # scalar 3 same / different
    if (w & 0x5F200C00) in (0x1E200800, 0x1E200C00) or (w & 0x5F200400) == 0x1E200400: return m(w)  # FP 2-src, FCSEL, FCCMP
    if (w & 0x5F000000) == 0x1F000000: return (m(w) & ~(31 << 10)) | (3 << 10)          # FP 3-source
    if (w & 0x5F203C00) == 0x1E202000: return w if w & 8 else m(w)                      # FCMP (#0.0: no Rm)
    if (w & 0xBF000400) == 0x0F000000: return (w & ~(15 << 16)) | (2 << 16)             # by element
    if (w & 0xBFE08C00) == 0x0E000000 or (w & 0xBF208C00) == 0x0E000800 or (w & 0xBF208400) == 0x2E000000:
        return m(w)                                                                     # TBL/TBX, ZIP/UZP/TRN, EXT
    return w


def form_int(w):
    op0 = (w >> 25) & 15
    if (op0 & 0b1110) == 0b1000:  # dp immediate
        w = ren(w, 0, 0)
        cls = (w >> 23) & 7
        if cls in (0b010, 0b011, 0b100, 0b110, 0b111): w = ren(w, 5, 1)
        return ren(w, 16, 2) if cls == 0b111 else w
    if (op0 & 0b0111) == 0b0101:  # dp register
        w = ren(ren(w, 0, 0), 5, 1)
        if (w & 0x1FE00000) == 0x1AC00000 and ((w >> 30) & 1): return w  # 1-source
        w = ren(w, 16, 2)
        return ren(w, 10, 3) if (w & 0x1F000000) == 0x1B000000 else w
    return None


def form_mem(w):
    if (((w >> 25) & 15) & 0b0101) != 0b0100: return None
    if (w & 0x3B000000) == 0x18000000 or (w & 0x3F000000) == 0x08000000: return None  # literals, exclusives
    w = ren(ren(w, 0, 0), 5, 1)
    if (w & 0x3A000000) == 0x28000000: return ren(w, 10, 3)
    if (w & 0xBF000000) in (0x0C000000, 0x0D000000): return ren(w, 16, 2) if w & 0x00800000 else w  # SIMD structures
    if (w & 0x3B200C00) == 0x38200800: return ren(w, 16, 2)
    return w


KINDS = {'simd': form_simd, 'int': form_int, 'mem': form_mem}

HARNESS_MAIN = r'''
// state: v0..v7, x0..x3, nzcv, fpcr, sp (184 bytes); out: v0..v7, x0..x3, nzcv, sp, crc32 of the buffer (180)
int main(int argc, char **argv) {
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    unsigned char st[184], (*states)[184] = NULL, *mem = NULL, *imgs = NULL;
    static Cpu c __attribute__((aligned(16)));
    int n = 0;
    while (fread(st, 1, 184, in) == 184) { states = realloc(states, (n + 1) * 184); memcpy(states[n++], st, 184); }
    if (argc > 3) {
        mem = mmap((void *)0x40000000, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        imgs = malloc((size_t)n << 20);
        FILE *fm = fopen(argv[3], "rb");
        if (fread(imgs, 1, (size_t)n << 20, fm) != (size_t)n << 20) return 1;
        fclose(fm);
    }
    for (unsigned t = 0; t < sizeof tests / sizeof *tests; t++)
        for (int k = 0; k < n; k++) {
            memset(&c, 0, sizeof c);
            memcpy(c.v, states[k], 128); memcpy(c.x, states[k] + 128, 32);
            memcpy(&c.nzcv, states[k] + 160, 8); memcpy(&c.fpcr, states[k] + 168, 8); memcpy(&c.sp, states[k] + 176, 8);
            c.host_fpcr = 1;  /* (simd.h: the host FP state left by the previous test is unknown to this one) */
            if (mem) memcpy(mem, imgs + ((size_t)k << 20), 1 << 20);
            tests[t](&c);
            uint32_t crc = mem ? (uint32_t)crc32(0, mem, 1 << 20) : 0;
            fwrite(c.v, 1, 128, out); fwrite(c.x, 1, 32, out); fwrite(&c.nzcv, 1, 8, out); fwrite(&c.sp, 1, 8, out);
            fwrite(&crc, 1, 4, out);
        }
    fclose(out);
    return 0;
}
'''


def states_for(kind, n, rnd):
    f32 = lambda x: struct.unpack('<I', struct.pack('<f', x))[0]  # noqa: E731
    f64 = lambda x: struct.unpack('<Q', struct.pack('<d', x))[0]  # noqa: E731
    sp32 = [0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0x7fa00000, 1, 0x807fffff, 0x7f7fffff, 0x3f800000,
            0xbf800000, 0x4f000000, 0xcf000000, 0x5f000000, 0x00800000]
    sp64 = [0, 1 << 63, 0x7ff0 << 48, 0xfff0 << 48, 0x7ff8 << 48, 0x7ff4 << 48, 1, 0x3ff0 << 48, 0xbff0 << 48,
            0x41e0 << 48, 0xc3e0 << 48, 0x0010 << 48]

    def lane(bits):
        r = rnd.random()
        if r < 0.35: return f32(rnd.uniform(-1000, 1000)) if bits == 32 else f64(rnd.uniform(-1e6, 1e6))
        if r < 0.6: return f32(rnd.uniform(-2, 2)) if bits == 32 else f64(rnd.uniform(-2, 2))
        if r < 0.7: return rnd.choice(sp32 if bits == 32 else sp64)
        if r < 0.8: return rnd.randrange(-300, 300) & ((1 << bits) - 1)
        return rnd.getrandbits(bits)

    out = []
    for k in range(n):
        v = [sum(lane(32) << (32 * i) for i in range(4)) if rnd.random() < 0.5 else lane(64) | (lane(64) << 64)
             for _ in range(8)]
        x = [lane(64) if rnd.random() < 0.5 else rnd.getrandbits(64) for _ in range(4)]
        fpcr = [0, 0, 1 << 24, (1 << 24) | (1 << 25), 1 << 25, 1 << 22][k % 6]
        if kind == 'mem':
            x[1], x[2] = MEM + 0x80000 + 16 * rnd.randrange(256), rnd.randrange(256)
            sp = MEM + 0x90000 + 16 * rnd.randrange(256)
        else:
            sp = 0x7ffff0000 + 16 * rnd.randrange(4096)
        out.append((v, x, rnd.randrange(16) << 28, fpcr, sp))
    return out


def isnan(u, bits):
    e, m = (u >> 23) & 0xff, u & 0x7fffff
    if bits == 64: e, m = (u >> 52) & 0x7ff, u & ((1 << 52) - 1)
    return e == (0xff if bits == 32 else 0x7ff) and m


def same_ish(a, b):  # equal, or every differing 32- (or 64-) bit lane a NaN in both
    if a == b: return True
    for bits in (32, 64):
        mask = (1 << bits) - 1
        if all(((a >> s) & mask) == ((b >> s) & mask) or (isnan((a >> s) & mask, bits) and isnan((b >> s) & mask, bits))
               for s in range(0, 128, bits)):
            return True
    return False


def run(kind, modules, trials, outdir):
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn import arm64_const as A
    import capstone
    cnt = collections.Counter()
    for path in modules:
        n = Nso(path)
        off, size = n.text
        for (w,) in struct.iter_unpack('<I', n.image[off:off + size]):
            f = KINDS[kind](w)
            if f is not None: cnt[f] += 1
    L = Lifter(0x7000000000, simd='c')
    forms, fns = [], []
    for w, _ in cnt.most_common():
        try:
            ins = L.lift(w, PC)
        except Exception:
            continue
        if ins.kind == 'op':
            fns.append('static void t%d(Cpu *c) { %s }' % (len(forms), ins.code))
            forms.append(w)
    print('%s: %d forms (%d lifted)' % (kind, len(cnt), len(forms)))
    src, exe = os.path.join(outdir, 'harness_%s.c' % kind), os.path.join(outdir, 'harness_%s' % kind)
    with open(src, 'w') as f:
        f.write('#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <sys/mman.h>\n#include <zlib.h>\n'
                '#include "cpu.h"\n#include "simd.h"\n' + '\n'.join(fns) + '\n')
        f.write('static void (*const tests[])(Cpu *) = {%s};\n' % ', '.join('t%d' % i for i in range(len(forms))))
        f.write(HARNESS_MAIN)
    subprocess.run(['clang', '-O1', '-fno-strict-aliasing', '-fdenormal-fp-math=dynamic', '-w', '-march=x86-64-v3', '-I' + os.path.join(HERE, '..', 'runtime'),
                    src, '-o', exe, '-lm', '-lz'], check=True)
    rnd = random.Random(1234)
    states = states_for(kind, trials, rnd)
    images = [random.Random(1000 + k).randbytes(1 << 20) for k in range(trials)] if kind == 'mem' else []
    sfile, lfile, mfile = (os.path.join(outdir, x) for x in ('states.bin', 'lifted_%s.bin' % kind, 'mem.bin'))
    with open(sfile, 'wb') as f:
        for v, x, nzcv, fpcr, sp in states:
            f.write(b''.join(r.to_bytes(16, 'little') for r in v) + b''.join(r.to_bytes(8, 'little') for r in x) +
                    struct.pack('<QQQ', nzcv, fpcr, sp))
    if images:
        with open(mfile, 'wb') as f:
            for im in images: f.write(im)
    subprocess.run([exe, sfile, lfile] + ([mfile] if images else []), check=True)
    lifted = open(lfile, 'rb').read()
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    mu.mem_map(PC & ~0xfff, 0x1000)
    mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 3 << 20)
    if images: mu.mem_map(MEM, 1 << 20)
    Q = [A.UC_ARM64_REG_Q0 + i for i in range(8)]
    X = [A.UC_ARM64_REG_X0 + i for i in range(4)]
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    bad = []
    for t, w in enumerate(forms):
        mu.mem_write(PC, struct.pack('<I', w))
        nbad, example = 0, None
        for k, (v, x, nzcv, fpcr, sp) in enumerate(states):
            for i in range(8): mu.reg_write(Q[i], v[i])
            mu.reg_write(A.UC_ARM64_REG_Q31, 0)  # (31 is not renamed in the vector register fields: zero, as in the harness)
            for i in range(4): mu.reg_write(X[i], x[i])
            mu.reg_write(A.UC_ARM64_REG_NZCV, nzcv)
            mu.reg_write(A.UC_ARM64_REG_FPCR, fpcr)
            mu.reg_write(A.UC_ARM64_REG_SP, sp)
            if images: mu.mem_write(MEM, images[k])
            try:
                mu.emu_start(PC, PC + 4)
            except Exception as e:
                nbad, example = -1, 'unicorn: %s' % e
                break
            o = (t * len(states) + k) * 180
            lv = [int.from_bytes(lifted[o + 16 * i:o + 16 * i + 16], 'little') for i in range(8)]
            lx = [int.from_bytes(lifted[o + 128 + 8 * i:o + 136 + 8 * i], 'little') for i in range(4)]
            lf, lsp, lcrc = struct.unpack_from('<QQI', lifted, o + 160)
            rv = [mu.reg_read(r) for r in Q]
            rx = [mu.reg_read(r) for r in X]
            ok = (all(same_ish(rv[i], lv[i]) for i in range(8)) and rx == lx and
                  (mu.reg_read(A.UC_ARM64_REG_NZCV) & 0xf0000000) == (lf & 0xf0000000) and mu.reg_read(A.UC_ARM64_REG_SP) == lsp and
                  (not images or zlib.crc32(mu.mem_read(MEM, 1 << 20)) == lcrc))
            if not ok:
                nbad += 1
                if example is None:
                    example = 'state %d: v %s x %s' % (k, [(i, hex(rv[i]), hex(lv[i])) for i in range(8) if not same_ish(rv[i], lv[i])][:2],
                                                      [(i, hex(rx[i]), hex(lx[i])) for i in range(4) if rx[i] != lx[i]][:2])
        if nbad:
            dis = ' '.join(i.mnemonic + ' ' + i.op_str for i in md.disasm(struct.pack('<I', w), 0)) or '?'
            bad.append((cnt[w], w, dis, nbad, example))
    bad.sort(reverse=True)
    with open(os.path.join(outdir, 'mismatch_%s.txt' % kind), 'w') as f:
        for uses, w, dis, nbad, ex in bad:
            f.write('%08x %-40s uses %6d bad %3d/%d  %s\n' % (w, dis, uses, nbad, len(states), ex))
    print('%s: %d of %d forms differ%s' % (kind, len(bad), len(forms), ' (mismatch_%s.txt)' % kind if bad else ''))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--kind', default='all', choices=['all'] + sorted(KINDS))
    ap.add_argument('--trials', type=int, default=0, help='states per form (default: simd 100, int 12, mem 6)')
    ap.add_argument('--out', default='build/difftest')
    ap.add_argument('modules', nargs='+')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for kind in (sorted(KINDS) if a.kind == 'all' else [a.kind]):
        run(kind, a.modules, a.trials or {'simd': 100, 'int': 12, 'mem': 6}[kind], a.out)


if __name__ == '__main__':
    main()
