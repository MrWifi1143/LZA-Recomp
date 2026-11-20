// Legends Lua: game scripting from Lua 5.3 (third_party/lua), as a runtime plugin (runtime/plugin.h). Two layers:
//   - the mod API (docs/lua-api.md, written in api.lua on top of the game's runtime/modapi.h): characters of a mod's own
//     made by the game (npc.spawn, walk_to, remove), the player, events (tick, area_loaded, button, talk), timers,
//     console commands, notices on screen - the same in both games;
//   - the raw layer underneath, for research and for what the mod API doesn't have yet: hook any game function, read
//     and write game memory, call the game's functions, run something every frame.
// Built into <game folder>/plugins/ (make legendslua). Loaded when a mod that is on carries scripts (mods/<name>/lua/),
// or when switched on in the Mods page (for <game folder>/lua/*.lua).
//
// Scripts:
//   mods/<name>/lua/      each mod that is on: every *.lua there in name order (files in folders below it are for
//                         include("lib/x.lua")). Each mod has its own globals (a table that sees the shared ones).
//   <game folder>/lua/    *.lua in name order (research scripts; the shared globals).
// They run as the game starts (the runtime's own hooks set by then, none of the game's code run yet) and again when one
// of them changes (hot reload: everything they made - characters, hooks, handlers - is dropped and they run anew).
// A live console: <dir>/lua_run.lua (dir: LARECOMP_LIVE, else the game folder) is run once and removed; <dir>/lua_cmd
// lines run console commands (cmd.add); what they print goes to the log and to <dir>/lua_out.txt.
//
// Safety: a script's error never stops the game - it is logged ("[lua] <mod>: ..."), a handler that keeps failing is
// switched off with a notice on screen. A script that runs too long (an endless loop) is stopped (LUA_SLICE_MS), Lua's
// memory is capped (LUA_MEM_CAP). Off the hot path: the files are watched from a thread of the plugin's own; the game's
// render thread only runs what the scripts asked for each frame (measured: lua.stats(), LARECOMP_LUA_STATS=1).
//
// The raw layer (addresses as the running game has them, main at 0x7000000000):
//   log(...) / print(...)              -> the log ("[lua] ...")
//   hook(addr, fn)                     fn(c) when the game calls addr; return true to skip the game's function (set
//                                      c's x0 first). Several on one address run in order (one skipping ends it);
//                                      hook(addr, nil) drops them all.
//   on_frame(fn)                       fn(frame) every game frame (render thread: memory yes, calls no)
//   c:x(n) c:setx(n, v)                registers x0..x30 (integers, 64-bit)
//   c:s(n) c:sets(n, v) c:d(n) c:setd(n, v)   float / double in v0..v31; c:vf(n, lane) c:setvf(n, lane, v) a vector's float 0..3
//   c.lr c.sp c.addr                   return address, stack pointer, the hooked function
//   c:original()                       run the game's function now (its result in c's x0 / v0 after)
//   c:call(addr, x0, x1, ...)          call a game function from inside a hook; returns its x0 (registers put back)
//   c:bt([n])                          the caller chain (frame pointers): a table of return addresses
//   mem.u8 u16 u32 u64 i32 f32 f64(addr)   reads (nil when not readable)
//   mem.w8 w16 w32 w64 wf32 wf64(addr, v)  writes (false when not writable)
//   mem.str(addr[, max]) mem.bytes(addr, n) mem.valid(addr, n) mem.dump(addr, n)
//   mem.find(lo, hi, bytes[, max])     where bytes (a Lua string, string.pack) occur in [lo, hi): a table of addresses
//   game.title game.root game.frame()  program id (hex), game folder, frames presented
//   watch(addr[, len]) watch_reset()   hardware watchpoints (research, Linux): each writer logged once ("[hwwatch]";
//                                      LARECOMP_HWWATCH_RW=1: readers too), up to 4
// Hooks run on the game's own threads, one at a time (one lock): keep them short. A hook that calls into the game
// while another thread's hook waits can stall both - call sparingly.
#define _GNU_SOURCE
#include <dirent.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

#include "modapi.h"
#include "plugin.h"
#include "lua/lauxlib.h"
#include "lua/lua.h"
#include "lua/lualib.h"

#define LUA_SLICE_MS 1000         // (a script running longer than this at once is stopped - an endless loop)
#define LUA_LOAD_MS 5000          // (loading the scripts: more time)
#define LUA_MEM_CAP (256u << 20)  // (Lua's memory, all scripts together)

static const LarecompPluginApi *A;
static const LarecompModApi *M;  // (NULL: a game build without the mod API - the raw layer only)
static long long scan_all(int *nfiles);
static lua_State *L;
static pthread_mutex_t g_lock;
static char g_live[1024];

// The plugin is handed the game's mod API right after its init (runtime/plugin.c)
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void larecomp_plugin_modapi(const LarecompModApi *api) {
    if (!api || api->version < 1) return;
    M = api;
    int n = 0;
    scan_all(&n);
    if (n) M->enable();  // (scripts to run: the game's characters and the player followed from its start - before the first
}                        //  level loads, so its people are noted as they are made)

static uint64_t now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

// the calling thread's own CPU time (the cost measured: a busy PC's other work - preemption - left out); wall time where
// there's no such clock
static uint64_t cpu_ns(void) {
#ifdef _WIN32
    return now_ns();
#else
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) return now_ns();
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

// ---- guest registers (runtime/plugin.h: x at byte 0, sp at 256, v at 512)
static uint64_t *reg_x(struct Cpu *c) { return (uint64_t *)(void *)c; }
static uint64_t *reg_sp(struct Cpu *c) { return (uint64_t *)((char *)c + 256); }
static unsigned char *reg_v(struct Cpu *c, int n) { return (unsigned char *)c + 512 + 16 * n; }

// ---- safe memory access: a bad address gives nil, not a crash
static int mem_read(uint64_t a, void *out, size_t n) {
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, out, n, &got) && got == n;
#else
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
#endif
}
static int mem_write(uint64_t a, const void *in, size_t n) {
#ifdef _WIN32
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)a, in, n, &put) && put == n;
#else
    struct iovec l = {(void *)in, n}, r = {(void *)(uintptr_t)a, n};
    return process_vm_writev(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
#endif
}

// ---- time and memory guards. A count hook every LUA_STEPS instructions looks at the clock: past the deadline of the
// call running now, the script gets an error (it never runs on forever on the game's thread)
#define LUA_STEPS 20000
static uint64_t g_deadline;  // (0: none - the console's and the loader's own calls set theirs)
static void count_hook(lua_State *s, lua_Debug *ar) {
    (void)ar;
    if (g_deadline && now_ns() > g_deadline) {
        g_deadline = 0;  // (once: the error's own handlers may run)
        luaL_error(s, "the script ran too long at once (an endless loop?) and was stopped");
    }
}
static uint64_t deadline_set(unsigned ms) {  // (the one before is given back: calls nest - a hook's call into the game
    uint64_t was = g_deadline;               //  may reach another hook)
    uint64_t d = now_ns() + (uint64_t)ms * 1000000ull;
    if (!was || d < was) g_deadline = d;
    return was;
}
static size_t g_mem;
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (!ptr) osize = 0;
    if (!nsize) { free(ptr); g_mem -= osize; return NULL; }
    if (nsize > osize && g_mem + (nsize - osize) > LUA_MEM_CAP) return NULL;  // (Lua raises "not enough memory")
    void *p = realloc(ptr, nsize);
    if (p) g_mem += nsize - osize;
    return p;
}

