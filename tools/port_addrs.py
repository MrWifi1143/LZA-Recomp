"""Addresses of one build of a game found in another (porting the runtime to a game update).

    port_addrs.py <old main> <new main> [--base 0x7000000000] [--out table.json] [--rewrite] <source files or address list...>

Every main address in the source files - 0x700xxxxxxx constants and f_700xxxxxxx function names, comments included -
(or the addresses in a text file, one a line) is looked for in the new build:
  code (in .text): the instructions from the address on, with what moves between builds masked (branch targets, the
      pages of ADRP and the offsets added to them, literal loads), searched for in the new build's text; the match
      grows until it is unique (or runs out); the delta between the builds is reported too
  data (.rodata / .data): the bytes there, searched for as they are; a run of words that look like code offsets
      (vtables, tables of functions: RELR keeps the addend in place) is ported word by word and the ported run looked
      for instead; else through the code that addresses it (ADRP + ADD / LDR, each ported as code)
Prints a table: old, new, how (code/N words, data, vtable, refs), confidence (unique / ambiguous / NOT FOUND), the files
that use it. --out writes the same as JSON.

--rewrite: each unique match written over the old address in the source files, in place (case and the 0x / f_ prefix
kept); the others are left as they are and listed. Only for files whose addresses are all the game's: the runtime's
Legends Z-A files (runtime/hle/za_*.c, warp.c). Files shared with the other games (menu_mouse.c, fps60.c,
oodle_server.c: tables by build id, Arceus / Let's Go addresses next to Z-A ones) are done by hand - an address of
another game still finds a "unique" match by chance. A match is the same instructions, not proof: an address
the update rewrote (a function moved or changed) needs a look at its callers (testtools callers.py / xref.py).

Legends Z-A 1.x -> 2.0.0 (2026-10-25) was ported this way: code moved by -17824..+2432 bytes (mostly +1568), rodata
and data by -20480. Four addresses needed a hand: a mid-function load (0x7000ededa0 -> 0x7000edf3c0), two script
natives (by their entries in the natives table, main+0x3BEF6F0 -> 0x3BEA6F0) and the story's phase advance, which
moved (0x7002ace9cc -> 0x7000adb384, found by its calls to the phase getter and the work setter).
"""
import json
import re
import struct
import sys

import nso

BASE = 0x7000000000
# an address in source: 0x7001234560(ull), f_7001234560 (a generated function's name)
ADDR_RE = re.compile(r'(?<![0-9a-fA-Fx])(?:0x|f_)(700[0-9a-fA-F]{7})(?![0-9a-fA-F])')


def mask_words(img, start, n):
    """n instruction words from start, each with the bits that move between builds cleared."""
    out = []
    adrp_regs = {}  # reg -> words since its ADRP
    for i in range(n):
        o = start + 4 * i
        if o + 4 > len(img):
            break
        w = struct.unpack_from('<I', img, o)[0]
        top = w >> 26
        m = w
        rd = w & 31
        rn = (w >> 5) & 31
        if top in (0b000101, 0b100101):  # B / BL
            m = w & 0xFC000000
        elif (w & 0xFF000010) == 0x54000000:  # B.cond
            m = w & 0xFF00001F
        elif (w & 0x7E000000) == 0x34000000:  # CBZ / CBNZ
            m = w & 0xFF00001F
        elif (w & 0x7E000000) == 0x36000000:  # TBZ / TBNZ
            m = w & 0xFFF8001F
        elif (w & 0x9F000000) == 0x90000000:  # ADRP
            m = w & 0x9F00001F
            adrp_regs[rd] = 0
        elif (w & 0x9F000000) == 0x10000000:  # ADR
            m = w & 0x9F00001F
        elif (w & 0x3B000000) == 0x18000000:  # LDR (literal), LDRSW literal, PRFM literal
            m = w & 0xFF00001F
        elif (w & 0x7F800000) == 0x11000000 and rn in adrp_regs:  # ADD imm to an ADRP page
            m = w & 0xFFC003FF
        elif (w & 0x3B000000) == 0x39000000 and rn in adrp_regs:  # LDR / STR unsigned offset from an ADRP page
            m = w & 0xFFC003FF
        # registers written by other instructions stop being pages (a rough rule: any Rd write)
        if not ((w & 0x9F000000) == 0x90000000):
            if (w & 0x3B000000) == 0x39000000 and (w >> 22) & 3 in (0, 2):  # a store: no Rd write
                pass
            elif rd in adrp_regs and (w & 0x3B000000) != 0x39000000:
                del adrp_regs[rd]
        for r in list(adrp_regs):
            adrp_regs[r] += 1
            if adrp_regs[r] > 6:
                del adrp_regs[r]
        out.append(m)
    return out


