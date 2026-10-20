"""Generate C for a whole NSO module.

Usage: gen.py <name> <nso> <base> <outdir> [--hle hle_symbols.txt] [--chunk N] [--simd asm|c]

--simd: FP/SIMD/CRC32 as AArch64 inline asm (default on arm64 hosts) or portable C (default elsewhere).
"""
import argparse
import bisect
import os
import platform
import re
import struct
import sys
import time
from collections import Counter

import unicorn as uc
from unicorn import arm64_const as A

from analyze import eh_frame_functions, eh_frame_ranges
from lifter import Insn, Lifter, Unsupported, bits, emit_asm_batch, hx, sext
from nso import Nso, R_AARCH64_RELATIVE

SCRATCH_MEM = 0x7300000000


def sext26(w):
    v = w & 0x03FFFFFF
    return v - 0x04000000 if v & 0x02000000 else v


def fname(a):
    return f'f_{a:x}'


def sanitize(sym):
    return 'hle_' + re.sub(r'[^A-Za-z0-9_]', '_', sym)


ALL_LABELS_MAX = 1024  # instructions: above this, only unresolved tables get the all-instruction fallback
OPTNONE_MIN = 2048     # instructions: a function this big with the fallback is compiled without optimization


class Module:
    def __init__(self, name, path, base, simd='asm', trace_stores=False):
        self.name = name
        self.nso = Nso(path)
        self.base = base
        n = self.nso
        self.lo = base + n.text[0]
        self.hi = base + n.text[0] + n.text[1]
        self.words = struct.unpack(f'<{n.text[1] // 4}I', bytes(n.image[n.text[0]:n.text[0] + n.text[1]]))
        self.lifter = Lifter(base, simd, trace_stores)
        self.cache = {}
        self.stats = Counter()
        self.mu = None
        self.setjmp_targets = self._setjmp_targets()
        self.stats['setjmp_sites'] = 0

    SETJMP_NAMES = {'setjmp', '_setjmp', '__setjmp', 'sigsetjmp', '__sigsetjmp'}

    def _setjmp_targets(self):
        """Addresses whose call is a setjmp: the module's own setjmp functions and its PLT stubs for imported ones
        (adrp x16, page; ldr x17, [x16, #off]; add x16, x16, #off; br x17 -> the GOT slot's JUMP_SLOT symbol)."""
        n, out = self.nso, set()
        for name, info, shndx, value, size in n.syms:
            if shndx and name in self.SETJMP_NAMES:
                out.add(self.base + value)
        slots = {off: n.syms[sym][0] for off, typ, sym, add in n.relocs if typ == 1026 and sym}  # R_AARCH64_JUMP_SLOT
        for i in range(3, len(self.words)):
            if self.words[i] != 0xD61F0220:  # br x17
                continue
            adrp, ldr = self.words[i - 3], self.words[i - 2]
            if (adrp & 0x9F00001F) != 0x90000010 or (ldr & 0xFFC003FF) != 0xF9400211:
                continue
            pc = self.lo + (i - 3) * 4
            imm = ((adrp >> 5) & 0x7FFFF) << 2 | ((adrp >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            slot = (pc & ~0xFFF) + (imm << 12) + ((ldr >> 10) & 0xFFF) * 8 - self.base
            if slots.get(slot) in self.SETJMP_NAMES:
                out.add(pc)
        return out

    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

    def word(self, a):
        return self.words[(a - self.lo) // 4]

    def lift(self, a):
        ins = self.cache.get(a)
        if ins is None:
            try:
                ins = self.lifter.lift(self.word(a), a)
            except Unsupported as e:
                self.stats['unsupported'] += 1
                ins = Insn('trap', f'rt_udf(c, {hx(a)}); /* {e} */')
            self.cache[a] = ins
        return ins

    def boundaries(self):
        n, b = self.nso, self.base
        starts = set()
        for a in eh_frame_functions(n):
            starts.add(b + a)
        for name, info, shndx, value, size in n.syms:
            if shndx and (info & 0xF) == 2:
                starts.add(b + value)
        # code pointers in data: function addresses, but also label tables of computed gotos (musl's printf_core in
        # newer SDKs) whose targets lie inside a function; those stay labels of their function (L_dispatch)
        ranges = sorted({(b + a, sz) for a, sz in eh_frame_ranges(n)} |
                        {(b + value, size) for name, info, shndx, value, size in n.syms
                         if shndx and (info & 0xF) == 2 and size})
        range_starts = [r[0] for r in ranges]
        def inside(a):
            i = bisect.bisect_right(range_starts, a) - 1
            for j in range(i, max(i - 4, -1), -1):  # (overlapping ranges, e.g. a symbol and its FDE: a few back)
                if range_starts[j] < a < range_starts[j] + ranges[j][1]:
                    return True
            return False
        self.inside = inside
        self.interior_skipped = 0
        self.interior_ptrs = set()
        for off, typ, sym, add in n.relocs:
            if typ == R_AARCH64_RELATIVE:
                if inside(b + add):
                    self.interior_skipped += 1
                    self.interior_ptrs.add(b + add)
                    continue
                starts.add(b + add)
        starts = sorted(a for a in starts if self.in_text(a))
        return starts

    # ------------------------------------------------------------ jump tables

    def emu(self):
        if self.mu is None:
            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
            n = self.nso
            mu.mem_map(self.base, len(n.image))
            mu.mem_write(self.base, bytes(n.image))
            # read-only: stores replayed with made-up pointers must not change code or tables that later
            # resolutions read (the emulator is shared by every function of the module)
            mu.mem_protect(self.base, len(n.image), uc.UC_PROT_READ | uc.UC_PROT_EXEC)
            mu.mem_map(SCRATCH_MEM, 0x100000)
            mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
            self.mu = mu
        return self.mu

    def _step_to(self, frm, to, skip_faults=False):
        """Single-step the emulator from frm to to (Unicorn's `until` is unreliable mid translation-block). The image
        is read-only, so a store through a made-up pointer faults and is skipped. Before the cmp only address
        materialization matters (the index is set at the cmp), so with skip_faults any faulting instruction there
        (the index fetched through a chain of loads) is skipped too instead of giving up on the table."""
        mu = self.mu
        pc = frm
        for _ in range(64):
            if pc == to:
                return True
            try:
                mu.emu_start(pc, 0, count=1)
                pc = mu.reg_read(A.UC_ARM64_REG_PC)
            except uc.UcError:
                w = self.word(pc) if self.in_text(pc) else 0
                is_store = (w & 0x0A000000) == 0x08000000 and not bits(w, 22, 1) and not bits(w, 23, 1)
                if not (skip_faults or is_store):
                    raise
                pc += 4
        return False

    def jump_table(self, br_pc, entry, end):
        """Resolve `br xN` targets from a bounds-checked jump table, or None."""
        r = self._jump_table_fallthrough(br_pc, entry, end)
        if r is None:
            r = self._jump_table_taken(br_pc, entry, end)
        if r is None:
            r = self._jump_table_masked(br_pc, entry, end)
        return r

    def _jump_table_masked(self, br_pc, entry, end):
        """Form: and wI, wX, #(2^k - 1) ... <table lookup with wI> ... br. No bounds check: the mask bounds the index
        (small switches, e.g. on a vector lane: PLA's f_70027b6628 picks one of 4 lane inserts)."""
        a = br_pc - 4
        while a >= entry and br_pc - a <= 4 * 48:
            if self.lift(a).kind not in ('op', 'asm'):
                return None
            w = self.word(a)
            if (w & 0x7F800000) == 0x12000000:  # AND (immediate)
                n_bit, immr, imms = bits(w, 22, 1), bits(w, 16, 6), bits(w, 10, 6)
                # a run of low ones in a 32-bit (N=0, imms < 32) or 64-bit (N=1) element, not rotated
                if immr == 0 and (n_bit or imms < 32) and imms < 8:
                    s = a
                    while s - 4 >= entry and a - (s - 4) <= 4 * 12 and self.lift(s - 4).kind in ('op', 'asm'):
                        s -= 4
                    return self._emulate_table(br_pc, entry, end, s, w & 31, 1 << (imms + 1), a + 4)
            a -= 4
        return None

    def _index_regs(self, start, at, idx):
        """The index register plus registers holding the same value in start..at: sources it was
        copied/masked from, and copies of those (the table lookup sometimes recomputes the index
        from another copy after the bounds check)."""
        def copy_src(w):
            if (w & 0x7F800000) == 0x12000000:  # AND (imm)
                return bits(w, 5, 5)
            if (w & 0x7FE0FFE0) == 0x2A0003E0:  # MOV (ORR with zr)
                return bits(w, 16, 5)
            if (w & 0x7F800000) in (0x53000000, 0x13000000):  # UBFM/SBFM (uxtb, uxth, sxtw...)
                return bits(w, 5, 5)
            return None
        regs = {idx}
        changed = True
        while changed:
            changed = False
            for a in range(start, at, 4):
                w = self.word(a)
                rd, src = w & 31, copy_src(w)
                if src is None or src >= 31 or rd >= 31:
                    continue
                for x, y in ((rd, src), (src, rd)):
                    if x in regs and y not in regs:
                        regs.add(y)
                        changed = True
        return sorted(regs)

    def _set_index(self, mu, regs, i):
        for r in regs:
            mu.reg_write(A.UC_ARM64_REG_X0 + r if r < 29 else A.UC_ARM64_REG_X29, i)

    def _emulate_table(self, br_pc, entry, end, start, idx, count, set_at):
        """Run start..br for each index value (set at address set_at), collect targets."""
        mu = self.emu()
        rn = bits(self.word(br_pc), 5, 5)

        step_to = self._step_to

        targets = []
        try:
            for i in range(count):
                for r in range(29):
                    mu.reg_write(A.UC_ARM64_REG_X0 + r, SCRATCH_MEM + 0x80000)
                mu.reg_write(A.UC_ARM64_REG_SP, SCRATCH_MEM + 0x80000)
                a2 = entry
                while a2 < start:
                    w2 = self.word(a2)
                    if (w2 & 0x1F000000) == 0x10000000 or (w2 & 0xFF800000) == 0x91000000:
                        mu.emu_start(a2, 0, count=1)
                    a2 += 4
                if set_at != start and not step_to(start, set_at, skip_faults=True):
                    return None
                self._set_index(mu, [idx], i)
                if not step_to(set_at, br_pc):
                    return None
                t = mu.reg_read(A.UC_ARM64_REG_X0 + rn if rn < 29 else
                                (A.UC_ARM64_REG_X29 if rn == 29 else A.UC_ARM64_REG_X30))
                if not (entry <= t < end) or t % 4:
                    return None
                targets.append(t)
        except uc.UcError:
            return None
        return sorted(set(targets))

    def _jump_table_taken(self, br_pc, entry, end):
        """Form: cmp idx, #N ; b.ls/b.lo table ... table: <address math> ; br"""
        # start of the block holding the br
        s = br_pc
        while s - 4 >= entry and br_pc - (s - 4) <= 4 * 12 and self.lift(s - 4).kind in ('op', 'asm'):
            s -= 4
        for g in range(entry, end, 4):
            w = self.word(g)
            if (w & 0xFF000010) != 0x54000000 or bits(w, 0, 4) not in (9, 3):
                continue
            t = g + sext(bits(w, 5, 19), 19) * 4
            if t != s:
                continue
            cw = self.word(g - 4) if g - 4 >= entry else 0
            if (cw & 0x7F80001F) != 0x7100001F or bits(cw, 22, 1):
                continue
            limit = bits(cw, 10, 12)
            count = limit + 1 if bits(w, 0, 4) == 9 else limit
            if count <= 0 or count > 4096:
                continue
            return self._emulate_table(br_pc, entry, end, s, bits(cw, 5, 5), count, s)
        return None

    def _jump_table_fallthrough(self, br_pc, entry, end):
        rn = bits(self.word(br_pc), 5, 5)
        # find the guarding b.hi / b.hs and its cmp, scanning backwards
        a = br_pc - 4
        guard = cmp = None
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if (w & 0xFF000010) == 0x54000000 and bits(w, 0, 4) in (8, 2):
                guard = a
                break
            if self.lift(a).kind in ('b', 'bl', 'blr', 'br', 'ret', 'trap'):
                return None
            a -= 4
        if guard is None:
            return None
        a = guard - 4
        while a >= entry and guard - a <= 4 * 8:
            w = self.word(a)
            # SUBS (imm) with Rd = zr, i.e. CMP #imm
            if (w & 0x7F80001F) == 0x7100001F:
                cmp = a
                break
            if self.lift(a).kind != 'op':
                return None
            a -= 4
        if cmp is None:
            return None
        w = self.word(cmp)
        if bits(w, 22, 1):
            return None
        limit = bits(w, 10, 12)
        count = limit + 1 if bits(self.word(guard), 0, 4) == 8 else limit
        if count <= 0 or count > 4096:
            return None
        idx = bits(w, 5, 5)
        # window start: back from cmp to the first control-flow instruction
        start = cmp
        while start - 4 >= entry and cmp - (start - 4) <= 4 * 12 and self.lift(start - 4).kind in ('op', 'asm'):
            start -= 4
        idx_regs = self._index_regs(start, cmp, idx)
        mu = self.emu()
        targets = []

        step_to = self._step_to

        try:
            for i in range(count):
                for r in range(29):
                    mu.reg_write(A.UC_ARM64_REG_X0 + r, SCRATCH_MEM + 0x80000)
                mu.reg_write(A.UC_ARM64_REG_SP, SCRATCH_MEM + 0x80000)
                # replay address materialization (ADRP/ADR/ADD-imm) from the function entry, so table
                # bases computed before loops or branches are known
                a2 = entry
                while a2 < start:
                    w2 = self.word(a2)
                    if (w2 & 0x1F000000) == 0x10000000 or (w2 & 0xFF800000) == 0x91000000:
                        mu.emu_start(a2, 0, count=1)
                    a2 += 4
                if not step_to(start, cmp, skip_faults=True):
                    return None
                self._set_index(mu, idx_regs, i)
                if not step_to(cmp, br_pc):
                    return None
                t = mu.reg_read(A.UC_ARM64_REG_X0 + rn if rn < 29 else
                                (A.UC_ARM64_REG_X29 if rn == 29 else A.UC_ARM64_REG_X30))
                if not (entry <= t < end) or t % 4:
                    return None
                targets.append(t)
        except uc.UcError:
            return None
        return sorted(set(targets))

    # ------------------------------------------------------------ functions

    def computed_target(self, br_pc, entry):
        """True if the br register comes from address arithmetic (a table) rather than a load."""
        rn = bits(self.word(br_pc), 5, 5)
        a = br_pc - 4
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if self.lift(a).kind not in ('op', 'asm'):
                return False
            if (w & 31) == rn:
                if (w & 0x1F000000) == 0x0B000000:  # ADD/SUB (shifted or extended register)
                    return True
                if (w & 0x9F000000) == 0x10000000:  # ADR
                    return True
                return False
            a -= 4
        return False

    def scan(self, entry, end):
        """Collect labels and external entries referenced by a function."""
        labels, ext, tables = {entry}, set(), {}
        terminated = False
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if ins.kind in ('b', 'bcond'):
                t = ins.target
                if entry <= t < end:
                    labels.add(t)
                elif self.in_text(t):
                    ext.add(t)
            elif ins.kind == 'bl':
                if self.in_text(ins.target):
                    ext.add(ins.target)
            elif ins.kind == 'br':
                jt = self.jump_table(a, entry, end)
                if jt:
                    tables[a] = jt
                    labels.update(jt)
                    self.stats['jumptables'] += 1
                elif self.computed_target(a, entry):
                    # unresolved table jump: allow jumping to any instruction of the function
                    tables[a] = 'all'
                    self.stats['br_all_labels'] += 1
                else:
                    self.stats['br_dispatch'] += 1
            terminated = ins.kind in ('b', 'br', 'ret', 'trap')
        if not terminated and end < self.hi:
            ext.add(end)
        return labels, ext, tables

    def render_single(self, ins, a, jump, refs):
        """C for one lifted instruction (no batching); used for patch variants."""
        k = ins.kind
        if k == 'asm':
            return [emit_asm_batch([ins.asm])]
        if k in ('op', 'trap'):
            return [ins.code]
        if k == 'b':
            return [jump(ins.target)]
        if k == 'bcond':
            return [f'if ({ins.cond}) {{ {jump(ins.target)} }}']
        if k == 'bl':
            t = ins.target
            if self.in_text(t):
                refs.add(t)
                return [f'c->x[30] = {hx(a + 4)}; {fname(t)}(c);']
            return [f'c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);']
        if k == 'blr':
            return [f'c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);']
        if k == 'br':
            return [f'c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);']
        if k == 'ret':
            return ['return;']
        raise Unsupported(k)

    def emit_function(self, entry, end, labels, tables, hle, aliases=None):
        out = [f'void {fname(entry)}(Cpu *c) {{']
        if entry in hle:
            out.append(f'    {sanitize(hle[entry])}(c);')
            out.append('}')
            return out
        out.append(f'    FN_ENTER({hx(entry)});')
        if aliases:  # entered through an alias stub: go to its label
            cases = ' '.join(f'case {hx(p)}: goto L_{p:x};' for p in aliases)
            out.append(f'    if (__builtin_expect(c->alias_pc != 0, 0)) {{ uint64_t t_ = c->alias_pc; c->alias_pc = 0; '
                       f'switch (t_) {{ {cases} default: break; }} }}')
        batch = []
        refs = set()

        def flush():
            if batch:
                out.append('    ' + emit_asm_batch(batch))
                batch.clear()

        def jump(t):
            if entry <= t < end:
                return f'goto L_{t:x};'
            if not self.in_text(t):
                return f'c->pc = {hx(t)}; __attribute__((musttail)) return rt_dispatch(c);'
            refs.add(t)
            return f'__attribute__((musttail)) return {fname(t)}(c);'

        # Functions with jump tables also get the all-instruction fallback: a table whose emulation
        # missed some cases then still reaches the right instruction instead of aborting. Only small functions get
        # it for tables that resolved: those enumerate every index (a miss still reports "no function at" through
        # rt_dispatch), and every instruction a label in a big function makes clang crawl (minutes for a function of
        # a few thousand instructions in PLA) or run out of memory (30k+). A big function that needs it (an
        # unresolved table) is compiled without optimization instead.
        all_labels = 'all' in tables.values() or (bool(tables) and (end - entry) // 4 <= ALL_LABELS_MAX)
        if all_labels:
            labels = set(range(entry, end, 4))
            if (end - entry) // 4 > OPTNONE_MIN:
                out[0] = '__attribute__((optnone, noinline)) ' + out[0]
                self.stats['optnone'] += 1
        rando_here = [a for a in range(entry, end, 4) if a - self.base in RANDO_CODE]
        if rando_here:
            labels = set(labels)
            for a in rando_here:
                for w in RANDO_CODE[a - self.base]:
                    v = self.lifter.lift(w, a)
                    if v.kind in ('b', 'bcond') and isinstance(v.target, int) and entry <= v.target < end:
                        labels.add(v.target)
        last = None
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if a in labels:
                flush()
                out.append(f'L_{a:x}:;')
            if a - self.base in RANDO_CODE:  # randomizer patch site: the active seed picks the instruction
                flush()
                out.append(f'    {{ extern uint32_t rando_word(uint64_t); uint32_t rw = rando_word({hx(a)});')
                first = True
                for w in RANDO_CODE[a - self.base]:
                    v = self.lifter.lift(w, a)
                    out.append(f'    {"if" if first else "else if"} (rw == {hx(w)}) {{ ' + ' '.join(self.render_single(v, a, jump, refs)) + ' }')
                    first = False
                out.append('    else { ' + ' '.join(self.render_single(ins, a, jump, refs)) + ' } }')
                last = 'op'
                continue
            if a in PATCHES:  # hand-written replacement for one instruction (tools/patches.py)
                flush()
                out.append('    ' + PATCHES[a](self, a))
                last = 'op'
                continue
            k = ins.kind
            if k == 'asm':
                if len(batch) >= 24:
                    flush()
                batch.append(ins.asm)
                try:
                    emit_asm_batch(batch)
                except Unsupported:
                    batch.pop()
                    flush()
                    batch.append(ins.asm)
                last = k
                continue
            flush()
            if k == 'op' or k == 'trap':
                out.append('    ' + ins.code)
            elif k == 'b':
                out.append('    ' + jump(ins.target))
            elif k == 'bcond':
                out.append(f'    if ({ins.cond}) {{ {jump(ins.target)} }}')
            elif k == 'bl':
                t = ins.target
                if t in self.setjmp_targets:
                    # setjmp: a host setjmp point for the guest's return address and sp. The guest longjmp restores
                    # the guest registers and branches to that return address, and rt_dispatch (which finds no
                    # function there) longjmps back here; the call is skipped and the code goes on with x0 = value.
                    refs.add(t)
                    self.stats['setjmp_sites'] += 1
                    out.append(f'    c->x[30] = {hx(a + 4)}; if (!__builtin_setjmp(rt_setjmp_point(c, {hx(a + 4)}))) '
                               f'{fname(t)}(c);')
                elif self.in_text(t):
                    refs.add(t)
                    out.append(f'    c->x[30] = {hx(a + 4)}; {fname(t)}(c);')
                else:
                    out.append(f'    c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);')
            elif k == 'blr':
                out.append(f'    c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);')
            elif k == 'br':
                jt = tables.get(a)
                if jt == 'all':
                    out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                    continue
                if jt:
                    cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in jt)
                    out.append(f'    switch ({ins.target}) {{ {cases} default: break; }}')
                    out.append(f'    c->pc = {ins.target}; goto L_dispatch;' if all_labels else
                               f'    c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);')
                    continue
                out.append(f'    c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);')
            elif k == 'ret':
                out.append('    return;')
            last = k
        flush()
        if last not in ('b', 'br', 'ret', 'trap'):
            out.append('    ' + jump(end) if end < self.hi else f'    rt_udf(c, {hx(end)});')
        if all_labels:
            out.append('L_dispatch:')
            cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in range(entry, end, 4))
            out.append(f'    switch (c->pc) {{ {cases} default: break; }}')
            out.append('    __attribute__((musttail)) return rt_dispatch(c);')
        out.append('}')
        self._refs = refs
        return out


from patches import BUILD_ADDRS, EEVEE_100_BUILD, PATCHES

import json as _json
import os as _os
_rv = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'rando', 'patch_variants.json')
RANDO_CODE = {}
if _os.path.exists(_rv):
    for _a, _ws in _json.load(open(_rv))['code'].items():
        RANDO_CODE[int(_a, 16)] = [int(_w, 16) for _w in _ws]


def write_if_changed(path, text):
    """Leaves unchanged chunks untouched so make only recompiles what a patch affected."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, 'w') as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('outdir')
    ap.add_argument('--hle')
    ap.add_argument('--chunk', type=int, default=1500)  # functions per file at most
    ap.add_argument('--chunk-bytes', type=int, default=1500000)  # and about this much C (even compile times)
    ap.add_argument('--big-fn', type=int, default=100000)  # bytes of C from which a function gets its own file
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--trace-stores', action='store_true')  # research build (letsgo-recomp's docs/DEPURACAO.md, LARECOMP_TRACE_STORES)
    ap.add_argument('--simd', choices=('asm', 'c'),
                    default='asm' if platform.machine().lower() in ('arm64', 'aarch64') else 'c')
    args = ap.parse_args()

    t0 = time.time()
    m = Module(args.name, args.nso, args.base, args.simd, args.trace_stores)
    bid = m.nso.build_id[:16]
    if args.name == 'main' and bid != EEVEE_100_BUILD:
        # The instruction patches are written for Let's Go Eevee 1.0.0's main: moved to the same instructions of a
        # build with an address table (tools/patches.py), left out of any other (another update has other code there).
        moved = {BUILD_ADDRS[bid][a]: fn for a, fn in PATCHES.items()} if bid in BUILD_ADDRS else {}
        print(f'{args.name}: build {bid}, ' + (f'{len(moved)} instruction patches moved' if moved else 'no instruction patches'))
        PATCHES.clear()
        PATCHES.update(moved)
        RANDO_CODE.clear()
    starts = m.boundaries()
    hle = {}
    if args.hle and os.path.exists(args.hle):
        want = {l.strip() for l in open(args.hle) if l.strip() and not l.startswith('#')}
        for name, info, shndx, value, size in m.nso.syms:
            if shndx and name in want:
                hle[args.base + value] = name

    # Partition text into functions; any branch/call target outside its function becomes a new
    # boundary (splitting the containing function) until a fixpoint is reached.
    start_set = set(starts)
    for i, w in enumerate(m.words):
        if (w & 0xFC000000) == 0x94000000:
            t = m.lo + i * 4 + sext26(w) * 4
            if m.in_text(t):
                start_set.add(t)
    # Code addresses materialized in registers (callbacks, vtables built at runtime): ADR, or
    # ADRP followed shortly by ADD on the same register.
    adrp_regs = {}
    for i, w in enumerate(m.words):
        pc = m.lo + i * 4
        if (w & 0x9F000000) == 0x10000000:  # ADR
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            if m.in_text(pc + imm):
                (m.interior_ptrs if m.inside(pc + imm) else start_set).add(pc + imm)
        elif (w & 0x9F000000) == 0x90000000:  # ADRP
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            adrp_regs[w & 31] = ((pc & ~0xFFF) + (imm << 12), i)
        elif (w & 0xFFC00000) == 0x91000000:  # ADD (imm, 64-bit, no shift)
            rn, rd = (w >> 5) & 31, w & 31
            if rn in adrp_regs and i - adrp_regs[rn][1] <= 8:
                t = adrp_regs[rn][0] + ((w >> 10) & 0xFFF)
                if m.in_text(t):
                    (m.interior_ptrs if m.inside(t) else start_set).add(t)
    scanned = {}
    rounds = 0
    while True:
        rounds += 1
        order = sorted(start_set)
        new = set()
        for i, a in enumerate(order):
            end = order[i + 1] if i + 1 < len(order) else m.hi
            key = (a, end)
            if key not in scanned:
                scanned[key] = m.scan(a, end)
            new |= scanned[key][1] - start_set
        if not new:
            break
        start_set |= new
    entries = {}
    for i, a in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else m.hi
        labels, ext, tables = scanned[(a, end)]
        entries[a] = (end, labels, tables)
    # Code pointers inside a function (a label table of computed gotos, or several entry points sharing a tail: PLA
    # calls tiny thunks through a pointer table) don't split it; each gets an alias stub f_<addr> that enters the
    # containing function at that label (c->alias_pc), so both a computed goto and a call through it work.
    aliases = {}  # containing function -> its interior entry points
    alias_of = {}
    keys = sorted(entries)
    for p in sorted(m.interior_ptrs):
        if p in entries:
            continue
        i = bisect.bisect_right(keys, p) - 1
        if i < 0 or not (keys[i] < p < entries[keys[i]][0]) or keys[i] in hle:
            continue
        aliases.setdefault(keys[i], []).append(p)
        alias_of[p] = keys[i]
    print(f'{args.name}: {len(starts)} boundaries, {len(entries)} functions, {rounds} rounds, {dict(m.stats)}, '
          f'{len(alias_of)} interior entry points (alias stubs), {time.time() - t0:.0f}s', file=sys.stderr)

    os.makedirs(args.outdir, exist_ok=True)
    order = sorted(entries)
    if args.limit:
        order = order[:args.limit]
    nchunks = 0
    ci = 0
    pending = None  # (address, lines, refs) of a big function that starts the next file
    while ci < len(order) or pending:
        body, refs, size, fns = [], set(), 0, []
        while (pending or ci < len(order)) and len(fns) < args.chunk and size < args.chunk_bytes:
            if pending:
                a, lines, r = pending
                pending = None
            else:
                a = order[ci]
                ci += 1
                end, labels, tables = entries[a]
                lines = m.emit_function(a, end, labels | set(aliases.get(a, ())), tables, hle, aliases.get(a))
                for p in aliases.get(a, ()):
                    lines.append(f'void {fname(p)}(Cpu *c) {{ c->alias_pc = {hx(p)}; {fname(a)}(c); }}')
                r = getattr(m, '_refs', set())
                m._refs = set()
            n = sum(len(l) + 1 for l in lines)
            if n > args.big_fn and fns:  # (a big function gets a file of its own: they compile in parallel)
                pending = (a, lines, r)
                break
            body += lines
            size += n
            fns.append(a)
            fns += aliases.get(a, [])
            refs |= r
            if n > args.big_fn:
                break
        path = os.path.join(args.outdir, f'{args.name}_{nchunks:03d}.c')
        text = ['#include "cpu.h"\n' + ('#include "simd.h"\n' if m.lifter.sc else '')]
        text += [f'void {fname(r)}(Cpu *c);\n' for r in sorted(refs)]
        text += [f'void {sanitize(hle[a])}(Cpu *c);\n' for a in sorted(set(hle) & set(fns))]
        text += ['\n'.join(body), '\n']
        write_if_changed(path, ''.join(text))
        nchunks += 1
        # (the start screen's setup reads these for its progress bar: runtime/gamebuild.c)
        print(f'{args.name}: progress {ci}/{len(order)} files {nchunks}', file=sys.stderr, flush=True)
    for fn in os.listdir(args.outdir):  # an earlier run's extra chunks
        mm = re.fullmatch(re.escape(args.name) + r'_(\d+)\.c', fn)  # (PLA's main: over 1000 chunks)
        if mm and int(mm.group(1)) >= nchunks:
            os.remove(os.path.join(args.outdir, fn))
    order = sorted(set(order) | {p for p, a in alias_of.items() if a in set(order)})
    import io
    with io.StringIO() as f:
        f.write('#include "cpu.h"\n')
        for a in order:
            f.write(f'void {fname(a)}(Cpu *c);\n')
        f.write(f'const uint64_t {args.name}_fn_addrs[] = {{\n')
        f.write(',\n'.join(hx(a) for a in order))
        f.write('};\n')
        f.write(f'const GuestFn {args.name}_fn_ptrs[] = {{\n')
        f.write(',\n'.join(fname(a) for a in order))
        f.write('};\n')
        f.write(f'const uint64_t {args.name}_fn_count = {len(order)};\n')
        write_if_changed(os.path.join(args.outdir, f'{args.name}_table.c'), f.getvalue())
    print(f'{args.name}: wrote {nchunks} chunks, {time.time() - t0:.0f}s total', file=sys.stderr)


if __name__ == '__main__':
    main()
