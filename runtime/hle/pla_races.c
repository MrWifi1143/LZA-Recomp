// Arceus: races between the game's threads that its code doesn't guard against, made safe.
//
// Area change after Laventon's photo (2026-11-10, a new game ~24 min in; also the rarer crash walking into the
// player's room): f_70016e33ec, run by the area loader thread (called from f_70019d23xx, which stores the result at
// +0x810), returns in *x8 a shared reference - the object a global weak reference (0x7004279680) leads to, looked up
// by id w1 (the target's virtual +0x50) - and every path then reads +0x118 of the object behind this+0x88 (a weak
// reference, locked by f_70016e9e98). With that one expired (torn down by the main thread mid-transition) the game
// reads +0x118 of a null pointer
// ("signal 11 accessing 0x118", host pc f_70016e33ec+42d). The Switch's few cores rarely let the two threads meet
// there; a PC runs them truly in parallel. Here that case answers an empty reference, which the caller checks
// (sp+0 == 0: nothing to release) - instead of the crash. Logged, so a report shows whether it happened.
#include <stdbool.h>
#include <stdint.h>
#include "../rt.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));

#define G64(a) (*(const uint64_t *)(uintptr_t)(a))

static bool guest_ptr(uint64_t a) { return a >= 0x7000000000ull && a < 0x8000000000ull && !(a & 7); }

// Whether the +0x88 fallback still holds an object: it is a weak reference's control block (f_70016e9e98 locks it:
// use count at +0xc, the object at +0x18 - 0x50); expired (use count 0) or empty, the lock gives nothing. The first
// crash report had the control block set but expired - checking only for it being set let the crash through.
static bool fallback_alive(uint64_t self) {
    uint64_t cb = G64(self + 0x88);
    return guest_ptr(cb) && *(const uint32_t *)(uintptr_t)(cb + 0xc) != 0 && G64(cb + 0x18) != 0;
}

static int shared_lookup_hook(Cpu *c, uint64_t addr) {
    uint64_t self = c->x[0], out = c->x[8];
    // the +0x88 fallback alive: as the game (every path through the function reads it)
    if (!guest_ptr(self) || !guest_ptr(out) || fallback_alive(self)) return 0;
    *(uint64_t *)(uintptr_t)out = 0, *(uint64_t *)(uintptr_t)(out + 8) = 0;  // (an empty shared reference)
    static uint64_t n;
    LOG("[race] area loader: its shared object was already gone (id %u) - answered empty instead of crashing (%llu)\n",
        (unsigned)(uint32_t)c->x[1], (unsigned long long)++n);
    (void)addr;
    return 1;
}

__attribute__((constructor)) static void pla_races_init(void) {
    rt_register_fnhook_at(0x70016e33ecull, shared_lookup_hook);
}
