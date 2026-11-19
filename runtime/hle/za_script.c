// Z-A's own Lua: mods' scripts run Lua text in the game's event framework (the mod API's game_script, runtime/modapi.h).
//
// The game's events are Haxe compiled to Lua 5.4 (romfs script/lua/bin/release/main/main.blua - the framework: Commands,
// FObj_*, the natives; event_ik/event_ik.blua - every event), run by the Lua 5.4 inside the game. Its main state takes
// one lua_pcallk a frame from the game's C++ (return address FRAME_PCALL_LR - 2026-11-19 trace,
// contrib/legendslua/research/za_luatrace.lua): at that call the VM is idle, so a job queued by a script is loaded
// and run there first, on the same state, and the stack put back as it was. What the job returns comes back as text.
//   The API found by its users (romfs code: the base library's pcall / load, 0x7003c086a0 {name, function} table):
// lua_pcallk, lua_gettop, lua_settop, lua_tolstring, luaL_loadbufferx below. The parser is in (load takes text).
//   The framework's globals have hashed names (Studio's zalua names them back): Commands = C0C062DE5AED06885
// (AddItem(item, n), FObj_Set_Pos_Ang(name, x, y, z, yaw), FObj_Get_Pos(name), FObj_Set_Animation(name, n),
// FObj_Move_To_PosXYZ_Navi(name, x, y, z, ...), S0D43FC7826F5A0FA(name, {x, y, z}, yaw, species, form, gender, ...) the
// event Pokemon make, S38F7587912F54E9A(name) its removal) - docs/lua-api.md has the ones the mod API uses.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));  // runtime/fnhook.c
void *hle_alloc(uint64_t size);

#define LUA_PCALLK 0x7000111bd0ull
#define LUA_GETTOP 0x70001115f0ull
#define LUA_SETTOP 0x7000111810ull
#define LUA_TOLSTRING 0x70004a2950ull
#define LUAL_LOADBUFFERX 0x70017ab5c0ull
#define FRAME_PCALL_LR 0x7000111560ull  // (the game's once-a-frame call into its framework's main state)
// The time of day (the mod API's time_set): the clock the bench's "wait until night / morning" sets. Its native
// c56DE81E7.fC88AB2B8(hour) (0x70027fbef0 -> 0x700294b28c) gets the time manager (0x700090de20 -> handle, 0x7000404348,
// 0x700090dd70 lets the handle go), whose +0x40 is the clock, and sets it with 0x7000313e60(clock, force, seconds, 6).
// Unforced (the native's way) the clock refuses while any of its holds (+0x5c..+0x70, seconds; -1 none) is set, and
// the story sets one: the test save's phase holds 15:00 at +0x68 (2026-11-19 - set forced alone, the next area load
// put 15:00 back). So the holds are let go (-1) and the clock set forced: the city goes to night (lit windows, the
// night zones on the map) after the game's own short black transition.
#define TIME_MGR_GET 0x700090de20ull
#define TIME_MGR_OF 0x7000404348ull
#define TIME_HANDLE_FREE 0x700090dd70ull
#define CLOCK_SET 0x7000313e60ull

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
static volatile int g_time_req;
static float g_time_hour;
static void time_run(Cpu *c);
static void hook_on(void) {
    static int done;
    if (done) return;
    done = 1;
    g_buf = hle_alloc(CODE_MAX + 256);
    rt_register_fnhook_at(LUA_PCALLK, pcall_hook);
}

int za_script(const char *code) {
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
int za_script_result(int id, char *out, int cap) {
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
    if (!g_queued && !g_time_req) return 0;
    uint64_t t0 = plat_now_ns();
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    if (g_time_req) time_run(c), memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv);
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

int za_time_set(float hour) {
    if (!(hour >= 0.0f && hour < 24.0f)) return 0;
    hook_on();
    g_time_hour = hour, g_time_req = 1;
    return 1;
}
static void time_run(Cpu *c) {  // (the game's thread, regs saved by the caller)
    g_time_req = 0;
    uint64_t *out = (uint64_t *)(void *)(g_buf + CODE_MAX + 192);
    out[0] = out[1] = 0;
    c->x[8] = (uint64_t)(uintptr_t)out;
    guest_call(c, TIME_MGR_GET);
    uint64_t h = out[0];
    if (!h) { LOG("[script] time: no time manager\n"); return; }
    c->x[0] = h;
    guest_call(c, TIME_MGR_OF);
    uint64_t p = c->x[0];
    c->x[0] = (uint64_t)(uintptr_t)out;
    guest_call(c, TIME_HANDLE_FREE);
    uint64_t tm = p ? *(const uint64_t *)(uintptr_t)p : 0, clock = tm ? *(const uint64_t *)(uintptr_t)(tm + 0x40) : 0;
    if (!clock) { LOG("[script] time: no clock\n"); return; }
    for (int o = 0x5c; o <= 0x70; o += 4) {  // (the story's holds on the clock let go - else it puts its own time back)
        float *hold = (float *)(uintptr_t)(clock + (uint64_t)o);
        if (*hold >= 0.0f) LOG("[script] time: the clock's hold +%x (%.2f h) let go\n", o, *hold / 3600.0f), *hold = -1.0f;
    }
    float sec = g_time_hour * 3600.0f;
    uint32_t bits;
    memcpy(&bits, &sec, 4);
    c->x[0] = clock, c->x[1] = 1, c->x[2] = 6, c->v[0] = bits;
    guest_call(c, CLOCK_SET);
    LOG("[script] time of day set to %.2f h (clock %llx)\n", g_time_hour, (unsigned long long)clock);
}

// what the jobs cost (render thread: the stats line)
void za_script_stats(uint64_t *runs, uint64_t *ns) { *runs = g_runs, *ns = g_ns; }