// ---- the cost (lua.stats(); LARECOMP_LUA_STATS=1 logs it every minute): what the scripts take on the render thread
// each frame (tick, timers, events) and in hooks on the game's threads
static struct { uint64_t frames, frame_ns, frame_max, hooks, hook_ns, hook_max; } g_st, g_st_win;

static FILE *g_out;  // (the live console's output while its chunk runs)
static int l_log(lua_State *s) {
    char line[2048];
    int k = 0, n = lua_gettop(s);
    for (int i = 1; i <= n && k < (int)sizeof line - 2; i++) {
        const char *t = luaL_tolstring(s, i, NULL);
        k += snprintf(line + k, sizeof line - k, "%s%s", i > 1 ? "\t" : "", t);
        lua_pop(s, 1);
    }
    A->log("[lua] %s\n", line);
    if (g_out) fprintf(g_out, "%s\n", line), fflush(g_out);
    return 0;
}
// (a pcall message handler: the error with where it happened)
static int l_trace(lua_State *s) {
    const char *m = lua_tostring(s, 1);
    if (!m) m = luaL_tolstring(s, 1, NULL);
    luaL_traceback(s, s, m, 1);
    return 1;
}
static int pcall_traced(lua_State *s, int nargs, int nres) {
    int base = lua_gettop(s) - nargs;
    lua_pushcfunction(s, l_trace);
    lua_insert(s, base);
    int r = lua_pcall(s, nargs, nres, base);
    lua_remove(s, base);
    return r;
}

// ---- hooks: one C function for every address, the Lua function in the registry table "hooks"
#define MAXHOOK 512
static uint64_t g_hooked[MAXHOOK];
static int g_nhooked, g_errs[MAXHOOK];

static int hook_index(uint64_t addr) {
    for (int i = 0; i < g_nhooked; i++) if (g_hooked[i] == addr) return i;
    return -1;
}

typedef struct { struct Cpu *c; uint64_t addr; } CpuRef;
static CpuRef *check_cpu(lua_State *s) { return (CpuRef *)luaL_checkudata(s, 1, "legends.cpu"); }

static int dispatch(struct Cpu *c, uint64_t addr) {
    pthread_mutex_lock(&g_lock);
    int skip = 0;
    if (L) {
        uint64_t t0 = cpu_ns(), was = deadline_set(LUA_SLICE_MS);
        lua_getfield(L, LUA_REGISTRYINDEX, "hooks");
        lua_rawgeti(L, -1, (lua_Integer)addr);  // (a list: every script's function on this address, in order)
        if (lua_istable(L, -1)) {
            CpuRef *r = (CpuRef *)lua_newuserdata(L, sizeof *r);
            r->c = c, r->addr = addr;
            luaL_setmetatable(L, "legends.cpu");
            for (lua_Integer k = 1, n = (lua_Integer)lua_rawlen(L, -2); k <= n && !skip; k++) {
                lua_rawgeti(L, -2, k);
                lua_pushvalue(L, -2);
                if (pcall_traced(L, 1, 1)) {
                    int i = hook_index(addr);
                    if (i >= 0 && g_errs[i]++ < 3) A->log("[lua] hook %" PRIx64 ": %s\n", addr, lua_tostring(L, -1));
                } else skip = lua_toboolean(L, -1);
                lua_pop(L, 1);
            }
            r->c = NULL;  // (the registers belong to this call only)
            lua_pop(L, 3);
        } else lua_pop(L, 2);
        g_deadline = was;
        uint64_t dt = cpu_ns() - t0;
        g_st.hooks++, g_st.hook_ns += dt;
        if (dt > g_st.hook_max) g_st.hook_max = dt;
    }
    pthread_mutex_unlock(&g_lock);
    return skip;
}

static int l_hook(lua_State *s) {
    uint64_t addr = (uint64_t)luaL_checkinteger(s, 1);
    if (!lua_isnil(s, 2)) luaL_checktype(s, 2, LUA_TFUNCTION);
    lua_getfield(s, LUA_REGISTRYINDEX, "hooks");
    if (lua_isnil(s, 2)) {  // (nil: every function on it dropped)
        lua_pushnil(s);
        lua_rawseti(s, -2, (lua_Integer)addr);
        return 0;
    }
    if (lua_rawgeti(s, -1, (lua_Integer)addr) != LUA_TTABLE) {
        lua_pop(s, 1);
        lua_newtable(s);
        lua_pushvalue(s, -1);
        lua_rawseti(s, -3, (lua_Integer)addr);
    }
    lua_pushvalue(s, 2);
    lua_rawseti(s, -2, (lua_Integer)lua_rawlen(s, -2) + 1);
    if (!lua_isnil(s, 2) && hook_index(addr) < 0) {
        if (g_nhooked == MAXHOOK) return luaL_error(s, "too many hooked functions");
        g_errs[g_nhooked] = 0, g_hooked[g_nhooked++] = addr;
        A->register_fnhook(addr, dispatch);  // (kept registered for good: with no Lua function it does nothing)
    }
    return 0;
}

static int l_on_frame(lua_State *s) {
    luaL_checktype(s, 1, LUA_TFUNCTION);
    lua_getfield(s, LUA_REGISTRYINDEX, "frames");
    lua_pushvalue(s, 1);
    lua_rawseti(s, -2, (lua_Integer)lua_rawlen(s, -2) + 1);
    return 0;
}

