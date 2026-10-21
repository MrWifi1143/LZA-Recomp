"""Per-instruction replacements applied by gen.py (guest address -> function(gen, addr) returning C code).

Derived from LASRecomp (MIT, (c) 2026 Peiikai). The Link's Awakening patches were removed: their
addresses belong to that game's executable. Let's Go patches (widescreen etc.) go here later.
"""

PATCHES = {}


# 60 fps: the frame counters the capture and battle scenes' timelines run on (found with the store tracer, letsgo-recomp's docs/PESQUISA-60FPS.md).
# Each steps once per update; at 60 fps they step on every other one (rt_scene_tick, runtime/hle/fps60.c) so those scenes
# keep the game's 30 steps a second. Everywhere else rt_scene_tick() is 1 and the store is the game's own.
def _scene_counter(addr_expr, reg):
    def patch(gen, a):
        return ('{ extern int rt_scene_tick(void); uint64_t ea = %s; if (rt_scene_tick()) ST32(ea, (uint32_t)c->x[%d]); }' % (addr_expr, reg))
    return patch


PATCHES[0x7000b88340] = _scene_counter('c->x[19] + 0x58ull', 8)   # str w8, [x19, #0x58]  (the sequence object's step, f_7000b88310)
PATCHES[0x7000b6acc8] = _scene_counter('c->x[9] + c->x[11]', 12)  # str w12, [x9, x11]  (the per-track counters of a timeline)


# The capture ring's counter (+0x78 of the object in x19): counts up while two fields match, back to 0 otherwise. The
# result is in w8 (computed by the csinc before); on the update that doesn't tick, keep the old value (w9).
def _ring_counter(gen, a):
    return ('{ extern int rt_scene_tick(void); uint64_t ea = c->x[19] + 0x78ull; '
            'if (!rt_scene_tick() && (uint32_t)c->x[8] == (uint32_t)c->x[9] + 1) c->x[8] = (uint32_t)c->x[9]; '
            'ST32(ea, (uint32_t)c->x[8]); }')


PATCHES[0x70002d2a44] = _ring_counter


# The addresses above are Let's Go Eevee 1.0.0's main (build id below). The same instructions in the other builds with
# address tables (found with letsgo-recomp's tools/addrmap.py; runtime/fnhook.c has the runtime's hooks): gen.py moves the patches
# there, and generates any other build (an update) without them.
EEVEE_100_BUILD = '0a3f0d21e67dad80'
BUILD_ADDRS = {
    '1c375692de4f4b87': {0x7000b88340: 0x7000b88350, 0x7000b6acc8: 0x7000b6acd8, 0x70002d2a44: 0x70002d2a14},  # Pikachu 1.0.0
}


# Legends: Arceus 1.1.1 at more than 30 fps (runtime/hle/fps60.c pla60_*): the world steps by the frame time of its
# table, but some systems build the 30 fps frame time 1/30 s (0x3d088889) in code - mov w, #0x8889; movk w, #0x3d08,
# lsl #16 - and step by it on every update, so at 144 fps they run 4.8 times as fast. Each movk below gets the frame
# time the game runs at instead (rt_step_bits, 1/30 at 30 fps); the runtime turns each site on or off by its index
# (LARECOMP_STEP_SITES, research). Sites that convert frames to seconds and back keep the constant (not listed).
PLA_STEP_SITES = [
    0x70013b33a0, 0x700167ff10, 0x70016832c4, 0x70016832e8, 0x70016836d4, 0x70017177b8, 0x7001728058, 0x7001982934,
    0x7001983764, 0x700198e024, 0x700198ec70, 0x7001a0baf4, 0x7001a0bee8, 0x7001a0da24, 0x7001a0eea4, 0x7001a0f308,
    0x7001a0f49c, 0x7001a0f560, 0x7001a17b14, 0x7001a17f40, 0x7001a19108, 0x7001a196c8, 0x7001b48a4c, 0x7001b48a98,
    0x7001b49008, 0x7001b493f8, 0x7001c5d38c, 0x7001d1b7d0, 0x7001d5d688, 0x700257ac34, 0x700257c9f8, 0x700257ce3c,
    0x7002795234, 0x70029faf90, 0x7002aefb28, 0x7002af25f8, 0x7002f3e8c8, 0x7002f3ec18, 0x7002fe9660, 0x7003002a08,
    0x70030143a0, 0x7003014d28, 0x700301526c, 0x7003018b94, 0x700301e520,
]


