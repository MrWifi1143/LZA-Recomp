// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library. Everything else in it stays hidden.
#include "gamelib.h"

// (weak: a game without some module - Legends: Z-A has no subsdk - has no table for it; only present ones are listed)
#define MOD(n) extern const uint64_t n##_fn_addrs[] __attribute__((weak)), n##_fn_count __attribute__((weak)); \
               extern const GuestFn n##_fn_ptrs[] __attribute__((weak));
MOD(main) MOD(sdk) MOD(subsdk0) MOD(subsdk1)
#undef MOD
#define TAB(n) if (&n##_fn_count) t.mods[t.nmods++] = (GameLibModule){#n, n##_fn_addrs, n##_fn_ptrs, &n##_fn_count}

GAMELIB_EXPORT const GameLibTables *lasgame_tables(void) {
    static GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, 0, {{0}}};
    if (!t.nmods) { TAB(main); TAB(sdk); TAB(subsdk0); TAB(subsdk1); }
    return &t;
}