// ---- the cpu object
static struct Cpu *live_cpu(lua_State *s) {
    CpuRef *r = check_cpu(s);
    if (!r->c) luaL_error(s, "these registers belong to a hook call that has ended");
    return r->c;
}
static int c_x(lua_State *s) { int n = (int)luaL_checkinteger(s, 2); luaL_argcheck(s, n >= 0 && n <= 30, 2, "x0..x30"); lua_pushinteger(s, (lua_Integer)reg_x(live_cpu(s))[n]); return 1; }
static int c_setx(lua_State *s) { int n = (int)luaL_checkinteger(s, 2); luaL_argcheck(s, n >= 0 && n <= 30, 2, "x0..x30"); reg_x(live_cpu(s))[n] = (uint64_t)luaL_checkinteger(s, 3); return 0; }
static int c_s(lua_State *s) { int n = (int)luaL_checkinteger(s, 2) & 31; float f; memcpy(&f, reg_v(live_cpu(s), n), 4); lua_pushnumber(s, f); return 1; }
static int c_sets(lua_State *s) { int n = (int)luaL_checkinteger(s, 2) & 31; float f = (float)luaL_checknumber(s, 3); unsigned char *v = reg_v(live_cpu(s), n); memset(v, 0, 16); memcpy(v, &f, 4); return 0; }
static int c_vf(lua_State *s) {
    int n = (int)luaL_checkinteger(s, 2) & 31, lane = (int)luaL_optinteger(s, 3, 0) & 3;
    float f;
    memcpy(&f, reg_v(live_cpu(s), n) + 4 * lane, 4);
    lua_pushnumber(s, f);
    return 1;
}
static int c_setvf(lua_State *s) {
    int n = (int)luaL_checkinteger(s, 2) & 31, lane = (int)luaL_checkinteger(s, 3) & 3;
    float f = (float)luaL_checknumber(s, 4);
    memcpy(reg_v(live_cpu(s), n) + 4 * lane, &f, 4);
    return 0;
}
static int c_d(lua_State *s) { int n = (int)luaL_checkinteger(s, 2) & 31; double d; memcpy(&d, reg_v(live_cpu(s), n), 8); lua_pushnumber(s, d); return 1; }
static int c_setd(lua_State *s) { int n = (int)luaL_checkinteger(s, 2) & 31; double d = luaL_checknumber(s, 3); unsigned char *v = reg_v(live_cpu(s), n); memset(v, 0, 16); memcpy(v, &d, 8); return 0; }
static int c_original(lua_State *s) { CpuRef *r = check_cpu(s); A->call_original(live_cpu(s), r->addr); return 0; }
static int c_call(lua_State *s) {
    struct Cpu *c = live_cpu(s);
    uint64_t addr = (uint64_t)luaL_checkinteger(s, 2), save[31];
    unsigned char vsave[8][16];
    memcpy(save, reg_x(c), sizeof save);
    for (int i = 0; i < 8; i++) memcpy(vsave[i], reg_v(c, i), 16);
    int n = lua_gettop(s) - 2;
    for (int i = 0; i < n && i < 8; i++) reg_x(c)[i] = (uint64_t)luaL_checkinteger(s, 3 + i);
    A->guest_call(c, addr);
    uint64_t r0 = reg_x(c)[0];
    float f0;
    memcpy(&f0, reg_v(c, 0), 4);
    memcpy(reg_x(c), save, sizeof save);
    for (int i = 0; i < 8; i++) memcpy(reg_v(c, i), vsave[i], 16);
    lua_pushinteger(s, (lua_Integer)r0);
    lua_pushnumber(s, f0);
    return 2;
}
static int c_bt(lua_State *s) {
    struct Cpu *c = live_cpu(s);
    int max = (int)luaL_optinteger(s, 2, 16);
    lua_newtable(s);
    uint64_t fp = reg_x(c)[29], frame[2];
    for (int i = 1; i <= max && fp && !(fp & 7) && mem_read(fp, frame, sizeof frame); i++) {
        lua_pushinteger(s, (lua_Integer)frame[1]);
        lua_rawseti(s, -2, i);
        fp = frame[0];
    }
    return 1;
}
static int c_index(lua_State *s) {
    const char *k = luaL_checkstring(s, 2);
    CpuRef *r = check_cpu(s);
    if (!strcmp(k, "addr")) { lua_pushinteger(s, (lua_Integer)r->addr); return 1; }
    if (!strcmp(k, "lr")) { lua_pushinteger(s, (lua_Integer)reg_x(live_cpu(s))[30]); return 1; }
    if (!strcmp(k, "sp")) { lua_pushinteger(s, (lua_Integer)*reg_sp(live_cpu(s))); return 1; }
    luaL_getmetafield(s, 1, "methods");
    lua_getfield(s, -1, k);
    return 1;
}

// ---- memory
#define RD(name, T, push) static int m_##name(lua_State *s) { T v; if (!mem_read((uint64_t)luaL_checkinteger(s, 1), &v, sizeof v)) { lua_pushnil(s); return 1; } push; return 1; }
RD(u8, uint8_t, lua_pushinteger(s, v))
RD(u16, uint16_t, lua_pushinteger(s, v))
RD(u32, uint32_t, lua_pushinteger(s, v))
RD(u64, uint64_t, lua_pushinteger(s, (lua_Integer)v))
RD(i32, int32_t, lua_pushinteger(s, v))
RD(f32, float, lua_pushnumber(s, v))
RD(f64, double, lua_pushnumber(s, v))
#define WR(name, T, get) static int m_##name(lua_State *s) { T v = (T)get(s, 2); lua_pushboolean(s, mem_write((uint64_t)luaL_checkinteger(s, 1), &v, sizeof v)); return 1; }
WR(w8, uint8_t, luaL_checkinteger)
WR(w16, uint16_t, luaL_checkinteger)
WR(w32, uint32_t, luaL_checkinteger)
WR(w64, uint64_t, luaL_checkinteger)
WR(wf32, float, luaL_checknumber)
WR(wf64, double, luaL_checknumber)
static int m_valid(lua_State *s) {
    char b;
    uint64_t a = (uint64_t)luaL_checkinteger(s, 1), n = (uint64_t)luaL_optinteger(s, 2, 1);
    lua_pushboolean(s, mem_read(a, &b, 1) && mem_read(a + (n ? n - 1 : 0), &b, 1));
    return 1;
}
static int m_bytes(lua_State *s) {
    uint64_t a = (uint64_t)luaL_checkinteger(s, 1);
    size_t n = (size_t)luaL_checkinteger(s, 2);
    if (n > (64u << 20)) return luaL_error(s, "at most 64 MB");
    char *b = malloc(n ? n : 1);
    if (!b || !mem_read(a, b, n)) { free(b); lua_pushnil(s); return 1; }
    lua_pushlstring(s, b, n);
    free(b);
    return 1;
}
static int m_str(lua_State *s) {
    uint64_t a = (uint64_t)luaL_checkinteger(s, 1);
    size_t max = (size_t)luaL_optinteger(s, 2, 256), k = 0;
    char b[4096];
    if (max > sizeof b) max = sizeof b;
    while (k < max && mem_read(a + k, &b[k], 1) && b[k]) k++;
    lua_pushlstring(s, b, k);
    return 1;
}
static int m_dump(lua_State *s) {
    uint64_t a = (uint64_t)luaL_checkinteger(s, 1);
    size_t n = (size_t)luaL_optinteger(s, 2, 64);
    luaL_Buffer out;
    luaL_buffinit(s, &out);
    for (size_t o = 0; o < n && o < 4096; o += 16) {
        unsigned char b[16];
        char line[128];
        int k = snprintf(line, sizeof line, "%" PRIx64 ":", a + o);
        if (!mem_read(a + o, b, 16)) { snprintf(line + k, sizeof line - k, " (not readable)\n"); luaL_addstring(&out, line); break; }
        for (int i = 0; i < 16; i++) k += snprintf(line + k, sizeof line - k, " %02x", b[i]);
        k += snprintf(line + k, sizeof line - k, "  ");
        for (int i = 0; i < 16; i++) line[k++] = b[i] >= 32 && b[i] < 127 ? (char)b[i] : '.';
        line[k++] = '\n', line[k] = 0;
        luaL_addstring(&out, line);
    }
    luaL_pushresult(&out);
    return 1;
}
static char *find_bytes(char *h, size_t hn, const char *n, size_t nn) {  // (memmem: not on Windows)
    for (size_t i = 0; nn && i + nn <= hn; i++)
        if (h[i] == n[0] && !memcmp(h + i, n, nn)) return h + i;
    return NULL;
}
static int m_find(lua_State *s) {
    uint64_t lo = (uint64_t)luaL_checkinteger(s, 1), hi = (uint64_t)luaL_checkinteger(s, 2);
    size_t pn;
    const char *pat = luaL_checklstring(s, 3, &pn);
    int max = (int)luaL_optinteger(s, 4, 100), found = 0;
    luaL_argcheck(s, pn > 0 && pn <= 4096, 3, "1..4096 bytes");
    const size_t chunk = 1u << 20;
    char *b = malloc(chunk + pn);
    lua_newtable(s);
    for (uint64_t a = lo; b && a < hi && found < max; a += chunk) {
        size_t n = (size_t)(hi - a < chunk ? hi - a : chunk) + pn - 1;
        if (!mem_read(a, b, n) && !mem_read(a, b, n -= pn - 1)) continue;  // (unmapped: skipped)
        for (char *p = b; found < max && (p = find_bytes(p, (size_t)(b + n - p), pat, pn)); p++) {
            lua_pushinteger(s, (lua_Integer)(a + (uint64_t)(p - b)));
            lua_rawseti(s, -2, ++found);
        }
    }
    free(b);
    return 1;
}

