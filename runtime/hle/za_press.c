// Legends Z-A above 30 fps: a button press is seen by whoever asks for it, not only on the one frame it happened.
//
// The game's controller update 0x70012cba00(Controller *c) turns the buttons held (+0x278) and those of the update
// before (+0x280) into held (+0x290), just pressed (+0x298: held & ~before), released (+0x2a0) and repeating (+0x2a8),
// once a frame. Its users ask through 0x70012cb010(c, &action) "was this action just pressed" and 0x70012cb270 "just
// pressed or repeating" (an action: a hashed name mapped to button masks). Most ask every frame; the battles' text
// (the ▼ of "You are challenged by ...!") asks on its timeline's steps, one frame in three at 144 fps: a press lasts
// one update, so a quick tap between two of its looks could go unseen. (The seconds it took no A at all were its
// timeline running at a quarter speed: za_fixes.c timeline_step.)
//
// Here a press stays "just pressed" for an action until that action is next asked about (for 150 ms at most): each
// action remembers the update it was last asked in, and is answered with the presses since the update before that.
// One asked every frame gets exactly the game's answer; several asking in the same update get the same one. At 30 fps
// nothing changes (the game as made). LARECOMP_ZA_PRESS_LATCH=0: off.
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);
extern bool pla60_enabled(void);  // (hle/fps60.c: above 30 fps)

#define ZA_PAD_UPDATE 0x70012cba00ull
#define ZA_PAD_PRESSED 0x70012cb010ull
#define ZA_PAD_PRESSED_OR_REPEAT 0x70012cb270ull
#define PRESSED_OFF 0x298
#define LATCH_NS 150000000ull

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// per controller: its updates counted, and each button's last press (that update, and when)
#define NCTL 4
static struct { uint64_t obj, seq, press_seq[64], press_ns[64]; } g_ctl[NCTL];
// per action asked about: the update it was last asked in, and the one before that
#define NACT 128
static struct { uint64_t obj, act, last, base; } g_act[NACT];
static int g_nact;

static int ctl_of(uint64_t obj, bool add) {
    for (int i = 0; i < NCTL; i++) if (g_ctl[i].obj == obj) return i;
    if (!add) return -1;
    for (int i = 0; i < NCTL; i++) if (!g_ctl[i].obj) { g_ctl[i].obj = obj; return i; }
    return -1;
}

static int pad_update(Cpu *c, uint64_t addr) {
    uint64_t obj = c->x[0];
    rt_call_original(c, addr);
    int k = ctl_of(obj, true);
    if (k < 0) return 1;
    uint64_t pressed = *(const uint64_t *)(uintptr_t)(obj + PRESSED_OFF), t = now_ns();
    g_ctl[k].seq++;
    for (int b = 0; b < 64; b++)
        if (pressed >> b & 1) g_ctl[k].press_seq[b] = g_ctl[k].seq, g_ctl[k].press_ns[b] = t;
    return 1;
}

static int pressed_query(Cpu *c, uint64_t addr) {
    uint64_t obj = c->x[0];
    int k = pla60_enabled() ? ctl_of(obj, false) : -1;
    if (k < 0 || !c->x[1]) return 0;  // (the game's own answer)
    uint64_t act = *(const uint64_t *)(uintptr_t)c->x[1], seq = g_ctl[k].seq;
    int i = 0;
    while (i < g_nact && !(g_act[i].obj == obj && g_act[i].act == act)) i++;
    if (i == g_nact) {
        if (g_nact == NACT) return 0;
        g_act[g_nact++] = (typeof(g_act[0])){obj, act, seq, seq ? seq - 1 : 0};
    } else if (g_act[i].last != seq) g_act[i].base = g_act[i].last, g_act[i].last = seq;
    uint64_t latched = 0, t = now_ns();
    for (int b = 0; b < 64; b++)
        if (g_ctl[k].press_seq[b] > g_act[i].base && g_ctl[k].press_seq[b] <= seq && t - g_ctl[k].press_ns[b] < LATCH_NS)
            latched |= 1ull << b;
    uint64_t *p = (uint64_t *)(uintptr_t)(obj + PRESSED_OFF), game = *p;
    *p = game | latched;  // (the getter reads it: the presses since this action was last asked)
    rt_call_original(c, addr);
    *p = game;
    static int log = -1;  // (LARECOMP_ZA_PRESS_LOG=1: each press a query got only through this)
    if (log < 0) log = getenv("LARECOMP_ZA_PRESS_LOG") != NULL;
    if (log && (latched & ~game))
        LOG("[press] action %016llx asked at update %llu (last before %llu): latched %#llx -> %d\n", (unsigned long long)act,
            (unsigned long long)seq, (unsigned long long)g_act[i].base, (unsigned long long)(latched & ~game), (int)(c->x[0] & 1));
    return 1;
}

void za_press_configure(void) {
    const char *e = getenv("LARECOMP_ZA_PRESS_LATCH");
    if (e && atoi(e) == 0) return;
    rt_register_fnhook_at(ZA_PAD_UPDATE, pad_update);
    rt_register_fnhook_at(ZA_PAD_PRESSED, pressed_query);
    rt_register_fnhook_at(ZA_PAD_PRESSED_OR_REPEAT, pressed_query);
}
