// Arceus' own Lua: mods' scripts run Lua text in the game's scripts (the mod API's game_script, runtime/modapi.h) - the
// Arceus side of Z-A's hle/za_script.c.
//
// Arceus runs Lua 5.3 with sol2 bindings (exe strings: sol::usertype_metatable<...>, prog/event_script/source/lua/...):
// its events and field scripts are Haxe compiled to Lua (romfs bin/haxe/release/event/*.blua, field/*.blua; Studio's
// `lua` decompiles them - their names are plain: SoundCommand.PostEvent, ...). One state per script: the player's field
// script (field_player_script.blua) has a state of its own, called from C++ once a frame - lua_callk(L, 2, -1) at
// 0x7001d1a5b0 (2026-11-19 trace: Legends Lua hooks on lua_pcallk / luaL_loadbufferx). A job runs there before that call,
// on the same state, the stack put back as it was.
//   The API from its users (the base library's {name, function} table 0x700401a1b8: pcall 0x70000567e0, load
// 0x70000565e0): lua_callk, lua_pcallk, lua_gettop, lua_settop, lua_tolstring, luaL_loadbufferx below.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));  // runtime/fnhook.c
void *hle_alloc(uint64_t size);

#define LUA_CALLK 0x7000052d80ull
#define LUA_PCALLK 0x7000052e10ull
#define LUA_GETTOP 0x700004fef0ull
#define LUA_SETTOP 0x700004ff10ull
#define LUA_TOLSTRING 0x7000050da0ull
#define LUAL_LOADBUFFERX 0x70000555e0ull
#define FRAME_PCALL_LR 0x7001d1a5b4ull  // (the player's field script, called once a frame)

#define NJOB 64
#define CODE_MAX (256 * 1024)
#define OUT_MAX 1024
typedef struct {
    int id, state;  // (0 free, 1 queued, 2 done, 3 failed)
    char *code;
    size_t len;
    char out[OUT_MAX];
} Job;
static Job g_job[NJOB];
static int g_next_id = 1;
static volatile int g_queued;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_buf;  // (guest memory: the text, its name, the mode, a size_t)
static uint64_t g_state;  // (the main state, once seen)
static volatile uint64_t g_runs, g_ns;

static int pcall_hook(Cpu *c, uint64_t addr);
static void hook_on(void) {
    static int done;
    if (done) return;
    done = 1;
    g_buf = hle_alloc(CODE_MAX + 256);
    rt_register_fnhook_at(LUA_CALLK, pcall_hook);
}

int pla_script(const char *code) {
    if (!code) return 0;
    hook_on();
    size_t n = strlen(code);
    if (n >= CODE_MAX) return 0;
    pthread_mutex_lock(&g_lock);
    int id = 0;
    for (int i = 0; i < NJOB && !id; i++)
        if (!g_job[i].state) {
            Job *j = &g_job[i];
            j->code = malloc(n + 1);
            if (!j->code) break;
            memcpy(j->code, code, n + 1);
            j->len = n, j->id = id = g_next_id++, j->state = 1, j->out[0] = 0;
            g_queued++;
        }
    pthread_mutex_unlock(&g_lock);
    return id;
}

// 0 still to run, 1 ran (out: what it returned, as text), 2 failed (out: Lua's message), -1 no such job (taken already)
int pla_script_result(int id, char *out, int cap) {
    pthread_mutex_lock(&g_lock);
    int r = -1;
    for (int i = 0; i < NJOB; i++)
        if (g_job[i].state && g_job[i].id == id) {
            Job *j = &g_job[i];
            r = j->state == 1 ? 0 : j->state == 2 ? 1 : 2;
            if (r && out && cap > 0) snprintf(out, (size_t)cap, "%s", j->out);
            if (r) j->state = 0;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static uint64_t call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3, c->x[4] = a4, c->x[5] = a5;
    guest_call(c, fn);
    return c->x[0];
}

static int pcall_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[30] != FRAME_PCALL_LR) return 0;
    g_state = c->x[0];
    if (!g_queued) return 0;
    uint64_t t0 = plat_now_ns();
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    uint64_t L = sx[0];
    char *name = (char *)g_buf + CODE_MAX, *mode = name + 64;
    uint64_t *lenp = (uint64_t *)(void *)(g_buf + CODE_MAX + 128);
    snprintf(name, 64, "=mod");
    snprintf(mode, 8, "t");
    for (int i = 0; i < NJOB; i++) {
        pthread_mutex_lock(&g_lock);
        Job *j = g_job[i].state == 1 ? &g_job[i] : NULL;
        size_t len = 0;
        if (j) memcpy(g_buf, j->code, j->len), len = j->len, free(j->code), j->code = NULL;
        pthread_mutex_unlock(&g_lock);
        if (!j) continue;
        int64_t top = (int32_t)call(c, LUA_GETTOP, L, 0, 0, 0, 0, 0);
        int st = (int)call(c, LUAL_LOADBUFFERX, L, (uint64_t)(uintptr_t)g_buf, len, (uint64_t)(uintptr_t)name, (uint64_t)(uintptr_t)mode, 0);
        if (st == 0) st = (int)call(c, LUA_PCALLK, L, 0, 1, 0, 0, 0);
        *lenp = 0;
        uint64_t s = call(c, LUA_TOLSTRING, L, (uint64_t)-1, (uint64_t)(uintptr_t)lenp, 0, 0, 0);
        char out[OUT_MAX];
        size_t n = s && *lenp ? (*lenp < OUT_MAX - 1 ? *lenp : OUT_MAX - 1) : 0;
        if (n) memcpy(out, (const void *)(uintptr_t)s, n);
        out[n] = 0;
        call(c, LUA_SETTOP, L, (uint64_t)top, 0, 0, 0, 0);
        pthread_mutex_lock(&g_lock);
        snprintf(j->out, OUT_MAX, "%s", out);
        j->state = st == 0 ? 2 : 3;
        g_queued--;
        pthread_mutex_unlock(&g_lock);
        if (st) LOG("[script] a mod's game script failed: %s\n", out);
    }
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
    g_runs++, g_ns += plat_now_ns() - t0;
    return 0;
}