static int l_frame(lua_State *s) { lua_pushinteger(s, (lua_Integer)A->present_count()); return 1; }
static int l_watch(lua_State *s) {
    uint64_t a = (uint64_t)luaL_checkinteger(s, 1), n = (uint64_t)luaL_optinteger(s, 2, 4);
    luaL_argcheck(s, n == 1 || n == 2 || n == 4 || n == 8, 2, "1, 2, 4 or 8 bytes");
    if (A->abi < 5) return luaL_error(s, "watch needs a newer game build (plugin ABI 5)");
    A->watch(a, n);
    return 0;
}
static int l_watch_reset(lua_State *s) { (void)s; if (A->abi >= 5) A->watch_reset(); return 0; }

// ---- the mod API's primitives (api.lua builds npc / player / ui / ... on them). Handles and states as runtime/modapi.h
static void need_m(lua_State *s) {
    if (!M) luaL_error(s, "this game build has no mod API (npc, player, ui): update the game");
}
static void opt_pos(lua_State *s, int i, float p[3]) {
    p[0] = (float)luaL_checknumber(s, i), p[1] = (float)luaL_checknumber(s, i + 1), p[2] = (float)luaL_checknumber(s, i + 2);
}
static int r_npc_spawn(lua_State *s) {
    need_m(s);
    float p[3];
    const char *model = luaL_optstring(s, 1, ""), *label = luaL_optstring(s, 6, "");
    opt_pos(s, 2, p);
    lua_pushinteger(s, M->npc_spawn(model, p, (float)luaL_optnumber(s, 5, 0), label));
    return 1;
}
static int r_npc_remove(lua_State *s) { need_m(s); M->npc_remove((int)luaL_checkinteger(s, 1)); return 0; }
static int r_npc_walk(lua_State *s) {
    need_m(s);
    float p[3];
    opt_pos(s, 2, p);
    M->npc_walk_to((int)luaL_checkinteger(s, 1), p, lua_isinteger(s, 5) ? (int)lua_tointeger(s, 5) : lua_toboolean(s, 5));
    return 0;
}
static int r_npc_stop(lua_State *s) { need_m(s); M->npc_stop((int)luaL_checkinteger(s, 1)); return 0; }
static int r_npc_set(lua_State *s) {
    need_m(s);
    float p[3];
    opt_pos(s, 2, p);
    M->npc_set_pos((int)luaL_checkinteger(s, 1), p, (float)luaL_optnumber(s, 5, 1e9));  // (1e9: facing kept)
    return 0;
}
static int r_npc_state(lua_State *s) {
    need_m(s);
    float p[3] = {0}, yaw = 0;
    int st = M->npc_state((int)luaL_checkinteger(s, 1), p, &yaw);
    lua_pushinteger(s, st);
    if (!st) return 1;
    lua_pushnumber(s, p[0]), lua_pushnumber(s, p[1]), lua_pushnumber(s, p[2]), lua_pushnumber(s, yaw);
    return 5;
}
static int r_npc_models(lua_State *s) {
    need_m(s);
    static char b[16384];
    int n = M->npc_models(b, sizeof b);
    lua_pushlstring(s, b, n > 0 && n < (int)sizeof b ? (size_t)n : strlen(b));
    return 1;
}
static int r_player(lua_State *s) {
    need_m(s);
    float p[3], yaw = 0;
    if (!M->player_pos(p, &yaw)) return 0;
    lua_pushnumber(s, p[0]), lua_pushnumber(s, p[1]), lua_pushnumber(s, p[2]), lua_pushnumber(s, yaw);
    return 4;
}
static int r_teleport(lua_State *s) { need_m(s); float p[3]; opt_pos(s, 1, p); M->player_teleport(p); return 0; }
static int r_area(lua_State *s) {
    need_m(s);
    char n[128] = "";
    lua_pushinteger(s, (lua_Integer)M->area(n, sizeof n));
    lua_pushstring(s, n);
    return 2;
}
// (version 2 of the mod API: outfits, animation parameters, Pokémon, works)
static void need_m2(lua_State *s) {
    need_m(s);
    if (M->version < 2) luaL_error(s, "this game build's mod API is too old for this (version %d, needs 2): update the game", (int)M->version);
}
static int r_npc_outfit(lua_State *s) { need_m2(s); lua_pushboolean(s, M->npc_outfit((int)luaL_checkinteger(s, 1), luaL_checkstring(s, 2))); return 1; }
static int r_npc_anim(lua_State *s) {
    need_m2(s);
    const char *k = luaL_checkstring(s, 3);
    lua_pushboolean(s, M->npc_anim((int)luaL_checkinteger(s, 1), luaL_checkstring(s, 2), k[0], (float)luaL_optnumber(s, 4, 0)));
    return 1;
}
static int r_poke_spawn(lua_State *s) {
    need_m2(s);
    float p[3];
    opt_pos(s, 2, p);
    lua_pushinteger(s, M->pokemon_spawn((int)luaL_checkinteger(s, 1), p, (float)luaL_optnumber(s, 5, 0), (int)luaL_optinteger(s, 6, 0),
                                        lua_toboolean(s, 7), luaL_optstring(s, 8, "")));
    return 1;
}
static int r_poke_remove(lua_State *s) { need_m2(s); M->pokemon_remove((int)luaL_checkinteger(s, 1)); return 0; }
static int r_poke_state(lua_State *s) {
    need_m2(s);
    float p[3] = {0}, yaw = 0;
    int st = M->pokemon_state((int)luaL_checkinteger(s, 1), p, &yaw);
    lua_pushinteger(s, st);
    if (!st) return 1;
    lua_pushnumber(s, p[0]), lua_pushnumber(s, p[1]), lua_pushnumber(s, p[2]), lua_pushnumber(s, yaw);
    return 5;
}
static int r_work(lua_State *s) {
    need_m2(s);
    int64_t v = 0;
    int set = !lua_isnoneornil(s, 2);
    if (!M->game_work(luaL_checkstring(s, 1), set, set ? (int64_t)luaL_checkinteger(s, 2) : 0, &v)) return 0;
    lua_pushinteger(s, (lua_Integer)v);
    return 1;
}
static void need_m3(lua_State *s) {
    need_m(s);
    if (M->version < 3) luaL_error(s, "this game build's mod API is too old for this (version %d, needs 3): update the game", (int)M->version);
}
// the game's own Lua (Z-A): a job number, 0 when the game has none
static int r_game_script(lua_State *s) { need_m3(s); lua_pushinteger(s, M->game_script(luaL_checkstring(s, 1))); return 1; }
static int r_script_result(lua_State *s) {  // -> state (0 waiting, 1 ran, 2 failed, -1 gone), text
    need_m3(s);
    char out[1024];
    out[0] = 0;
    int st = M->script_result((int)luaL_checkinteger(s, 1), out, (int)sizeof out);
    lua_pushinteger(s, st);
    lua_pushstring(s, out);
    return 2;
}
static int r_object_anim(lua_State *s) {  // (object name, parameter, kind 'i'/'f'/'b'/'t', value)
    need_m3(s);
    const char *k = luaL_checkstring(s, 3);
    lua_pushboolean(s, M->object_anim(luaL_checkstring(s, 1), luaL_checkstring(s, 2), k[0], (float)luaL_optnumber(s, 4, 0)));
    return 1;
}
static int r_message_text(lua_State *s) {
    need_m3(s);
    lua_pushboolean(s, M->message_text((int)luaL_checkinteger(s, 1), luaL_checkstring(s, 2)));
    return 1;
}
// (version 4) the game's Talk prompt over a script's character: npc_talk(h, on) -> 1 done / h 0: 1 when the game has
// it; npc_talked() -> the character talked to through it (0 none). Older builds: 0 (api.lua's own A-by-one talk then)
static int r_npc_talk(lua_State *s) {
    need_m(s);
    lua_pushinteger(s, M->version >= 4 ? M->npc_talk((int)luaL_checkinteger(s, 1), (int)luaL_optinteger(s, 2, 1)) : 0);
    return 1;
}
static int r_npc_talked(lua_State *s) { need_m(s); lua_pushinteger(s, M->version >= 4 ? M->npc_talked() : 0); return 1; }
// (version 5) a speech bubble: bubble(kind, handle, object, x, y, z, height, text, seconds) -> id (0 none / older build);
// bubble_hide(id) (0: every script's)
static int r_bubble(lua_State *s) {
    need_m(s);
    if (M->version < 5) { lua_pushinteger(s, 0); return 1; }
    float p[3] = {(float)luaL_optnumber(s, 4, 0), (float)luaL_optnumber(s, 5, 0), (float)luaL_optnumber(s, 6, 0)};
    lua_pushinteger(s, M->bubble((int)luaL_checkinteger(s, 1), (int)luaL_optinteger(s, 2, 0), luaL_optstring(s, 3, ""), p,
                                 (float)luaL_optnumber(s, 7, 0), luaL_checkstring(s, 8), (float)luaL_optnumber(s, 9, 0)));
    return 1;
}
static int r_bubble_hide(lua_State *s) { need_m(s); lua_pushinteger(s, M->version >= 5 ? M->bubble_hide((int)luaL_checkinteger(s, 1)) : 0); return 1; }
// (version 7) the game's interface: ui_set(layout, pane, key, value) -> bool; ui_unset([layout [, pane]]);
// ui_layouts() / ui_panes(layout) / ui_text(layout, pane) -> text (nil: none / older build); ui_clone(layout, src, name)
static int r_ui_set(lua_State *s) {
    need_m(s);
    lua_pushboolean(s, M->version >= 7 && M->ui_set(luaL_checkstring(s, 1), luaL_optstring(s, 2, ""), luaL_checkstring(s, 3),
                                                    luaL_checkstring(s, 4)));
    return 1;
}
static int r_ui_unset(lua_State *s) {
    need_m(s);
    if (M->version >= 7) M->ui_unset(luaL_optstring(s, 1, NULL), luaL_optstring(s, 2, NULL));
    return 0;
}
static int ui_text_answer(lua_State *s, int kind) {
    need_m(s);
    if (M->version < 7) return 0;
    int cap = 1 << 18;
    char *out = malloc((size_t)cap);
    if (!out) return 0;
    int n = kind == 0 ? M->ui_layouts(out, cap) : kind == 1 ? M->ui_panes(luaL_checkstring(s, 1), out, cap)
                                                           : M->ui_text(luaL_checkstring(s, 1), luaL_checkstring(s, 2), out, cap);
    if (n >= 0) lua_pushlstring(s, out, (size_t)n);
    free(out);
    return n >= 0;
}
static int r_ui_layouts(lua_State *s) { return ui_text_answer(s, 0); }
static int r_ui_panes(lua_State *s) { return ui_text_answer(s, 1); }
static int r_ui_text(lua_State *s) { return ui_text_answer(s, 2); }
static int r_ui_clone(lua_State *s) {
    need_m(s);
    lua_pushboolean(s, M->version >= 7 && M->ui_clone(luaL_checkstring(s, 1), luaL_checkstring(s, 2), luaL_checkstring(s, 3)));
    return 1;
}
static int r_time_set(lua_State *s) { need_m3(s); lua_pushboolean(s, M->time_set((float)luaL_checknumber(s, 1))); return 1; }
static int r_item_give(lua_State *s) { need_m3(s); lua_pushboolean(s, M->item_give((int)luaL_checkinteger(s, 1), (int)luaL_checkinteger(s, 2))); return 1; }
static int r_npc_object(lua_State *s) {
    need_m3(s);
    char out[64];
    if (!M->npc_object((int)luaL_checkinteger(s, 1), out, (int)sizeof out)) return 0;
    lua_pushstring(s, out);
    return 1;
}
static int r_buttons(lua_State *s) { lua_pushinteger(s, M ? (lua_Integer)M->buttons() : 0); return 1; }
static int r_notify(lua_State *s) {
    const char *t = luaL_checkstring(s, 1);
    double sec = luaL_optnumber(s, 2, 4);
    if (M) M->notify(t, (unsigned)(sec * 1000));
    A->log("[lua] (on screen) %s\n", t);
    return 0;
}
static int r_now(lua_State *s) { lua_pushnumber(s, (double)now_ns() / 1e9); return 1; }
static int r_cpu(lua_State *s) { lua_pushnumber(s, (double)cpu_ns() / 1e9); return 1; }  // (this thread's CPU time: profiling)
static int r_stats(lua_State *s) {
    lua_newtable(s);
    double f = g_st.frames ? (double)g_st.frame_ns / g_st.frames / 1000.0 : 0, h = g_st.hooks ? (double)g_st.hook_ns / g_st.hooks / 1000.0 : 0;
    lua_pushnumber(s, f), lua_setfield(s, -2, "frame_us");        // (average per frame, render thread)
    lua_pushnumber(s, g_st.frame_max / 1000.0), lua_setfield(s, -2, "frame_max_us");
    lua_pushinteger(s, (lua_Integer)g_st.frames), lua_setfield(s, -2, "frames");
    lua_pushinteger(s, (lua_Integer)g_st.hooks), lua_setfield(s, -2, "hook_calls");
    lua_pushnumber(s, h), lua_setfield(s, -2, "hook_us");          // (average per hook call, the game's threads)
    lua_pushnumber(s, g_st.hook_max / 1000.0), lua_setfield(s, -2, "hook_max_us");
    lua_pushinteger(s, (lua_Integer)(g_mem >> 10)), lua_setfield(s, -2, "memory_kb");
    return 1;
}
static int r_stats_reset(lua_State *s) { (void)s; memset(&g_st, 0, sizeof g_st); return 0; }
// load a script file into env (the table at 2): the chunk, or nil + the error
static int r_loadfile(lua_State *s) {
    const char *path = luaL_checkstring(s, 1);
    luaL_checktype(s, 2, LUA_TTABLE);
    if (luaL_loadfilex(s, path, "t")) { lua_pushnil(s); lua_insert(s, -2); return 2; }  // (text only: no bytecode)
    lua_pushvalue(s, 2);
    lua_setupvalue(s, -2, 1);  // (_ENV)
    return 1;
}