class Build:
    def __init__(self, path):
        self.n = nso.Nso(path)
        self.img = bytes(self.n.image)
        self.text = self.n.text
        self.rodata = self.n.rodata
        self.data = self.n.data
        t0, tn = self.text
        self.masked = None
        self._t0, self._tn = t0, tn

    def masked_text(self):
        if self.masked is None:
            t0, tn = self.text
            words = mask_words(self.img, t0, tn // 4)
            self.masked = struct.pack('<%dI' % len(words), *words)
        return self.masked

    def seg_of(self, off):
        for name, (o, n) in (('text', self.text), ('rodata', self.rodata), ('data', self.data)):
            if o <= off < o + n:
                return name
        if self.data[0] + self.data[1] <= off < self.n.size:
            return 'bss'
        return None

    def find_masked(self, pattern_words):
        """Positions (image offsets) in .text whose masked words start with pattern_words."""
        pat = struct.pack('<%dI' % len(pattern_words), *pattern_words)
        hay = self.masked_text()
        hits = []
        i = hay.find(pat)
        while i >= 0:
            if i % 4 == 0:
                hits.append(self.text[0] + i)
                if len(hits) > 50:
                    break
            i = hay.find(pat, i + 1)
        return hits

    def find_bytes(self, b, seg=None):
        lo, n = (0, len(self.img)) if seg is None else getattr(self, seg)
        hay = self.img
        hits = []
        i = hay.find(b, lo)
        while i >= 0 and i < lo + n:
            hits.append(i)
            if len(hits) > 50:
                break
            i = hay.find(b, i + 1)
        return hits


def port_code(old, new, off, min_words=6, max_words=96):
    """A code address: (new offset or None, words matched, candidates). A unique match is checked with twice the
    words (a short run can match by chance): one that fails the check counts as 0 candidates."""
    n = min_words
    best = None
    while n <= max_words:
        pat = mask_words(old.img, off, n)
        if len(pat) < n:
            break
        hits = new.find_masked(pat)
        if len(hits) == 1:
            longer = mask_words(old.img, off, min(n * 2, 48) if n < 48 else n + 16)
            if len(longer) > n and mask_words(new.img, hits[0], len(longer)) != longer:
                # (the code after the match differs: a real port keeps going the same for a while)
                same = sum(1 for a, b in zip(longer, mask_words(new.img, hits[0], len(longer))) if a == b)
                if same < len(longer) * 0.85:
                    return None, n, 0
            return hits[0], n, 1
        if not hits:
            return (best[0] if best and best[2] == 1 else None), n, len(hits) if not best else best[2]
        best = (hits[0], n, len(hits))
        n = n * 2 if n < 24 else n + 24
    return (best[0] if best else None), (best[1] if best else 0), (best[2] if best else 0)


def looks_like_code_offsets(old, words):
    t0, tn = old.text
    return sum(1 for w in words if t0 <= w < t0 + tn and w % 4 == 0) >= max(2, len(words) * 2 // 3)


def port_data(old, new, off, ported_code):
    """A data address: tries the bytes as they are, then a run of code offsets ported one by one."""
    seg = old.seg_of(off)
    for n in (32, 16, 8):
        b = old.img[off:off + n]
        hits = new.find_bytes(b, seg if seg in ('rodata', 'data') else None)
        if len(hits) == 1:
            return hits[0], 'data/%d bytes' % n, 1
    # a vtable / function table: 4 words of code offsets around it
    words = list(struct.unpack_from('<4Q', old.img, off)) if off + 32 <= len(old.img) else []
    if words and looks_like_code_offsets(old, words):
        new_words = []
        for w in words:
            if old.text[0] <= w < old.text[0] + old.text[1]:
                nw = ported_code.get(w)
                if nw is None:
                    nw, _, k = port_code(old, new, w)
                    if k != 1:
                        nw = None
                    ported_code[w] = nw
                if nw is None:
                    break
                new_words.append(nw)
            else:
                new_words.append(w)
        if len(new_words) >= 2:
            b = struct.pack('<%dQ' % len(new_words), *new_words)
            hits = new.find_bytes(b, 'data')
            if len(hits) == 1:
                return hits[0], 'vtable/%d entries' % len(new_words), 1
            if hits:
                return hits[0], 'vtable', len(hits)
    hits = new.find_bytes(old.img[off:off + 8], seg if seg in ('rodata', 'data') else None)
    return (hits[0] if hits else None), 'data/8 bytes', len(hits)


def adrp_refs(b, target, limit=8):
    """Instruction offsets in b's text that compute `target` as ADRP + ADD / LDR / STR (the ADD's / load's offset)."""
    img, (t0, tn) = b.img, b.text
    page = target & ~0xFFF
    refs = []
    for o in range(t0, t0 + tn - 4, 4):
        w = struct.unpack_from('<I', img, o)[0]
        if (w & 0x9F000000) != 0x90000000:
            continue
        immlo, immhi = (w >> 29) & 3, (w >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & (1 << 20):
            imm -= 1 << 21
        if (o & ~0xFFF) + (imm << 12) != page:
            continue
        rd = w & 31
        for k in range(1, 6):
            w2 = struct.unpack_from('<I', img, o + 4 * k)[0]
            if (w2 & 0x7F800000) == 0x11000000 and (w2 >> 5) & 31 == rd:  # ADD imm
                if ((w2 >> 10) & 0xFFF) == (target & 0xFFF):
                    refs.append(o + 4 * k)
                break
            if (w2 & 0x3B000000) == 0x39000000 and (w2 >> 5) & 31 == rd:  # LDR / STR unsigned offset
                size = w2 >> 30
                if (((w2 >> 10) & 0xFFF) << size) == (target & 0xFFF):
                    refs.append(o + 4 * k)
                break
            if (w2 & 31) == rd and (w2 & 0x9F000000) != 0x90000000:
                break
        if len(refs) >= limit:
            break
    return refs


def port_by_refs(old, new, off):
    """A data address through the code that addresses it: each old ADRP + ADD / LDR ported as code, the pair read in
    the new build. (new offset or None, how, candidates: how many references agree)."""
    votes = {}
    for ref in adrp_refs(old, off):
        # the ADRP is one of the 5 words before ref
        for back in range(1, 6):
            w = struct.unpack_from('<I', old.img, ref - 4 * back)[0]
            if (w & 0x9F000000) == 0x90000000 and (w & 31) == (struct.unpack_from('<I', old.img, ref)[0] >> 5) & 31:
                start = ref - 4 * back
                break
        else:
            continue
        noff, n, k = port_code(old, new, start, min_words=8)
        if k != 1:
            continue
        wa = struct.unpack_from('<I', new.img, noff)[0]
        immlo, immhi = (wa >> 29) & 3, (wa >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & (1 << 20):
            imm -= 1 << 21
        page = (noff & ~0xFFF) + (imm << 12)
        w2 = struct.unpack_from('<I', new.img, noff + 4 * back)[0]
        if (w2 & 0x7F800000) == 0x11000000:
            lo = (w2 >> 10) & 0xFFF
        elif (w2 & 0x3B000000) == 0x39000000:
            lo = ((w2 >> 10) & 0xFFF) << (w2 >> 30)
        else:
            continue
        votes[page + lo] = votes.get(page + lo, 0) + 1
    if not votes:
        return None, 'refs', 0
    best = max(votes, key=votes.get)
    return best, 'refs x%d' % votes[best], 1 if len(votes) == 1 else len(votes)


def main():
    args = sys.argv[1:]
    base = BASE
    out = None
    rewrite = False
    files = []
    i = 0
    while i < len(args):
        if args[i] == '--base':
            base = int(args[i + 1], 0)
            i += 2
        elif args[i] == '--out':
            out = args[i + 1]
            i += 2
        elif args[i] == '--rewrite':
            rewrite = True
            i += 1
        else:
            files.append(args[i])
            i += 1
    old = Build(files[0])
    new = Build(files[1])
    print('old build %s, new build %s' % (old.n.build_id[:16], new.n.build_id[:16]), file=sys.stderr)
    uses = {}
    for f in files[2:]:
        text = open(f, errors='replace').read()
        for m in re.finditer(ADDR_RE, text):
            a = int(m.group(1), 16)
            line = text.count('\n', 0, m.start()) + 1
            uses.setdefault(a, []).append('%s:%d' % (f.split('/')[-1], line))
        for m in re.finditer(r'^\s*(?:0x)?(70[0-9a-fA-F]{8})\s*$', text, re.M):  # (a list of addresses)
            uses.setdefault(int(m.group(1), 16), []).append(f.split('/')[-1])
    ported_code = {}
    rows = []
    for a in sorted(uses):
        off = a - base
        if off < 0 or off >= old.n.size or off == 0:
            continue
        seg = old.seg_of(off)
        if seg == 'text':
            noff, n, k = port_code(old, new, off)
            ported_code[off] = noff if k == 1 else None
            how = 'code/%d words' % n
            if k != 1:  # (a table kept in .text: the bytes as they are, else through the code that addresses it)
                noff, how, k = port_data(old, new, off, ported_code)
                if k != 1:
                    noff, how, k = port_by_refs(old, new, off)
        elif seg in ('rodata', 'data', 'bss'):
            noff, how, k = port_data(old, new, off, ported_code)
            if k != 1:
                noff, how, k = port_by_refs(old, new, off)
        else:
            noff, how, k = None, 'outside the image', 0
        conf = 'unique' if k == 1 else ('ambiguous x%d' % k if k > 1 else 'NOT FOUND')
        rows.append({'old': a, 'new': base + noff if noff is not None else None, 'seg': seg, 'how': how, 'conf': conf,
                     'delta': (noff - off) if noff is not None else None, 'uses': sorted(set(uses[a]))})
    for r in rows:
        print('%x -> %-12s %-6s %-18s %-14s %+8s  %s' % (r['old'], ('%x' % r['new']) if r['new'] else '-', r['seg'] or '?',
                                                      r['how'], r['conf'], ('%d' % r['delta']) if r['delta'] is not None else '',
                                                      ' '.join(r['uses'][:3])))
    n_ok = sum(1 for r in rows if r['conf'] == 'unique')
    print('%d addresses: %d unique, %d ambiguous, %d not found' % (len(rows), n_ok, sum(1 for r in rows if r['conf'].startswith('amb')),
                                                                 sum(1 for r in rows if r['new'] is None)), file=sys.stderr)
    if rewrite:  # (the unique matches written over the old addresses, in place)
        new_of = {r['old']: r['new'] for r in rows if r['conf'] == 'unique'}
        for f in files[2:]:
            text = open(f).read()
            n = [0]

            def sub(m):
                a = int(m.group(1), 16)
                if a not in new_of:
                    return m.group(0)
                s = '%x' % new_of[a]
                if m.group(1) != m.group(1).lower():
                    s = s.upper()
                n[0] += new_of[a] != a
                return m.group(0)[:m.start(1) - m.start(0)] + s
            new_text = ADDR_RE.sub(sub, text)
            if new_text != text:
                open(f, 'w').write(new_text)
            print('%s: %d addresses rewritten' % (f, n[0]), file=sys.stderr)
        for r in rows:
            if r['conf'] != 'unique':
                print('left as it was (%s): %x  %s' % (r['conf'], r['old'], ' '.join(r['uses'])), file=sys.stderr)
    if out:
        json.dump({'old_build': old.n.build_id[:16], 'new_build': new.n.build_id[:16], 'map': rows}, open(out, 'w'), indent=1)


if __name__ == '__main__':
    main()
