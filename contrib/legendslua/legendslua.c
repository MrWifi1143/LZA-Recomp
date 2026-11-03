// Legends Lua: game scripting from Lua 5.3 (third_party/lua), as a runtime plugin (runtime/plugin.h). For research and
// mods that need code: hook any game function, read and write game memory, call the game's functions, run something
// every frame. Built into <game folder>/plugins/ (make legendslua); off until switched on in the Mods page.
//
// Scripts: <game folder>/lua/*.lua, run in name order as the game starts (the runtime's own hooks set by then), run
// again when one of them changes (hot reload: the hooks they set are dropped first). A live console: a file
// <dir>/lua_run.lua (dir: LARECOMP_LIVE, else the game folder) is run once and removed; what it prints goes to the log
// and to <dir>/lua_out.txt.
//
// The API (addresses as the running game has them, main at 0x7000000000):
//   log(...) / print(...)              -> the log ("[lua] ...")
//   hook(addr, fn)                     fn(c) when the game calls addr; return true to skip the game's function (set
//                                      c's x0 first). Several on one address run in order (one skipping ends it);
//                                      hook(addr, nil) drops them all.
//   on_frame(fn)                       fn(frame) every game frame (render thread: memory yes, calls no)
//   c:x(n) c:setx(n, v)                registers x0..x30 (integers, 64-bit)
//   c:s(n) c:sets(n, v) c:d(n) c:setd(n, v)   float / double in v0..v31; c:vf(n, lane) a vector's float 0..3
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

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

#include "plugin.h"
#include "lua/lauxlib.h"
#include "lua/lua.h"
#include "lua/lualib.h"