#include "world.c"  // (the world side: net, party, battles, the player held... - world.lua on top)
#include "questlib.c"  // (the quest library: lib/quest.lua)

static const char k_api_lua[] = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed "api.lua"
#pragma clang diagnostic pop
    , 0};

// ---- the mod API's version 6 (the overlay, input, sounds, files, storage) and its Lua: run after api.lua with the same
// rt table (hud.lua: draw / hud / input / sound / storage / files; gui.lua: menus)
#include "ext.c"
static const char k_hud_lua[] = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed "hud.lua"
#pragma clang diagnostic pop
    , 0};
static const char k_gui_lua[] = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed "gui.lua"
#pragma clang diagnostic pop
    , 0};
static void ext_lua(const char *name, const char *code) {  // (the rt table at the top of the stack stays)
    if (luaL_loadbufferx(L, code, strlen(code), name, "t")) { A->log("[lua] %s: %s\n", name + 1, lua_tostring(L, -1)), lua_pop(L, 1); return; }
    lua_pushvalue(L, -2);
    if (pcall_traced(L, 1, 0)) A->log("[lua] %s: %s\n", name + 1, lua_tostring(L, -1)), lua_pop(L, 1);
}

static void new_state(void) {
    g_mem = 0;
    L = lua_newstate(l_alloc, NULL);
    lua_sethook(L, count_hook, LUA_MASKCOUNT, LUA_STEPS);
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_settop(L, 0);  // (no io / os / package / debug: scripts get the game, not the PC)
    lua_pushnil(L), lua_setglobal(L, "dofile");  // (files only through include: a mod's own folder)
    lua_pushnil(L), lua_setglobal(L, "loadfile");
    lua_newtable(L), lua_setfield(L, LUA_REGISTRYINDEX, "hooks");
    lua_newtable(L), lua_setfield(L, LUA_REGISTRYINDEX, "frames");
    lua_register(L, "log", l_log);
    lua_register(L, "print", l_log);
    lua_register(L, "hook", l_hook);
    lua_register(L, "on_frame", l_on_frame);
    lua_register(L, "watch", l_watch);
    lua_register(L, "watch_reset", l_watch_reset);
    static const luaL_Reg cm[] = {{"x", c_x}, {"setx", c_setx}, {"s", c_s}, {"sets", c_sets}, {"vf", c_vf}, {"setvf", c_setvf}, {"d", c_d}, {"setd", c_setd},
                                  {"original", c_original}, {"call", c_call}, {"bt", c_bt}, {NULL, NULL}};
    luaL_newmetatable(L, "legends.cpu");
    luaL_newlib(L, cm), lua_setfield(L, -2, "methods");
    lua_pushcfunction(L, c_index), lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    static const luaL_Reg mm[] = {{"u8", m_u8}, {"u16", m_u16}, {"u32", m_u32}, {"u64", m_u64}, {"i32", m_i32}, {"f32", m_f32},
                                  {"f64", m_f64}, {"w8", m_w8}, {"w16", m_w16}, {"w32", m_w32}, {"w64", m_w64}, {"wf32", m_wf32},
                                  {"wf64", m_wf64}, {"valid", m_valid}, {"bytes", m_bytes}, {"str", m_str}, {"dump", m_dump},
                                  {"find", m_find}, {NULL, NULL}};
    luaL_newlib(L, mm), lua_setglobal(L, "mem");
    lua_newtable(L);
    char t[32];
    snprintf(t, sizeof t, "%016" PRIX64, A->title_id());
    lua_pushstring(L, t), lua_setfield(L, -2, "title");
    lua_pushstring(L, A->data_root()), lua_setfield(L, -2, "root");
    lua_pushcfunction(L, l_frame), lua_setfield(L, -2, "frame");
    lua_pushstring(L, M ? M->game : "?"), lua_setfield(L, -2, "name");
    lua_setglobal(L, "game");
    // the mod API (api.lua), given the primitives
    static const luaL_Reg rm[] = {{"npc_spawn", r_npc_spawn}, {"npc_remove", r_npc_remove}, {"npc_walk", r_npc_walk}, {"npc_stop", r_npc_stop},
                                  {"npc_set", r_npc_set}, {"npc_state", r_npc_state}, {"npc_models", r_npc_models}, {"player", r_player},
                                  {"teleport", r_teleport}, {"area", r_area}, {"buttons", r_buttons}, {"notify", r_notify}, {"now", r_now}, {"cpu", r_cpu},
                                  {"stats", r_stats}, {"stats_reset", r_stats_reset}, {"loadfile", r_loadfile}, {"log", l_log},
                                  {"npc_outfit", r_npc_outfit}, {"npc_anim", r_npc_anim}, {"poke_spawn", r_poke_spawn}, {"poke_remove", r_poke_remove},
                                  {"poke_state", r_poke_state}, {"work", r_work}, {"game_script", r_game_script},
                                  {"script_result", r_script_result}, {"item_give", r_item_give}, {"npc_object", r_npc_object},
                                  {"time_set", r_time_set}, {"message_text", r_message_text}, {"object_anim", r_object_anim},
                                  {"npc_talk", r_npc_talk}, {"npc_talked", r_npc_talked},
                                  {"bubble", r_bubble}, {"bubble_hide", r_bubble_hide},
                                  {"ui_set", r_ui_set}, {"ui_unset", r_ui_unset}, {"ui_layouts", r_ui_layouts}, {"ui_panes", r_ui_panes},
                                  {"ui_text", r_ui_text}, {"ui_clone", r_ui_clone},
                                  {"trace", l_trace}, {NULL, NULL}};
    uint64_t was = deadline_set(LUA_LOAD_MS);
    if (luaL_loadbufferx(L, k_api_lua, strlen(k_api_lua), "=api.lua", "t")) {
        A->log("[lua] api.lua: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        luaL_newlib(L, rm);
        lua_pushboolean(L, M != NULL), lua_setfield(L, -2, "have_api");
        lua_pushinteger(L, M ? (lua_Integer)M->version : 0), lua_setfield(L, -2, "api_version");
        ext_register(L);
        lua_pushvalue(L, -1);
        lua_insert(L, -3);  // (a copy of rt kept under the chunk for the extensions)
        if (pcall_traced(L, 1, 0)) A->log("[lua] api.lua: %s\n", lua_tostring(L, -1)), lua_pop(L, 1);
        else {
            ext_lua("=hud.lua", k_hud_lua);
            ext_lua("=gui.lua", k_gui_lua);
            world_open(L);
            quest_open(L);
        }
        lua_pop(L, 1);
    }
    g_deadline = was;
}

// ---- the scripts: the game folder's lua/ (the shared globals) and each mod's lua/ (its own), in that order
#define MAXDIRS 33
typedef struct { char dir[1024], mod[128]; } ScriptDir;
static int script_dirs(ScriptDir *d, int max) {
    int n = 0;
    snprintf(d[n].dir, sizeof d[n].dir, "%s/lua", A->data_root()), d[n++].mod[0] = 0;
    if (!M) return n;
    char rel[MAXDIRS][256];
    int k = M->mod_lua_dirs(rel, MAXDIRS - 1);
    for (int i = 0; i < k && n < max; i++, n++) {  // ("mods/<name>/lua")
        snprintf(d[n].dir, sizeof d[n].dir, "%s/%s", A->data_root(), rel[i]);
        const char *a = rel[i] + 5, *b = strrchr(rel[i], '/');
        snprintf(d[n].mod, sizeof d[n].mod, "%.*s", b && b > a ? (int)(b - a) : 127, a);
    }
    return n;
}
static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
// a folder's scripts' names (sorted) and a stamp of their times and sizes (a change -> a reload)
static long long scan(const char *dir, char (*names)[256], int *n, int max) {
    long long stamp = 0;
    *n = 0;
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) {
        size_t k = strlen(e->d_name);
        if (k < 5 || k >= 256 || strcmp(e->d_name + k - 4, ".lua")) continue;
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st)) continue;
        stamp = stamp * 31 + (long long)st.st_mtime * 1000003 + (long long)st.st_size + (long long)k;
        if (!names) (*n)++;
        else if (*n < max) snprintf(names[(*n)++], 256, "%s", e->d_name);
    }
    if (d) closedir(d);
    if (names) qsort(names, (size_t)*n, 256, name_cmp);
    return stamp;
}
// (the watcher thread and the loader both: no shared buffers - names not kept)
static long long scan_all(int *nfiles) {
    ScriptDir d[MAXDIRS];
    int nd = script_dirs(d, MAXDIRS), total = 0;
    long long stamp = nd;
    for (int i = 0; i < nd; i++) {
        int n;
        stamp = stamp * 1000003 + scan(d[i].dir, NULL, &n, 0) + n;
        total += n;
    }
    if (nfiles) *nfiles = total;
    return stamp;
}