def _step_site(i):
    def patch(gen, a):
        w = gen.words[(a - gen.base) // 4]
        assert w & 0x7fffffe0 == 0x72a7a100, hex(a)  # movk w/x<d>, #0x3d08, lsl #16 (after mov <d>, #0x8889)
        d = w & 31
        keep = '(c->x[%d] & 0xffffffff00000000ull) | ' % d if w >> 31 else ''  # (an x register: the upper half stays)
        return '{ extern uint32_t rt_step_bits(int); c->x[%d] = %s(uint64_t)rt_step_bits(%d); }' % (d, keep, i)
    return patch


# Frame counters that step once per update (integers): above 30 fps they step only on the updates where 30 fps time
# moved on a frame (rt_pla_tick, runtime/hle/fps60.c), so what counts on them keeps the game's 30 steps a second.
def _pla_tick_store(reg, base, off):
    def patch(gen, a):
        return ('{ extern int rt_pla_tick(int); if (rt_pla_tick(%d)) ST32(c->x[%d] + %#xull, (uint32_t)c->x[%d]); }' % (a & 0xffff, base, off, reg))
    return patch


# The camera's turn: the stick moves the target yaw and pitch by stick * a turn-rate parameter (f_700193c2c4, read by
# name hash) on every update, with no frame time in it - 4.8 times as fast at 144 fps. Each call returns that rate
# scaled to the frame time (rt_scale30_f32: frame time / (1/30), 1 at 30 fps).
def _scaled_rate_call(target):
    def patch(gen, a):
        return ('{ void f_%x(Cpu *c); extern uint32_t rt_scale30_f32(uint32_t); c->x[30] = %#xull; f_%x(c); '
                'c->v[0] = rt_scale30_f32((uint32_t)c->v[0]); }' % (target, a + 4, target))
    return patch


# The message printer (the state machine of f_7002b31600, x19 = the printer): each update it counts a wait counter
# (+0x9e) up and prints the next characters once it passes the text speed (+0x9d) - characters per update, 4.8 times
# as fast at 144 fps. On the updates where 30 fps time didn't move on a frame it waits (rt_pla_tick); the rest of the
# state machine (the A button, the window's fades) runs as before.
def _printer_wait(gen, a):
    return '{ extern int rt_pla_tick(int); if (COND_CS(c) || !rt_pla_tick(%d)) { goto L_7002b31e38; } }' % (a & 0xffff)


# The sky's scroll phases (the environment object: x19 + 0x604..0x614, each stepped once per update and wrapped to
# 0..1): steps sized for 30 fps, scaled to the frame time. fdiv s0, s0, s1 (+0x614's step) and fmul s2, s2, s11
# (the divisor of the +0x604..+0x610 steps).
_F32 = 'union { uint32_t u; float f; } a, b; '


def _sky_step_div(gen, a):
    return ('{ extern uint32_t rt_scale30_f32(uint32_t); %sa.u = (uint32_t)c->v[0]; b.u = (uint32_t)c->v[1]; '
            'a.f = a.f / b.f; c->v[0] = rt_scale30_f32(a.u); }' % _F32)


def _sky_step_divisor(gen, a):
    return ('{ extern uint32_t rt_unscale30_f32(uint32_t); %sa.u = (uint32_t)c->v[2]; b.u = (uint32_t)c->v[11]; '
            'a.f = a.f * b.f; c->v[2] = rt_unscale30_f32(a.u); }' % _F32)


# The cutscene (demo) player's clock: its frame counter (+0x7ec of the demo, in 60 Hz frames) gets 2 on every update
# - the whole cutscene 4.8 times as fast at 144 fps. add w9, w9, #2 adds the frames the frame time is worth instead
# (rt_pla_frames60: 2 at 30 fps, the fraction carried to the next update).
def _demo_frames(i):
    def patch(gen, a):
        assert gen.words[(a - gen.base) // 4] == 0x11000929, hex(a)  # add w9, w9, #2
        return ('{ extern uint32_t rt_pla_frames60(int); c->x[9] = (uint32_t)((uint32_t)c->x[9] + rt_pla_frames60(%d)); }' % i)
    return patch


# The effect manager's frame rate for the effect system (f_7002b8fe..: s8 = speed * frames): the frame time in whole
# 60 Hz frames - fcvtzs w8, d0 / scvtf s1, w8: 2 at 30 fps, 1 at 60, 0 at 120 and 144, where effects that fade in
# or emit over frames (the opening's golden light, the title logo) never show. The frames with their fraction instead
# (rt_pla_frames60_f: whole frames at 30 fps, as the game does). fcvtzs w8, d0 carries them as a float's bits in w8
# (d0 is reused by the next instruction), scvtf s1, w8 takes them back.
def _vfx_frames_cvt(gen, a):  # fcvtzs w8, d0: w8 = the frames as a float's bits (d0 is reused right after)
    assert gen.words[(a - gen.base) // 4] == 0x1e780008, hex(a)
    return '{ extern uint32_t rt_pla_frames60_f(uint64_t); c->x[8] = rt_pla_frames60_f((uint64_t)c->v[0]); }'


def _vfx_frames_use(gen, a):  # scvtf s1, w8: s1 = those bits
    assert gen.words[(a - gen.base) // 4] == 0x1e220101, hex(a)
    return 'c->v[1] = (uint32_t)c->x[8];'


PLA_PATCHES = {a: _step_site(i) for i, a in enumerate(PLA_STEP_SITES)}
PLA_PATCHES[0x7002b90040] = _vfx_frames_cvt
PLA_PATCHES[0x7002b90048] = _vfx_frames_use
for _i, _a in enumerate((0x7001458424, 0x700145e234, 0x700145e580)):
    PLA_PATCHES[_a] = _demo_frames(_i)
PLA_PATCHES[0x70029c1934] = _sky_step_div      # fdiv s0, s0, s1
PLA_PATCHES[0x70029c23cc] = _sky_step_divisor  # fmul s2, s2, s11
PLA_PATCHES[0x7002b31864] = _printer_wait  # b.hs 0x7002b31e38 (still waiting: return)
for _a in (0x70019358d0, 0x7001935c6c, 0x700193d26c):  # bl f_700193c2c4 (the camera's input turn, three modes)
    PLA_PATCHES[_a] = _scaled_rate_call(0x700193c2c4)
PLA_PATCHES[0x7002d407e8] = _pla_tick_store(9, 8, 0xf4)  # str w9, [x8, #0xf4] (f_7002d3f550: each listed object's frame count)

BUILD_PATCHES = {
    'aee8f150dda1b5a8': PLA_PATCHES,  # Legends: Arceus 1.1.1
}