static const LarecompPluginApi *A;
static lua_State *L;
static pthread_mutex_t g_lock;
static char g_dir[1024], g_live[1024];

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
    if (g_out) fprintf(g_out, "%s\n", line);
    return 0;
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
        lua_getfield(L, LUA_REGISTRYINDEX, "hooks");
        lua_rawgeti(L, -1, (lua_Integer)addr);  // (a list: every script's function on this address, in order)
        if (lua_istable(L, -1)) {
            CpuRef *r = (CpuRef *)lua_newuserdata(L, sizeof *r);
            r->c = c, r->addr = addr;
            luaL_setmetatable(L, "legends.cpu");
            for (lua_Integer k = 1, n = (lua_Integer)lua_rawlen(L, -2); k <= n && !skip; k++) {
                lua_rawgeti(L, -2, k);
                lua_pushvalue(L, -2);
                if (lua_pcall(L, 1, 1, 0)) {
                    int i = hook_index(addr);
                    if (i >= 0 && g_errs[i]++ < 3) A->log("[lua] hook %" PRIx64 ": %s\n", addr, lua_tostring(L, -1));
                } else skip = lua_toboolean(L, -1);
                lua_pop(L, 1);
            }
            r->c = NULL;  // (the registers belong to this call only)
            lua_pop(L, 3);
        } else lua_pop(L, 2);
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
#define RD(name, T, push) static int m_##name(lua_State *s) { T v; if (!mem_read((uint64_t)luaL_checkinteger(s, 1), &v, sizeof v)) return 0; push; return 1; }
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
    if (!b || !mem_read(a, b, n)) { free(b); return 0; }
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
        for (char *p = b; found < max && (p = memmem(p, (size_t)(b + n - p), pat, pn)); p++) {
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

static void new_state(void) {
    L = luaL_newstate();
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_settop(L, 0);  // (no io / os / package: scripts get the game, not the PC)
    lua_newtable(L), lua_setfield(L, LUA_REGISTRYINDEX, "hooks");
    lua_newtable(L), lua_setfield(L, LUA_REGISTRYINDEX, "frames");
    lua_register(L, "log", l_log);
    lua_register(L, "print", l_log);
    lua_register(L, "hook", l_hook);
    lua_register(L, "on_frame", l_on_frame);
    lua_register(L, "watch", l_watch);
    lua_register(L, "watch_reset", l_watch_reset);
    static const luaL_Reg cm[] = {{"x", c_x}, {"setx", c_setx}, {"s", c_s}, {"sets", c_sets}, {"vf", c_vf}, {"d", c_d}, {"setd", c_setd},
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
    lua_setglobal(L, "game");
}

static void run_file(const char *path) {
    if (luaL_dofile(L, path)) A->log("[lua] %s: %s\n", path, lua_tostring(L, -1)), lua_pop(L, 1);
}

static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
// The scripts' names and a stamp of their times and sizes (a change -> a reload)
static long long scan(char (*names)[256], int *n, int max) {
    long long stamp = 0;
    *n = 0;
    DIR *d = opendir(g_dir);
    for (struct dirent *e; d && (e = readdir(d));) {
        size_t k = strlen(e->d_name);
        if (k < 5 || k >= 256 || strcmp(e->d_name + k - 4, ".lua")) continue;
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
        if (stat(p, &st)) continue;
        stamp = stamp * 31 + (long long)st.st_mtime * 1000003 + (long long)st.st_size + (long long)k;
        if (*n < max) snprintf(names[(*n)++], 256, "%s", e->d_name);
    }
    if (d) closedir(d);
    qsort(names, (size_t)*n, 256, name_cmp);
    return stamp;
}

static void load_scripts(void) {
    static char names[128][256];
    int n;
    scan(names, &n, 128);
    if (L) lua_close(L);
    new_state();
    for (int i = 0; i < g_nhooked; i++) g_errs[i] = 0;
    for (int i = 0; i < n; i++) {
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", g_dir, names[i]);
        A->log("[lua] running %s\n", names[i]);
        run_file(p);
    }
}

static void game_start(void) {  // (before the game's first instruction: boot-time calls can be hooked too)
    pthread_mutex_lock(&g_lock);
    load_scripts();
    pthread_mutex_unlock(&g_lock);
}

static void game_frame(uint64_t frame) {
    pthread_mutex_lock(&g_lock);
    if (!L) load_scripts();  // (a build without hooks.game_start: the first frame - the runtime's hooks are set by now)
    if (frame % 30 == 0) {  // (twice a second: the scripts changed? the console?)
        static long long last = -1;
        static char names[128][256];
        int n;
        long long st = scan(names, &n, 128);
        if (last != -1 && st != last) A->log("[lua] scripts changed: reloading\n"), load_scripts();
        last = st;
        char p[1100];
        snprintf(p, sizeof p, "%s/lua_run.lua", g_live);
        FILE *f = fopen(p, "r");
        if (f) {
            fclose(f);
            char o[1100];
            snprintf(o, sizeof o, "%s/lua_out.txt", g_live);
            g_out = fopen(o, "a");
            run_file(p);
            remove(p);
            if (g_out) fprintf(g_out, "-- done (frame %" PRIu64 ")\n", frame), fclose(g_out), g_out = NULL;
        }
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "frames");
    for (lua_Integer i = 1, n = (lua_Integer)lua_rawlen(L, -1); i <= n; i++) {
        lua_rawgeti(L, -1, i);
        lua_pushinteger(L, (lua_Integer)frame);
        if (lua_pcall(L, 1, 0, 0)) {
            static int errs;
            if (errs++ < 5) A->log("[lua] on_frame: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
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
    snprintf(g_dir, sizeof g_dir, "%s/lua", api->data_root());
    const char *live = getenv("LARECOMP_LIVE");
    snprintf(g_live, sizeof g_live, "%s", live && *live ? live : api->data_root());
    hooks->game_frame = game_frame;
    if (api->abi >= 5) hooks->game_start = game_start;
    A->log("[lua] Legends Lua ready (%s)\n", LUA_RELEASE);
    return 0;
}