// run one folder's scripts in the environment api.lua makes for its mod (the shared one for the game folder's)
static void run_dir(const ScriptDir *sd) {
    static char names[128][256];
    int n;
    scan(sd->dir, names, &n, 128);
    if (!n) return;
    lua_getglobal(L, "__mod_env");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushstring(L, sd->mod), lua_pushstring(L, sd->dir);
    uint64_t was = deadline_set(LUA_LOAD_MS);
    if (pcall_traced(L, 2, 1)) { A->log("[lua] %s: %s\n", sd->mod, lua_tostring(L, -1)), lua_pop(L, 1); g_deadline = was; return; }
    for (int i = 0; i < n; i++) {
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", sd->dir, names[i]);
        A->log("[lua] running %s%s%s\n", sd->mod[0] ? sd->mod : "", sd->mod[0] ? "/" : "", names[i]);
        lua_getglobal(L, "__mod_run");  // (__mod_run(env, path, file): loads and runs it; a failure is reported there)
        lua_pushvalue(L, -2), lua_pushstring(L, p), lua_pushstring(L, names[i]);
        g_deadline = now_ns() + LUA_LOAD_MS * 1000000ull;  // (each script its own time)
        if (pcall_traced(L, 3, 0)) A->log("[lua] %s: %s\n", names[i], lua_tostring(L, -1)), lua_pop(L, 1);
    }
    lua_pop(L, 1);
    g_deadline = was;
}

