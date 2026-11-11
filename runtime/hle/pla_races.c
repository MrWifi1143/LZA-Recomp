// Arceus: races between the game's threads that its code doesn't guard against, made safe.
//
// Area change after Laventon's photo (2026-11-10, a new game ~24 min in; also the rarer crash walking into the
// player's room): f_70016e33ec, run by the area loader thread (called from f_70019d23xx, which stores the result at
// +0x810), returns in *x8 a shared reference - the object a global weak reference (0x7004279680) leads to, looked up
// by id w1 (the target's virtual +0x50), or, when that fails, the copy at this+0x88. With both missing (the global
// torn down by the main thread mid-transition and no +0x88 copy) the game reads +0x118 of a null pointer
// ("signal 11 accessing 0x118", host pc f_70016e33ec+42d). The Switch's few cores rarely let the two threads meet
// there; a PC runs them truly in parallel. Here that case answers an empty reference, which the caller checks
// (sp+0 == 0: nothing to release) - instead of the crash. Logged, so a report shows whether it happened.
#include <stdbool.h>
#include <stdint.h>
#include "../rt.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));

#define G64(a) (*(const uint64_t *)(uintptr_t)(a))

static bool guest_ptr(uint64_t a) { return a >= 0x7000000000ull && a < 0x8000000000ull && !(a & 7); }

// Whether the global path finds an object for id: the weak reference locked (its target at control block +0x58)
// and the target asked for id. The lookup is the game's own find; called once more by the function when it runs.
static bool global_finds(Cpu *c, uint32_t id) {
    uint64_t slot = G64(0x7004279680ull);  // (the function's static: a pointer to the weak reference's object pointer)
    if (!guest_ptr(slot)) return false;
    uint64_t obj = G64(slot);
    if (!guest_ptr(obj)) return false;
    uint64_t cb = G64(obj + 0xd0);
    if (!guest_ptr(cb)) return false;
    uint64_t target = G64(cb + 0x58);
    if (!guest_ptr(target)) return false;
    uint64_t vt = G64(target);
    if (!guest_ptr(vt) || !G64(vt + 0x50)) return false;
    Cpu save = *c;
    c->x[0] = target, c->x[1] = id;
    guest_call(c, G64(vt + 0x50));
    uint64_t found = c->x[0];
    *c = save;
    return found != 0;
}

static int shared_lookup_hook(Cpu *c, uint64_t addr) {
    uint64_t self = c->x[0], out = c->x[8];
    // its static not set up yet (the function builds it on first use), or the +0x88 fallback present: as the game
    uint64_t guard = G64(0x7004279678ull);
    if (!guest_ptr(self) || !guest_ptr(out) || !guest_ptr(guard) || !(*(const uint8_t *)(uintptr_t)guard & 1) || G64(self + 0x88))
        return 0;
    if (global_finds(c, (uint32_t)c->x[1])) return 0;
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