static void load_scripts(void) {
    if (L) {
        lua_getglobal(L, "__unload");  // (what the old scripts made goes first: their characters...)
        if (lua_isfunction(L, -1)) { uint64_t was = deadline_set(LUA_SLICE_MS); if (pcall_traced(L, 0, 0)) lua_pop(L, 1); g_deadline = was; }
        else lua_pop(L, 1);
        lua_close(L);
        L = NULL;
    }
    if (M) {
        M->npc_remove_all();
        if (M->version >= 5) M->bubble_hide(0);  // (their bubbles too)
        if (M->version >= 7) M->ui_unset(NULL, NULL);  // (and their interface edits)
        int n = 0;
        scan_all(&n);
        if (n) M->enable();  // (scripts added while the game runs: from now on)
    }
    new_state();
    for (int i = 0; i < g_nhooked; i++) g_errs[i] = 0;
    ScriptDir d[MAXDIRS];
    int nd = script_dirs(d, MAXDIRS);
    for (int i = 0; i < nd; i++) run_dir(&d[i]);
    lua_getglobal(L, "__loaded");
    if (lua_isfunction(L, -1)) { uint64_t was = deadline_set(LUA_SLICE_MS); if (pcall_traced(L, 0, 0)) A->log("[lua] %s\n", lua_tostring(L, -1)), lua_pop(L, 1); g_deadline = was; }
    else lua_pop(L, 1);
}

// ---- the watcher (a thread of its own, twice a second: no file system work on the game's threads): a script changed ->
// g_reload; the console's files there -> g_console / g_cmdfile
static volatile int g_reload, g_console, g_cmdfile, g_watch_on;
static long long g_stamp = -1;
static void *watcher(void *arg) {
    (void)arg;
    char run[1100], cmd[1100];
    snprintf(run, sizeof run, "%s/lua_run.lua", g_live);
    snprintf(cmd, sizeof cmd, "%s/lua_cmd", g_live);
    for (;;) {
#ifdef _WIN32
        Sleep(500);
#else
        usleep(500000);
#endif
        ext_flush_writes();  // (scripts' storage / files: hud.lua)
        if (!g_watch_on) continue;
        long long st = scan_all(NULL);
        if (g_stamp != -1 && st != g_stamp) g_reload = 1;
        g_stamp = st;
        struct stat sb;
        if (!g_console && !stat(run, &sb)) g_console = 1;
        if (!g_cmdfile && !stat(cmd, &sb)) g_cmdfile = 1;
    }
    return NULL;
}

static void console_run(uint64_t frame) {
    char p[1100], o[1100];
    snprintf(p, sizeof p, "%s/lua_run.lua", g_live);
    snprintf(o, sizeof o, "%s/lua_out.txt", g_live);
    g_out = fopen(o, "a");
    uint64_t was = deadline_set(LUA_LOAD_MS);
    if (luaL_loadfilex(L, p, "t") || pcall_traced(L, 0, 0)) {
        const char *e = lua_tostring(L, -1);
        A->log("[lua] lua_run.lua: %s\n", e);
        if (g_out) fprintf(g_out, "error: %s\n", e);
        lua_pop(L, 1);
    }
    g_deadline = was;
    remove(p);
    if (g_out) fprintf(g_out, "-- done (frame %" PRIu64 ")\n", frame), fclose(g_out), g_out = NULL;
}
static void cmdfile_run(uint64_t frame) {
    char p[1100], o[1100], line[1024];
    snprintf(p, sizeof p, "%s/lua_cmd", g_live);
    snprintf(o, sizeof o, "%s/lua_out.txt", g_live);
    FILE *f = fopen(p, "r");
    if (!f) return;
    g_out = fopen(o, "a");
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        lua_getglobal(L, "__cmd_line");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); break; }
        lua_pushstring(L, line);
        uint64_t was = deadline_set(LUA_SLICE_MS);
        if (pcall_traced(L, 1, 0)) {
            const char *e = lua_tostring(L, -1);
            if (g_out) fprintf(g_out, "error: %s\n", e);
            A->log("[lua] command %s: %s\n", line, e);
            lua_pop(L, 1);
        }
        g_deadline = was;
    }
    fclose(f);
    remove(p);
    if (g_out) fprintf(g_out, "-- done (frame %" PRIu64 ")\n", frame), fclose(g_out), g_out = NULL;
}

static void game_start(void) {  // (before the game's first instruction: boot-time calls can be hooked too)
    pthread_mutex_lock(&g_lock);
    load_scripts();
    g_watch_on = 1;
    pthread_mutex_unlock(&g_lock);
}

static void game_frame(uint64_t frame) {
    pthread_mutex_lock(&g_lock);
    if (!L) load_scripts(), g_watch_on = 1;  // (a build without hooks.game_start: the first frame - the runtime's hooks are set by now)
    if (g_reload) g_reload = 0, A->log("[lua] scripts changed: reloading\n"), load_scripts();
    if (g_console) console_run(frame), g_console = 0;
    if (g_cmdfile) cmdfile_run(frame), g_cmdfile = 0;
    uint64_t t0 = cpu_ns(), was = deadline_set(LUA_SLICE_MS);
    static uint64_t last_tick;
    static lua_Integer every = 1;  // (api.lua's __tick_every: 1 while scripts need each frame, else 4)
    if (frame - last_tick >= (uint64_t)every || frame < last_tick) {
        last_tick = frame;
        lua_getglobal(L, "__tick");  // (api.lua: events, timers, the characters' states)
        if (lua_isfunction(L, -1)) {
            lua_pushinteger(L, (lua_Integer)frame);
            if (pcall_traced(L, 1, 0)) {
                static int errs;
                if (errs++ < 5) A->log("[lua] tick: %s\n", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else lua_pop(L, 1);
        lua_getglobal(L, "__tick_every");
        every = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 1;
        if (every < 1 || every > 8) every = 1;
        lua_pop(L, 1);
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "frames");
    for (lua_Integer i = 1, n = (lua_Integer)lua_rawlen(L, -1); i <= n; i++) {
        lua_rawgeti(L, -1, i);
        lua_pushinteger(L, (lua_Integer)frame);
        if (pcall_traced(L, 1, 0)) {
            static int errs;
            if (errs++ < 5) A->log("[lua] on_frame: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    g_deadline = was;
    uint64_t dt = cpu_ns() - t0;  // (CPU time of the render thread: lua.stats)
    g_st.frames++, g_st.frame_ns += dt;
    if (dt > g_st.frame_max) g_st.frame_max = dt;
    static int stats = -1;
    if (stats < 0) stats = getenv("LARECOMP_LUA_STATS") != NULL;
    if (stats) {  // (the last minute's cost, logged)
        g_st_win.frames++, g_st_win.frame_ns += dt;
        if (dt > g_st_win.frame_max) g_st_win.frame_max = dt;
        if (g_st_win.frames >= 3600) {
            uint64_t hk = g_st.hooks - g_st_win.hooks, hns = g_st.hook_ns - g_st_win.hook_ns;
            A->log("[lua] cost: %.1f us a frame on average (max %.1f) over %llu frames; hooks %llu calls, %.1f us each; Lua memory %zu KB\n",
                   (double)g_st_win.frame_ns / g_st_win.frames / 1000.0, (double)g_st_win.frame_max / 1000.0, (unsigned long long)g_st_win.frames,
                   (unsigned long long)hk, hk ? (double)hns / hk / 1000.0 : 0.0, g_mem >> 10);
            memset(&g_st_win, 0, sizeof g_st_win);
            g_st_win.hooks = g_st.hooks, g_st_win.hook_ns = g_st.hook_ns;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int larecomp_plugin_init(const LarecompPluginApi *api, LarecompPluginHooks *hooks) {
    if (api->abi < 4) { api->log("[lua] this game build is too old for Legends Lua (plugin ABI %u, needs 4)\n", api->abi); return 1; }
    A = api;
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);  // (a hook's call into the game may reach another hook)
    pthread_mutex_init(&g_lock, &at);
    const char *live = getenv("LARECOMP_LIVE");
    snprintf(g_live, sizeof g_live, "%s", live && *live ? live : api->data_root());
    hooks->game_frame = game_frame;
    if (api->abi >= 5) hooks->game_start = game_start;
    pthread_t th;
    if (!pthread_create(&th, NULL, watcher, NULL)) pthread_detach(th);
    A->log("[lua] Legends Lua ready (%s)\n", LUA_RELEASE);
    return 0;
}
