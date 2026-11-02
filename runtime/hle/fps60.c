// 60 fps (experimental): the world and the menus at 60, battles and captures as the game made them (30).
//
// The game keeps a table of frame durations G (main+0xf7eeb8: 16.67 ms, 33.33 ms, 50 ms, ...; read through the pointer
// at main+0x15fb438). The world's update step comes from G[1] through the accessor f_7000039600; menus, battles and
// captures count their own frames, so at 60 fps they run twice as fast (letsgo-recomp's docs/PESQUISA-60FPS.md). A community "60 FPS"
// patch changes G[1] for every reader; here:
//  - the animation systems take "elapsed time" as a fixed 33.33 ms per update and divide it by G[1] to get frames: the
//    time the accessor hands out is that elapsed time, so with 16.67 ms there they advance half a frame per update at
//    60 fps - right, by time (menus, the world's animations);
//  - battles and captures also count integer frames of their own: they keep the game's 33.33 ms and 30 fps (the entry
//    points of both scenes, mapped in letsgo-recomp's docs/MODO-BATALHA.md, switch to it until the world's update runs again).
// Scenes move from the 30 fps side to the 60 fps side one at a time, once their frame counters are fixed.
//
// LARECOMP_FPS60=1 turns it on (settings: fps = 60). LARECOMP_G1_READERS="<addr>[,<addr>...]" (research): G[1] is
// 16.67 ms only while those functions run, to find which readers a speed comes from.
#include <stdatomic.h>
#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "../kernel.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));  // runtime/fnhook.c
void rt_call_original(Cpu *c, uint64_t addr);
uint64_t renderer_game_frame(void);

#define G1_ADDR 0x7000f7eec0ull  // G[1]: 33333334 ns
#define G1_ORIGINAL 33333334u
#define G1_60HZ 16666667u
#define FIELD_UPDATE 0x70002f8bb0ull  // the world's update, every frame while it runs
#define G_ACCESSOR 0x7000039600ull    // G[index]: the world's step comes through here

static bool g_on;
static bool g_world = true;             // 60 fps now (not inside a battle or a capture)
static bool g_all60;                    // LARECOMP_FPS60=2
static bool g_special;                  // a battle or a capture started and the world hasn't updated since
#define ENCOUNTER_SCENE 0x700028ef70ull  // creates the capture scene (x0 = the encounter object)
#define BATTLE_START 0x700028bb60ull     // copies the battle's configuration

static long g_scene_start = -1;  // research: the present a capture began in (LARECOMP_SHOT_REL)
long fps60_scene_start(void) { return g_scene_start; }
static int scene_start_hook(Cpu *c, uint64_t addr) {
    if (g_scene_start < 0) {
        g_scene_start = (long)renderer_game_frame();
        LOG("[fps60] scene start %ld (f_%llx)\n", g_scene_start, (unsigned long long)addr);
    }
    (void)c, (void)addr;
    return 0;
}

static bool g_scene;  // inside a battle or a capture (until the world updates again)

// The frame counters a scene's timeline runs on (tools/patches.py) step on every other update at 60 fps: 1 = step now.
// Research (LARECOMP_FPS60_STATS=1): once a second, what the game did: presents, world updates (FIELD_UPDATE), reads of G through
// the accessor, and how many times the scene counters (tools/patches.py) were offered a step / allowed to take it.
static bool g_stats;
static unsigned long g_n_world, g_n_acc, g_n_tick_ask, g_n_tick_do;
int rt_scene_tick(void) {
    int r = !(g_all60 && g_scene && (renderer_game_frame() & 1));
    g_n_tick_ask++, g_n_tick_do += r;
    return r;
}

// Every battle / capture and the world's return after it, in the log ("[scene]"; letsgo-recomp's tools/capture_loop.py reads them) and for
// the scripted input (LARECOMP_INPUT_LOOP holds still for a while after the world comes back: input.c)
static bool g_mark_scene;
static long g_world_back = -1;
long fps60_world_back(void) { return g_world_back; } bool fps60_in_scene(void) { return g_mark_scene; }
static int scene_hook(Cpu *c, uint64_t addr) {
    scene_start_hook(c, addr);  // (one hook per address: this one also marks the start for LARECOMP_SHOT_REL)
    if (!g_mark_scene) { g_mark_scene = true; LOG("[scene] start %llu (f_%llx)\n", (unsigned long long)renderer_game_frame(), (unsigned long long)addr); }
    g_scene = true;
    if (g_all60) return 0;  // LARECOMP_FPS60=2: battles and captures at 60 too (their counters halved)
    g_special = true;
    (void)c, (void)addr;
    return 0;
}

// A battle the game starts without BATTLE_START: the wild battles mode builds it through the static battles' process
// (runtime/hle/wildbattle.c), which never calls it. Without this it counted as a menu and got in-between images.
void fps60_scene_begin(void) {
    g_scene = true;
    if (!g_all60) g_special = true;
}

static bool g_field_ran;  // the world's update ran since the last present
static bool g_played;     // the world has updated at least once (before: the controller screen, the title, loading)
static int field_hook(Cpu *c, uint64_t addr) {
    g_n_world++;
    if (g_mark_scene) { g_mark_scene = false; g_world_back = (long)renderer_game_frame(); LOG("[scene] world again %ld\n", g_world_back); }
    g_field_ran = true;
    g_played = true;
    g_special = false;
    g_scene = false;
    (void)c, (void)addr;
    return 0;  // (the game's function runs as usual)
}

// G[1] at 60 Hz for the duration of f: the world's step; any other scene keeps the game's value.
static int accessor_hook(Cpu *c, uint64_t addr) {
    volatile uint32_t *g1 = (volatile uint32_t *)(uintptr_t)G1_ADDR;
    g_n_acc++;
    if (!g_on || !g_world) return 0;
    *g1 = G1_60HZ;
    rt_call_original(c, addr);
    *g1 = G1_ORIGINAL;
    return 1;
}

static int g1_reader_hook(Cpu *c, uint64_t addr) {
    volatile uint32_t *g1 = (volatile uint32_t *)(uintptr_t)G1_ADDR;
    *g1 = G1_60HZ;
    rt_call_original(c, addr);
    *g1 = G1_ORIGINAL;
    return 1;
}

// Research (LARECOMP_FPS60_LOG=1): the frame-rate setting object the animation systems index G with (main+0x15fab08
// holds a pointer to a pointer to it; +0x88 is the index), once a second.
static void log_index(void) {
    static uint64_t last;
    uint64_t f = renderer_game_frame();
    if (f - last < 60) return;
    last = f;
    const uint64_t *slot = (const uint64_t *)(uintptr_t)(0x7000000000ull + 0x15fab08);
    const uint64_t *obj = (const uint64_t *)(uintptr_t)*slot;
    if (!obj || (uint64_t)(uintptr_t)obj < 0x7000000000ull) return;
    const uint64_t *real = (const uint64_t *)(uintptr_t)*obj;
    if (!real || (uint64_t)(uintptr_t)real < 0x7000000000ull) return;
    LOG("[fps60] present %llu: frame-rate index %d (G[idx] = %u)\n", (unsigned long long)f, *(const int32_t *)((const uint8_t *)real + 0x88),
        *(const uint32_t *)(uintptr_t)(0x7000f7eeb8ull + 8ull * (uint32_t)*(const int32_t *)((const uint8_t *)real + 0x88)));
}

// Research (LARECOMP_ELAPSED_LOG=1): the elapsed time the animation functions f_7000663660 / f_7000664cd0 divide by
// G[index]: the object their parameter holds, asked as they do (its first virtual method, argument 1), field +8.
// LARECOMP_ELAPSED_LOG="<addr>:<which>[,...]" names other readers and the argument they pass (f_7000029d30, the frame
// loop: 0; f_700067be50: 1); LARECOMP_TIME_PROBE_FRAMES limits it to some presents.
static uint64_t g_el_fn[16];
static int g_el_arg[16], g_nel;
static long g_tp_from, g_tp_to = 0x7fffffffL;
static int elapsed_hook(Cpu *c, uint64_t addr) {
    static int n;
    long f = (long)renderer_game_frame();
    if (f < g_tp_from || f > g_tp_to) return 0;
    int which = 1;
    for (int i = 0; i < g_nel; i++) if (g_el_fn[i] == addr) which = g_el_arg[i];
    uint64_t saved[32], pc_arg = c->x[1];
    memcpy(saved, c->x, sizeof saved);
    uint64_t obj = pc_arg ? *(uint64_t *)(uintptr_t)pc_arg : 0;
    if (obj >= 0x7000000000ull && n < 400) {
        uint64_t vt = *(uint64_t *)(uintptr_t)obj, fn = vt >= 0x7000000000ull ? *(uint64_t *)(uintptr_t)vt : 0;
        if (fn >= 0x7000000000ull) {
            c->x[0] = obj;
            c->x[1] = (uint64_t)which;
            guest_call(c, fn);
            uint64_t r = c->x[0];
            if (r >= 0x7000000000ull)
                LOG("[elapsed] f_%llx present %llu: delta %lld ns (timer %d: object %llx, getter f_%llx, step %llx)\n", (unsigned long long)addr,
                    (unsigned long long)renderer_game_frame(), (long long)*(const int64_t *)(uintptr_t)(r + 8), which,
                    (unsigned long long)obj, (unsigned long long)fn, (unsigned long long)r);
            n++;
        }
    }
    memcpy(c->x, saved, sizeof saved);
    return 0;
}

bool fps60_enabled(void) { return g_on; }
bool fps60_world(void) { return g_world; }
// A menu (the Pokédex, the bag...): the world stopped and no battle or capture runs. Logic at 30, shown at 60 with the
// in-between image (runtime/nvn/vulkan/exec.c); battles and captures get none (tried, left out: letsgo-recomp's docs/HISTORICO.md 46).
bool fps60_menu(void) { return g_on && g_played && !g_world && !g_special; }

// At each present (before its pacing): was this a world frame?
void fps60_probe_present(void);
int rt_present_interval(void);
void fps60_present(void) {
    fps60_probe_present();
    if (g_stats) {
        static uint64_t t0, p0;
        extern uint64_t plat_now_ns(void);
        uint64_t now = plat_now_ns(), p = renderer_game_frame();
        if (!t0) t0 = now, p0 = p;
        if (now - t0 >= 1000000000ull) {
            LOG("[stats] t+%.2fs presents %llu world %lu acc %lu tick_ask %lu tick_do %lu scene %d interval %d G1 %u\n", (now - t0) / 1e9,
                (unsigned long long)(p - p0), g_n_world, g_n_acc, g_n_tick_ask, g_n_tick_do, g_scene, rt_present_interval(), *(volatile uint32_t *)(uintptr_t)G1_ADDR);
            t0 = now, p0 = p, g_n_world = g_n_acc = g_n_tick_ask = g_n_tick_do = 0;
        }
    }
    if (getenv("LARECOMP_FPS60_LOG")) log_index();
    if (!g_on) return;
    // 60 only while the world itself updates: menus, the Pokédex and other screens that stop it count their own frames
    // and ran twice as fast (measured: the menu opened in 4 game frames instead of 8)
    static int menu_native = -1;  // (research, LARECOMP_FPS60_MENU_NATIVE=1: menus at 60 natively, as before HISTORICO 47)
    if (menu_native < 0) menu_native = getenv("LARECOMP_FPS60_MENU_NATIVE") != NULL;
    bool w = !g_special && (g_field_ran || menu_native);
    g_field_ran = false;
    g_world = w;
    static int last = -1;  // 0 world, 1 menu (in-between images), 2 battle / capture
    int state = w ? 0 : fps60_menu() ? 1 : 2;
    static const char *const name[] = {"world: 60 fps", "menu: 30 fps with in-between images", "battle / capture: 30 fps"};
    if (state != last) LOG("[fps60] %s from present %llu\n", name[state], (unsigned long long)renderer_game_frame());
    last = state;
}

// Before each boot (runtime/main.c): the launcher's "60 fps (experimental)" (larecomp.ini fps = 60) turns the mode on
// when LARECOMP_FPS60 doesn't say otherwise.
void fps60_configure(void) {
    static bool hooked;
    extern void settings_load(void);
    settings_load();  // (read once; without the launcher it would come later, in boot)
    if (!getenv("LARECOMP_FPS60")) g_on = g_settings.fps == 60, g_all60 = false;
    extern uint64_t rt_addr(uint64_t eevee);  // (runtime/fnhook.c: 0 in a build it doesn't know)
    if (!rt_addr(FIELD_UPDATE)) g_on = g_all60 = false;
    if ((g_on || g_stats || getenv("LARECOMP_INPUT_LOOP")) && !hooked) {
        hooked = true;
        rt_register_fnhook(FIELD_UPDATE, field_hook);
        rt_register_fnhook(ENCOUNTER_SCENE, scene_hook);
        rt_register_fnhook(BATTLE_START, scene_hook);
        rt_register_fnhook(G_ACCESSOR, accessor_hook);
    }
    g_world = true, g_special = g_scene = g_played = false;
    if (g_on) LOG("[fps60] on: the world at 60, menus at 30 with in-between images, battles and captures at 30\n");
}

__attribute__((constructor)) static void fps60_init(void) {
    const char *e = getenv("LARECOMP_FPS60");
    g_on = e && atoi(e);
    g_all60 = e && atoi(e) == 2;
    g_stats = getenv("LARECOMP_FPS60_STATS") != NULL;  // (the hooks: fps60_configure)
    if (getenv("LARECOMP_ELAPSED_LOG")) {
        const char *fr = getenv("LARECOMP_TIME_PROBE_FRAMES");
        if (fr) { g_tp_from = atol(fr); g_tp_to = strchr(fr, '-') ? atol(strchr(fr, '-') + 1) : g_tp_from; }
        char lb[512];
        snprintf(lb, sizeof lb, "%s", getenv("LARECOMP_ELAPSED_LOG"));
        if (strchr(lb, ':'))
            for (char *t = strtok(lb, ","); t && g_nel < 16; t = strtok(NULL, ",")) {
                g_el_fn[g_nel] = strtoull(t, NULL, 16);
                g_el_arg[g_nel] = strchr(t, ':') ? atoi(strchr(t, ':') + 1) : 1;
                rt_register_fnhook(g_el_fn[g_nel++], elapsed_hook);
            }
        else {
            rt_register_fnhook(0x7000663660ull, elapsed_hook);
            rt_register_fnhook(0x7000664cd0ull, elapsed_hook);
        }
    }
    if (!g_on && (getenv("LARECOMP_SHOT_REL") || getenv("LARECOMP_WINDOW_SHOTS_REL"))) { rt_register_fnhook(ENCOUNTER_SCENE, scene_start_hook); rt_register_fnhook(BATTLE_START, scene_start_hook); }
    e = getenv("LARECOMP_G1_READERS");
    char buf[512];
    if (!e) return;
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) rt_register_fnhook(strtoull(t, NULL, 16), g1_reader_hook);
}

// ---- research: LARECOMP_TIME_PROBE="<fn addr>[,...]" (+ LARECOMP_TIME_PROBE_FRAMES=<first>-<last>) ------------------
// The readers of the frame-duration table G take an elapsed-time object in x1 (its +8: nanoseconds): log, per call, the
// present, x1's elapsed time and what the function returned (x0, and d0 as a double), up to 600 lines.
static int time_probe_hook(Cpu *c, uint64_t addr) {
    static int lines;
    uint64_t x1 = c->x[1];
    long f = (long)renderer_game_frame();
    int64_t el = x1 >= 0x7000000000ull && x1 < 0x7800000000ull ? *(const int64_t *)(uintptr_t)(x1 + 8) : -1;
    rt_call_original(c, addr);
    if (f >= g_tp_from && f <= g_tp_to && lines < 600) {
        double d0;
        memcpy(&d0, &c->v[0], 8);
        LOG("[time] present %ld f_%llx elapsed %lld ns -> x0 %llx d0 %g\n", f, (unsigned long long)addr, (long long)el,
            (unsigned long long)c->x[0], d0);
        lines++;
    }
    return 1;
}
__attribute__((constructor)) static void time_probe_init(void) {
    const char *e = getenv("LARECOMP_TIME_PROBE");
    if (!e) return;
    const char *fr = getenv("LARECOMP_TIME_PROBE_FRAMES");
    if (fr) { g_tp_from = atol(fr); g_tp_to = strchr(fr, '-') ? atol(strchr(fr, '-') + 1) : g_tp_from; }
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) rt_register_fnhook(strtoull(t, NULL, 16), time_probe_hook);
}

// ---- research: LARECOMP_TIMER_USERS=<first>-<last> -------------------------------------------------------------
// Who reads the frame-step timers: calls to their getter f_70000395f0 (x1 = the timer, the caller's lr), counted over
// those presents and listed at the last one. Timer 1 is the step the accessor f_7000039600 rewrites every frame with
// G[1]; timer 0 keeps 33.3 ms.
#define TIMER_GETTER 0x70000395f0ull
static struct { uint64_t lr; int which; uint64_t n; } g_tu[256];
static int g_ntu;
static long g_tu_from, g_tu_to;
static int timer_users_hook(Cpu *c, uint64_t addr) {
    long f = (long)renderer_game_frame();
    if (f >= g_tu_from && f <= g_tu_to) {
        uint64_t lr = c->x[30];
        int which = (int)c->x[1], i;
        for (i = 0; i < g_ntu && !(g_tu[i].lr == lr && g_tu[i].which == which); i++) {}
        if (i == g_ntu && g_ntu < 256) g_tu[g_ntu++] = (__typeof__(g_tu[0])){lr, which, 0};
        if (i < 256) g_tu[i].n++;
    } else if (f > g_tu_to && g_ntu) {
        for (int i = 0; i < g_ntu; i++)
            LOG("[timers] timer %d read from lr %llx: %llu times in %ld presents\n", g_tu[i].which, (unsigned long long)g_tu[i].lr,
                (unsigned long long)g_tu[i].n, g_tu_to - g_tu_from + 1);
        g_ntu = 0;
        g_tu_to = -1;
    }
    (void)addr;
    return 0;
}
__attribute__((constructor)) static void timer_users_init(void) {
    const char *e = getenv("LARECOMP_TIMER_USERS");
    if (!e) return;
    g_tu_from = atol(e);
    g_tu_to = strchr(e, '-') ? atol(strchr(e, '-') + 1) : g_tu_from;
    rt_register_fnhook(TIMER_GETTER, timer_users_hook);
}

// ---- research: LARECOMP_CALLCOUNT="<fn>[,...]" (+ LARECOMP_TIME_PROBE_FRAMES): calls per present of each function
#define CC_MAX 8
static uint64_t g_cc_fn[CC_MAX];
static unsigned g_cc_n[CC_MAX];
static int g_ncc;
static long g_cc_frame = -1;
static int callcount_hook(Cpu *c, uint64_t addr) {
    long f = (long)renderer_game_frame();
    if (f != g_cc_frame) {
        if (g_cc_frame >= g_tp_from && g_cc_frame <= g_tp_to) {
            char line[256];
            int o = snprintf(line, sizeof line, "[calls] present %ld:", g_cc_frame);
            for (int i = 0; i < g_ncc; i++) o += snprintf(line + o, sizeof line - o, " f_%llx %u", (unsigned long long)g_cc_fn[i], g_cc_n[i]);
            LOG("%s\n", line);
        }
        memset(g_cc_n, 0, sizeof g_cc_n);
        g_cc_frame = f;
    }
    for (int i = 0; i < g_ncc; i++) if (g_cc_fn[i] == addr) g_cc_n[i]++;
    (void)c;
    return 0;
}
__attribute__((constructor)) static void callcount_init(void) {
    const char *e = getenv("LARECOMP_CALLCOUNT");
    if (!e) return;
    const char *fr = getenv("LARECOMP_TIME_PROBE_FRAMES");
    if (fr) { g_tp_from = atol(fr); g_tp_to = strchr(fr, '-') ? atol(strchr(fr, '-') + 1) : g_tp_from; }
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t && g_ncc < CC_MAX; t = strtok(NULL, ",")) {
        g_cc_fn[g_ncc++] = strtoull(t, NULL, 16);
        rt_register_fnhook(g_cc_fn[g_ncc - 1], callcount_hook);
    }
}

// ---- research: LARECOMP_PROBE="<fn addr>[:<bytes>]" ----------------------------------------------------------------
// At each entry of the function, remember its x0 (object) and, after the original returns, x0 (result): for the next
// LARECOMP_PROBE_FRAMES (120) presents, compare `bytes` (0x400) of each of them with the previous present and log, at
// the end, every 32-bit word that changed: how many presents, and its usual step as an integer and as a float. A
// frame counter shows as an integer stepping by 1 (or a float by a constant) every present of the scene.
#define PROBE_MAX 4
static struct { uint64_t base; const char *what; uint32_t prev[0x400 / 4]; uint32_t changed[0x400 / 4]; int32_t step[0x400 / 4]; uint32_t step_n[0x400 / 4]; bool have; } g_probe[PROBE_MAX];
static int g_nprobe;
static uint32_t g_probe_bytes = 0x400;
static uint64_t g_probe_from, g_probe_to;
static int probe_hook(Cpu *c, uint64_t addr) {
    uint64_t obj = c->x[0];
    rt_call_original(c, addr);
    uint64_t ret = c->x[0];
    if (!g_probe_from) g_probe_from = renderer_game_frame() + 1, g_probe_to = g_probe_from + (getenv("LARECOMP_PROBE_FRAMES") ? atoi(getenv("LARECOMP_PROBE_FRAMES")) : 120);
    for (int pass = 0; pass < 2 && g_nprobe < PROBE_MAX; pass++) {
        uint64_t p = pass ? ret : obj;
        if (p < 0x7000000000ull || p > 0x7800000000ull) continue;
        g_probe[g_nprobe].base = p, g_probe[g_nprobe].what = pass ? "returned object" : "x0 object";
        LOG("[probe] f_%llx %s %llx\n", (unsigned long long)addr, g_probe[g_nprobe].what, (unsigned long long)p);
        g_nprobe++;
    }
    return 1;
}
void fps60_probe_present(void) {
    uint64_t f = renderer_game_frame();
    if (!g_probe_from || f < g_probe_from || f > g_probe_to) return;
    for (int i = 0; i < g_nprobe; i++) {
        const uint32_t *now = (const uint32_t *)(uintptr_t)g_probe[i].base;
        for (uint32_t w = 0; w < g_probe_bytes / 4; w++) {
            if (g_probe[i].have && now[w] != g_probe[i].prev[w]) {
                g_probe[i].changed[w]++;
                int32_t d = (int32_t)(now[w] - g_probe[i].prev[w]);
                if (d == g_probe[i].step[w]) g_probe[i].step_n[w]++;
                else if (g_probe[i].step_n[w] == 0) g_probe[i].step[w] = d;
            }
            g_probe[i].prev[w] = now[w];
        }
        g_probe[i].have = true;
    }
    if (f == g_probe_to) {
        for (int i = 0; i < g_nprobe; i++)
            for (uint32_t w = 0; w < g_probe_bytes / 4; w++) {
                if (g_probe[i].changed[w] < 5) continue;
                float fv;
                memcpy(&fv, &g_probe[i].prev[w], 4);
                LOG("[probe] %s +0x%03x: changed in %u of %llu presents; usual step %d (%u times) value %u / %g\n", g_probe[i].what, w * 4,
                    g_probe[i].changed[w], (unsigned long long)(g_probe_to - g_probe_from), g_probe[i].step[w], g_probe[i].step_n[w],
                    g_probe[i].prev[w], fv);
            }
    }
}
__attribute__((constructor)) static void probe_init(void) {
    const char *e = getenv("LARECOMP_PROBE");
    if (!e) return;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", e);
    char *colon = strchr(buf, ':');
    if (colon) *colon = 0, g_probe_bytes = (uint32_t)strtoul(colon + 1, NULL, 16);
    if (g_probe_bytes > 0x400) g_probe_bytes = 0x400;
    rt_register_fnhook(strtoull(buf, NULL, 16), probe_hook);
}

// ---- The Legends games at 60 fps (experimental), the way the community "60 FPS" cheat does it for Legends: Arceus:
// both step their world by a frame time looked up by present interval in a table of u64 nanoseconds (16666667,
// 33333334, 50000000... for intervals 1, 2, 3...) and present every other vsync. With the interval-2 entry at 16666667
// and presents paced at 60 (nvn/objects.c present_pace) the world runs at its own speed - a 10 s walk from the save
// ends at the same spot at 30 and at 60 fps. The launcher's 60 fps option, or LARECOMP_PLA_60FPS=1 (0: off).
#include "../platform.h"
static const struct { uint8_t id[8]; uint64_t entry; const char *name; } k_legends60[] = {
    {{0xae, 0xe8, 0xf1, 0x50, 0xdd, 0xa1, 0xb5, 0xa8}, 0x7000000000ull + 0x397E8D8, "Legends: Arceus 1.1.1"},
    {{0xb1, 0xf1, 0x2f, 0xd9, 0x19, 0xea, 0xe8, 0x6a}, 0x7000000000ull + 0x3393E28, "Legends Z-A"},
};
static uint64_t g_pla60_entry;  // the interval-2 frame time of the running build (0: off)
static uint64_t g_pla60_build;  // (that entry of the running build, also at 30)
static bool g_pla60_patched;
static uint64_t g_pla60_ns = 16666667;  // the frame time: 60 fps, 120, 144, FPS_UNLIMITED's, or LARECOMP_LEGENDS_FPS=<n>
// This update's step: the period, or with real-time steps (pla60_present) the time the last frame really took; and the
// world's time so far in those steps, the last present's and this one's (rt_pla_tick)
static uint64_t g_pla60_dt = 16666667, g_world_prev, g_world_now;
bool pla60_enabled(void) { return g_pla60_entry != 0; }
uint64_t pla60_period_ns(void) { return g_pla60_ns; }

// Legends: Arceus mouse look. The free camera's input turn (f_700193d16c, x0 = the camera) adds the stick times its turn
// rate to the yaw it keeps (+0x360, radians) and the pitch (+0x700, a double 0..1), then sets them. Before it runs, the
// held mouse's motion is added to both: the camera turns by as much as the mouse moved, with no stick curve or top
// speed; a controller's stick still turns it the game's way. The pitch's 0..1 is taken as the yaw's radians scaled by
// the two turn speeds (+0x708 over +0x420: the stick turns both alike). LARECOMP_PLA_MOUSE_LOOK=0: the mouse is the
// camera stick instead (as on other cameras: menus, cutscenes, aiming).
static bool g_pla_mouse_look;
static int pla_mouse_look_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    extern uint64_t g_field_camera_frame, renderer_game_frame(void);  // (runtime/hle/menu_mouse.c: not in a menu)
    g_field_camera_frame = renderer_game_frame();
    static int camlog = -1;  // (research, LARECOMP_PLA_CAM_LOG=<every n>: the camera's yaw and pitch, the caller)
    if (camlog < 0) camlog = getenv("LARECOMP_PLA_CAM_LOG") ? atoi(getenv("LARECOMP_PLA_CAM_LOG")) : 0;
    if (camlog > 0) {
        static unsigned n;
        const uint8_t *cm = (const uint8_t *)(uintptr_t)c->x[0];
        extern uint64_t renderer_present_count(void);
        if (n++ % (unsigned)camlog == 0 && n < 4000)
            LOG("[cam] %llx from %llx present %llu yaw %g pitch %g\n", (unsigned long long)c->x[0], (unsigned long long)c->x[30],
                (unsigned long long)renderer_present_count(), *(const float *)(cm + 0x360), *(const double *)(cm + 0x700));
    }
    if (!g_pla_mouse_look) return 0;
    extern bool input_mouse_camera_take(float *dyaw, float *dpitch);
    float dy, dp;
    if (!input_mouse_camera_take(&dy, &dp)) return 0;
    uint8_t *cam = (uint8_t *)(uintptr_t)c->x[0];
    *(float *)(cam + 0x360) += dy;
    float ky = *(float *)(cam + 0x420), kp = *(float *)(cam + 0x708);
    double *pitch = (double *)(cam + 0x700), v = *pitch + dp * (ky > 0 && kp > 0 ? kp / ky : 0.45);
    *pitch = v < 0 ? 0 : v > 1 ? 1 : v;
    return 0;
}

// The event scripts (Lua coroutines, compiled from Haxe: bin/haxe/release/...blua) count their waits in updates - each
// update resumes each running script once (f_70015ba460: one step, returns 1 while it still runs). At 60 fps every
// wait ended twice as early: people in cutscenes vanished mid-walk, Volo's Poke Ball was put away while still in his
// hand, shots cut early. Above 30 fps a script steps only on the updates where 30 fps time moved on a frame
// (rt_pla_tick), so scripts keep the game's 30 steps a second; the world they move is at the frame rate as before.
// LARECOMP_PLA_SCRIPT_TICK=0: every update (as the game).
#define PLA_SCRIPT_STEP 0x70015ba460ull
int rt_pla_tick(int site);
// The buttons go with it (input.c fill_state: pla_input_tick): a script sees a press on the update it lands on, so a
// press on an update without a script step was lost (dialogue that wouldn't advance). Above 30 fps the buttons the game
// reads change only on the updates scripts step on (a press waits at most one update); the sticks are read as ever.
static int g_pla_script_tick = -1;
static bool pla_script_tick_on(void) {  // (off by default: at 144 fps a battle's result, there for one update, was missed and
                                        // Volo's battle started over and over; LARECOMP_PLA_SCRIPT_TICK=1: on, research)
    if (g_pla_script_tick < 0) g_pla_script_tick = getenv("LARECOMP_PLA_SCRIPT_TICK") && atoi(getenv("LARECOMP_PLA_SCRIPT_TICK")) != 0;
    return g_pla_script_tick && g_pla60_build;
}
int pla_input_tick(void) { return !pla_script_tick_on() || rt_pla_tick(0xa461); }
static int pla_script_step_hook(Cpu *c, uint64_t addr) {
    if (!pla_script_tick_on() || rt_pla_tick(0xa460)) return 0;  // (the game's own step)
    uint64_t state = *(const uint64_t *)(uintptr_t)(c->x[0] + 0x140);
    c->x[0] = state && *(const uint8_t *)(uintptr_t)(state + 0x191) == 0;  // (still running: what a step returns)
    (void)addr;
    return 1;
}

// The event scripts' timed commands (WaitFrame, AfterWaitFrame...: 15 of them) count down by the frames the update is
// worth to their script context (f_7001059a78, [ctx+0x88]): at 60 fps their waits ended twice as early (Volo's
// challenge shot - his Poke Ball - cut short). They get the frame only on the updates where 30 fps time moved on one.
// LARECOMP_PLA_WAIT_TICK=0: off.
#define PLA_SCRIPT_FRAMES 0x7001059a78ull
static int pla_script_frames_hook(Cpu *c, uint64_t addr) {
    if (rt_pla_tick(0x9a78)) return 0;
    c->x[0] = 0;
    (void)addr;
    return 1;
}

// Research (LARECOMP_PLA_BALL_LOG=1): a thrown object's flight step (f_7001a7191c state 1, at its velocity read
// f_7002adbce0 from 0x7001a71b98): the frames it was given ([sp]), its drag (s8) and lift (s9), the velocity read.
static int pla_ball_log_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != 0x7001a71b9cull) return 0;
    float fr, s8, s9;
    memcpy(&fr, (const void *)(uintptr_t)c->sp, 4), memcpy(&s8, &c->v[8], 4), memcpy(&s9, &c->v[9], 4);
    rt_call_original(c, addr);
    float v[4];
    memcpy(v, &c->v[0], 16);
    static int n;
    extern uint64_t renderer_present_count(void);
    if (n++ < 400) LOG("[ball] present %llu frames %g drag %g lift %g velocity %g %g %g\n", (unsigned long long)renderer_present_count(), fr, s8, s9, v[0], v[1], v[2]);
    return 1;
}

// (and, with it, the clock object f_7001a7236c hands that step: its vtable and the frames getter at +0x30)
static int pla_ball_clock_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != 0x7001a71a28ull) return 0;
    uint64_t out = c->x[8];
    rt_call_original(c, addr);
    uint64_t o = *(const uint64_t *)(uintptr_t)out, vt = o ? *(const uint64_t *)(uintptr_t)o : 0;
    static int n;
    if (n++ < 3) LOG("[ball] clock %llx vtable %llx frames getter %llx\n", (unsigned long long)o, (unsigned long long)vt,
                     vt ? (unsigned long long)*(const uint64_t *)(uintptr_t)(vt + 0x30) : 0ull);
    return 1;
}

// Research (LARECOMP_PLA_FLICK_LOG=1): a wild Pokémon's "flick the ball away" check (f_7001aa085c, x0 = its component):
// each change of its answer is logged with the flags it reads and the frame.
static int pla_flick_log_hook(Cpu *c, uint64_t addr) {
    uint64_t o = c->x[0];
    const uint8_t *m = (const uint8_t *)(uintptr_t)o;
    rt_call_original(c, addr);
    static uint64_t last_o; static int last_r = -1;
    int r = (int)(c->x[0] & 1);
    if (o != last_o || r != last_r) {
        extern uint64_t renderer_present_count(void);
        LOG("[flick] %llx -> %d at present %llu (8d9 %u 821 %u 81d %u 956 %u a90 %u | 8b0 %d 8b4 %d)\n", (unsigned long long)o, r,
            (unsigned long long)renderer_present_count(), m[0x8d9], m[0x821], m[0x81d], m[0x956], m[0xa90],
            *(const int *)(m + 0x8b0), *(const int *)(m + 0x8b4));
        last_o = o, last_r = r;
    }
    return 1;
}

// Research (LARECOMP_PLA_FLICK_LOG=1, with the above): the hit events reaching a wild Pokémon (f_7001911760, x2 = the
// event, its type at +0xf0; 5 = a ball's hit, which runs the capture checks)
static int pla_hit_event_hook(Cpu *c, uint64_t addr) {
    uint64_t ev = c->x[2];
    int type = ev ? *(const int *)(uintptr_t)(ev + 0xf0) : -1;
    static int n;
    if (n++ < 400) {
        extern uint64_t renderer_present_count(void);
        LOG("[hitev] obj %llx event %llx type %d at present %llu (event vtable %llx, obj vtable %llx)\n", (unsigned long long)c->x[1],
            (unsigned long long)ev, type, (unsigned long long)renderer_present_count(),
            ev ? (unsigned long long)*(const uint64_t *)(uintptr_t)ev : 0ull,
            c->x[1] ? (unsigned long long)*(const uint64_t *)(uintptr_t)c->x[1] : 0ull);
    }
    (void)addr;
    return 0;
}

// A character's scripted move (f_70019b2140, x0 = its navigator, each update while [+0x3e4] == 1): the progress +0x4bc
// goes up by the step +0x4b4 an update - no frame time - and the move ends at 1 (state 0, the script's "arrived" check
// f_70019b24dc turns false). At 60 fps moves ended twice as early: Volo removed mid-walk after his battle, people in
// cutscenes vanishing before they walk off screen. The step is scaled to the frame time for the update (1 at 30 fps).
#define PLA_SCRIPTED_MOVE 0x70019b2140ull
uint32_t rt_scale30_f32(uint32_t bits);
static int pla_scripted_move_hook(Cpu *c, uint64_t addr) {
    uint64_t nav = c->x[0];
    if (!nav || *(const uint32_t *)(uintptr_t)(nav + 0x3e4) != 1) return 0;
    uint32_t *step = (uint32_t *)(uintptr_t)(nav + 0x4b4), was = *step;
    *step = rt_scale30_f32(was);
    rt_call_original(c, addr);
    *step = was;
    return 1;
}

// Research (LARECOMP_PLA_STATE_WATCH=1): the event scripts' "arrived" check (f_70019b24dc: [obj+0x3e4] == 1) - a
// hardware watchpoint on the first object it's asked about names who writes that state.
static int pla_state_watch_hook(Cpu *c, uint64_t addr) {
    static uint64_t armed;
    if (!armed && c->x[0]) {
        armed = c->x[0];
        void hwwatch_arm(uint64_t addr, uint64_t len);
        LOG("[statewatch] obj %llx state %u\n", (unsigned long long)armed, *(const uint32_t *)(uintptr_t)(armed + 0x3e4));
        hwwatch_arm(armed + 0x3e4, 4);
    }
    (void)addr;
    return 0;
}

// A thrown object's flight (Poke Balls, bait, mud: f_7001a7191c with [+0x5f8] == 1) each update takes the frames the
// update is worth from its clock (the getter at its vtable +0x30, f_70024a8660: 1 an update, as at 30 fps), then adds
// them / 30 to its flight time, lifts it by lift * frames * its speed across and keeps (1 - drag * frames) of its
// velocity (bin/capture/throw_param_table.bin: drag 0.04, lift 0.01). At 144 fps a ball lost 4% of its speed 144 times
// a second - 0.3% left after a second instead of 29% - and dropped short of the Pokemon it was thrown at (and at 60
// fps, of far ones). The frames are scaled to the frame time for the update (1 at 30 fps). LARECOMP_PLA_THROW_SCALE=0:
// off.
#define PLA_THROW_FRAMES 0x70024a8660ull
#define PLA_THROW_STEP_RET 0x7001a71a40ull  // (the flight step's call of it)
static int pla_throw_frames_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != PLA_THROW_STEP_RET) return 0;
    rt_call_original(c, addr);
    c->v[0] = rt_scale30_f32((uint32_t)c->v[0]);
    return 1;
}

// The player's fall time (f_7001713904, s0 = the step: its two callers pass a clock's frames / 30, or 1 / 30 when
// there is no clock) adds the step each update to [+0x2ac] and hands it, in ms, to the animation as PL_FALLTIME: above
// 30 fps falls counted 1/30 s an update (4.8x too fast at 144). The step is scaled to the frame time for the update.
// LARECOMP_PLA_THROW_SCALE=0 turns it off with the throw's.
#define PLA_FALL_TIME 0x7001713904ull
static int pla_fall_time_hook(Cpu *c, uint64_t addr) {
    c->v[0] = rt_scale30_f32((uint32_t)c->v[0]);
    (void)addr;
    return 0;
}

// Research (LARECOMP_PLA_GATE30=<addr>[,<addr>...]): those functions run only on the updates where 30 fps time moved
// on a frame (rt_pla_tick) - which per-update counting a behaviour comes from.
static int pla_gate30_hook(Cpu *c, uint64_t addr) {
    (void)c, (void)addr;
    return !rt_pla_tick(0x6a30);
}

// Research (LARECOMP_PLA_AIDUMP=1 + <live>/aidump): the behaviour tree components seen (f_7000a6bc64's x0, up to 8),
// each with what it points to two levels down, written to <live>/ai_<present>_<n>.bin when the file appears (records:
// u64 address, u32 size, u32 path length, the path ("+off/+off/"), the bytes) - for comparing runs offline.
// Research (LARECOMP_PLA_BT=<addr>,...): the guest call stack (frame-pointer chain) the first 3 times each is called
static uint64_t g_bt_addr[8];
static int g_bt_n[8];
static int pla_bt_hook(Cpu *c, uint64_t addr) {
    for (int i = 0; i < 8; i++) {
        if (g_bt_addr[i] != addr) continue;
        if (g_bt_n[i]++ >= 3) return 0;
        char line[512];
        int n = snprintf(line, sizeof line, "lr %llx x0 %llx:", (unsigned long long)c->x[30], (unsigned long long)c->x[0]);
        uint64_t fp = c->x[29];
        for (int k = 0; k < 14 && fp >= 0x7200000000ull && fp < 0x7400000000ull && !(fp & 7) && n < 480; k++) {
            n += snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)((const uint64_t *)(uintptr_t)fp)[1]);
            fp = ((const uint64_t *)(uintptr_t)fp)[0];
        }
        LOG("[bt] %llx: %s\n", (unsigned long long)addr, line);
    }
    return 0;
}
static uint64_t g_ai_seen[64];
static int pla_ai_watch(uint64_t comp) {  // (the word LARECOMP_PLA_AI_WATCH names in that component, watched)
    uint64_t a = comp;
    char *e = getenv("LARECOMP_PLA_AI_WATCH");
    while (*e) {
        uint64_t off = strtoull(e, &e, 16);
        if (*e == ',') { e++; a = *(const uint64_t *)(uintptr_t)(a + off); if (!a) break; }
        else { a += off; break; }
    }
    if (!a || *e) return 0;
    void hwwatch_arm(uint64_t addr, uint64_t len);
    LOG("[aiwatch] component %llx: watching %llx\n", (unsigned long long)comp, (unsigned long long)a);
    hwwatch_arm(a, 4);
    return 1;
}
static int pla_ai_seen_hook(Cpu *c, uint64_t addr) {
    static int watched;  // (LARECOMP_PLA_AI_WATCH=<off>,...,<last off>: a watchpoint on that word of the first component)
    if (!watched && getenv("LARECOMP_PLA_AI_WATCH") && !getenv("LARECOMP_PLA_AIDUMP") && c->x[0]) watched = pla_ai_watch(c->x[0]);
    for (int i = 0; i < 64 && c->x[0]; i++) {
        if (g_ai_seen[i] == c->x[0]) break;
        if (!g_ai_seen[i]) { g_ai_seen[i] = c->x[0]; break; }
    }
    (void)addr;
    return 0;
}
static void aidump_node(FILE *f, uint64_t a, const char *path, int depth, size_t *budget) {
    uint32_t len = depth ? 0x400 : 0x800;
    if (a < 0x7200000000ull || a >= 0x7400000000ull || (a & 7) || *budget < len) return;
    *budget -= len;
    uint32_t pl = (uint32_t)strlen(path);
    fwrite(&a, 8, 1, f), fwrite(&len, 4, 1, f), fwrite(&pl, 4, 1, f), fwrite(path, 1, pl, f);
    fwrite((const void *)(uintptr_t)a, 1, len, f);
    if (depth >= 2) return;
    for (uint32_t o = 0; o < len; o += 8) {
        uint64_t q = *(const uint64_t *)(uintptr_t)(a + o);
        if (q == a) continue;
        char sub[96];
        snprintf(sub, sizeof sub, "%s+%x/", path, o);
        aidump_node(f, q, sub, depth + 1, budget);
    }
}
void pla_aidump_present(void) {
    static const char *dir;
    static int init;
    if (!init) init = 1, dir = getenv("LARECOMP_PLA_AIDUMP") ? getenv("LARECOMP_LIVE") : NULL;
    if (!dir) return;
    char p[600];
    snprintf(p, sizeof p, "%s/pwatch", dir);
    if (!remove(p)) {  // (<live>/pwatch: a watchpoint on the player's body position x - with LARECOMP_HWWATCH_RW=1 its readers)
        uint64_t warp_player_body(void);
        void hwwatch_arm(uint64_t addr, uint64_t len);
        uint64_t b = warp_player_body();
        LOG("[pwatch] body %llx\n", (unsigned long long)b);
        if (b) hwwatch_arm(b + 0x90, 4);
    }
    snprintf(p, sizeof p, "%s/pfind", dir);
    if (!remove(p)) {  // (<live>/pfind: every copy of the player's position in the heap - 3 floats within 1e-3)
        bool warp_player_pos(float out[3]);
        float q[3];
        extern uint64_t g_stdheap_used;
        uint64_t top = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used - HEAP_BASE : 0;
        uint64_t words = (kernel_heap_size() > top ? kernel_heap_size() : top) / 4;
        const float *h = (const float *)(uintptr_t)HEAP_BASE;
        int hits = 0;
        if (warp_player_pos(q))
            for (uint64_t i = 0; i + 2 < words && hits < 100; i++)
                if (fabsf(h[i] - q[0]) < 1e-3f && fabsf(h[i + 1] - q[1]) < 1e-3f && fabsf(h[i + 2] - q[2]) < 1e-3f)
                    LOG("[pfind] %llx\n", (unsigned long long)(HEAP_BASE + i * 4)), hits++;
        LOG("[pfind] %.3f %.3f %.3f: %d copies\n", q[0], q[1], q[2], hits);
    }
    snprintf(p, sizeof p, "%s/rwatch", dir);
    FILE *rf = fopen(p, "r");
    if (rf) {  // (<live>/rwatch: hex addresses, each watched)
        void hwwatch_arm(uint64_t addr, uint64_t len);
        unsigned long long a;
        while (fscanf(rf, "%llx", &a) == 1) { LOG("[rwatch] %llx\n", a); hwwatch_arm(a, 4); }
        fclose(rf), remove(p);
    }
    snprintf(p, sizeof p, "%s/aiwatch", dir);
    bool watch = getenv("LARECOMP_PLA_AI_WATCH") && !remove(p);  // (<live>/aiwatch: the watch armed in the near ones)
    snprintf(p, sizeof p, "%s/aidump", dir);
    if (!watch && remove(p)) return;
    extern uint64_t renderer_present_count(void);
    float nx = 0, nz = 0, nr = -1;  // (LARECOMP_PLA_AIDUMP=<x>,<z>,<r>: only those with a position that close one level down)
    sscanf(getenv("LARECOMP_PLA_AIDUMP"), "%f,%f,%f", &nx, &nz, &nr);
    for (int i = 0; i < 64 && g_ai_seen[i]; i++) {
        if (nr > 0) {
            bool near = false;
            const uint64_t *top = (const uint64_t *)(uintptr_t)g_ai_seen[i];
            for (int k = 0; k < 0x100 && !near; k++) {
                uint64_t q = top[k];
                if (q < 0x7200000000ull || q >= 0x7400000000ull || (q & 3)) continue;
                const float *fv = (const float *)(uintptr_t)q;
                for (int w = 0; w + 2 < 0x100 && !near; w++)
                    near = fabsf(fv[w] - nx) < nr && fabsf(fv[w + 2] - nz) < nr && fv[w + 1] > 0 && fv[w + 1] < 200;
            }
            if (!near) continue;
        }
        if (watch) { pla_ai_watch(g_ai_seen[i]); continue; }
        snprintf(p, sizeof p, "%s/ai_%llu_%d.bin", dir, (unsigned long long)renderer_present_count(), i);
        FILE *f = fopen(p, "wb");
        if (!f) continue;
        size_t budget = 24u << 20;
        aidump_node(f, g_ai_seen[i], "", 0, &budget);
        fclose(f);
    }
}

// Research (LARECOMP_PLA_FRAMES_USERS=<presents>): who else asks a clock for its frames (f_70024a84fc, also reached
// from the thunk above): the callers (lr) and how often, listed once after that many presents of the world.
static int pla_frames_users_hook(Cpu *c, uint64_t addr) {
    static struct { uint64_t fn, lr; unsigned n; } u[64];
    static int nu, done;
    static uint64_t from;
    extern uint64_t renderer_present_count(void);
    uint64_t now = renderer_present_count();
    if (done) return 0;
    if (!from) from = now;
    int i = 0;
    while (i < nu && (u[i].lr != c->x[30] || u[i].fn != addr)) i++;
    if (i == nu && nu < 64) u[nu].fn = addr, u[nu++].lr = c->x[30];
    if (i < 64) u[i].n++;
    if (now - from > (uint64_t)atol(getenv("LARECOMP_PLA_FRAMES_USERS"))) {
        done = 1;
        for (int k = 0; k < nu; k++)
            LOG("[frames users] f_%llx from %llx: %u calls over %llu presents\n", (unsigned long long)u[k].fn, (unsigned long long)u[k].lr, u[k].n,
                (unsigned long long)(now - from));
    }
    return 0;
}

void pla60_configure(const char *exefs) {
    char p[1100];
    snprintf(p, sizeof p, "%s/main", exefs);
    uint8_t id[8] = {0};
    FILE *f = fopen(p, "rb");
    if (f) {
        fseek(f, 0x40, SEEK_SET);
        if (fread(id, 1, 8, f) != 8) memset(id, 0, 8);
        fclose(f);
    }
    const char *e = getenv("LARECOMP_PLA_60FPS"), *n = getenv("LARECOMP_LEGENDS_FPS");
    bool on = e ? atoi(e) != 0 : g_settings.fps >= 60;
    g_pla60_ns = !e && g_settings.fps > 60 ? 1000000000ull / (uint64_t)g_settings.fps : 16666667;
    if (n && atoi(n) > 30) on = true, g_pla60_ns = 1000000000ull / (uint64_t)atoi(n);
    g_pla60_entry = g_pla60_build = 0;
    g_pla60_patched = false;
    { void dt_probe_init(void), watch_arg_init(void), gate_init(void), args_probe_init(void); dt_probe_init(); watch_arg_init(); gate_init(); args_probe_init(); }
    if (!memcmp(id, k_legends60[0].id, 8)) {  // (Legends: Arceus 1.1.1)
        void rt_register_fnhook_at(uint64_t, int (*)(Cpu *, uint64_t));
        static bool hooked;
        if (!hooked) {
            hooked = true;
            g_pla_mouse_look = !getenv("LARECOMP_PLA_MOUSE_LOOK") || atoi(getenv("LARECOMP_PLA_MOUSE_LOOK")) != 0;
            rt_register_fnhook_at(0x700193d16cull, pla_mouse_look_hook);
            rt_register_fnhook_at(PLA_SCRIPT_STEP, pla_script_step_hook);
            if (getenv("LARECOMP_PLA_FLICK_LOG")) rt_register_fnhook_at(0x7001aa085cull, pla_flick_log_hook);
            if (getenv("LARECOMP_PLA_FLICK_LOG")) rt_register_fnhook_at(0x7001911760ull, pla_hit_event_hook);
            if (getenv("LARECOMP_PLA_BALL_LOG")) rt_register_fnhook_at(0x7002adbce0ull, pla_ball_log_hook);
            if (getenv("LARECOMP_PLA_BALL_LOG")) rt_register_fnhook_at(0x7001a7236cull, pla_ball_clock_hook);
            if (!getenv("LARECOMP_PLA_WAIT_TICK") || atoi(getenv("LARECOMP_PLA_WAIT_TICK"))) rt_register_fnhook_at(PLA_SCRIPT_FRAMES, pla_script_frames_hook);
            if (!getenv("LARECOMP_PLA_MOVE_SCALE") || atoi(getenv("LARECOMP_PLA_MOVE_SCALE"))) rt_register_fnhook_at(PLA_SCRIPTED_MOVE, pla_scripted_move_hook);
            if (!getenv("LARECOMP_PLA_THROW_SCALE") || atoi(getenv("LARECOMP_PLA_THROW_SCALE"))) {
                rt_register_fnhook_at(PLA_THROW_FRAMES, pla_throw_frames_hook);
                rt_register_fnhook_at(PLA_FALL_TIME, pla_fall_time_hook);
            }
            if (getenv("LARECOMP_PLA_FRAMES_USERS")) {  // (and the clock's other two getters, vtable +0x38 / +0x40)
                rt_register_fnhook_at(0x70024a84fcull, pla_frames_users_hook);
                rt_register_fnhook_at(0x70024a87d4ull, pla_frames_users_hook);
                rt_register_fnhook_at(0x70024a8cc4ull, pla_frames_users_hook);
            }
            if (getenv("LARECOMP_PLA_AIDUMP")) rt_register_fnhook_at(0x7000a6bc64ull, pla_ai_seen_hook);
            if (getenv("LARECOMP_PLA_BT"))
                for (char *e = getenv("LARECOMP_PLA_BT"); *e;) {
                    static int nb;
                    uint64_t a = strtoull(e, &e, 16);
                    if (a && nb < 8) g_bt_addr[nb++] = a, rt_register_fnhook_at(a, pla_bt_hook);
                    if (*e != ',') break;
                    e++;
                }
            for (char *e = getenv("LARECOMP_PLA_GATE30"); e && *e;) {
                uint64_t a = strtoull(e, &e, 16);
                if (a) rt_register_fnhook_at(a, pla_gate30_hook);
                while (*e == ',') e++;
                if (*e && !isxdigit((unsigned char)*e)) break;
            }
            if (getenv("LARECOMP_PLA_STATE_WATCH")) rt_register_fnhook_at(0x70019b24dcull, pla_state_watch_hook);
            // the mouse in the menus (runtime/hle/menu_mouse.c): ui2d's CalculateMtx walk over the panes, each frame
            extern void menu_mouse_configure(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t);
            menu_mouse_configure(0x700014319cull, 0x700272b6d0ull, 0x7002719878ull, 0x910, 0x914);  // (ui2d matrices,
            // the menu navigator's core, the scroll lists' select cell)
            extern void pla_options_configure(void);
            pla_options_configure();
        }
    }
    for (size_t i = 0; i < sizeof k_legends60 / sizeof *k_legends60; i++)
        if (!memcmp(id, k_legends60[i].id, 8)) {
            g_pla60_build = k_legends60[i].entry;
            if (on) g_pla60_entry = g_pla60_build, LOG("[fps60] %s at %.0f fps\n", k_legends60[i].name, 1e9 / (double)g_pla60_ns);
        }
}

// The frame rate changed while playing (the game's Settings screen, hle/*_options.c): 30 the game's own; above it the
// table entry and the present pacing (and the rest that follows pla60_period_ns) at once. False: not a known build.
bool legends_set_fps(int fps) {
    if (!g_pla60_build) return false;
    uint64_t ns = fps == 60 ? 16666667 : fps > 30 ? 1000000000ull / (uint64_t)fps : 33333334;
    volatile uint64_t *ft = (volatile uint64_t *)(uintptr_t)g_pla60_build;
    bool loaded = g_pla60_patched || *ft == 33333334u;  // (the game's data there: else pla60_present patches it later)
    if (loaded) {
        plat_protect(g_pla60_build & ~(PLAT_PAGE - 1), PLAT_PAGE, PLAT_PROT_RW);
        *ft = ns;
    }
    g_pla60_ns = g_pla60_dt = fps > 30 ? ns : 16666667;
    g_pla60_entry = fps > 30 ? g_pla60_build : 0;
    g_pla60_patched = fps > 30 && loaded;
    LOG("[fps60] %d fps from now on\n", fps);
    return true;
}

// A real-time step (pla60_present) from the time since the last present: at least half the frame rate's period (two
// presents close together), the time a short step took for granted taken off the next ones - the world's time adds up
// to the real time exactly (clamped to the period, the paced presents' jitter ran the game a few % fast) - and at most
// 33.3 ms, the game's own step at 30 fps. A longer frame's rest is made up over the next frames, each still at most
// 33.3 ms: the sound runs on the audio device's clock, and a cutscene's picture stayed behind its sound by every hitch
// it had (a first play's shader stall, a stream encoding beside the game: seconds by the end of Z-A's intro). Up to 1 s
// is made up (LARECOMP_CATCHUP_MS; 0: none, as before) - a longer stall is a load, and its next scene isn't sped up.
static uint64_t real_step(uint64_t real) {
    static uint64_t ahead, behind;  // (the world's time ahead of / behind the real time)
    static int64_t max_behind = -1;
    if (max_behind < 0) max_behind = (getenv("LARECOMP_CATCHUP_MS") ? atoll(getenv("LARECOMP_CATCHUP_MS")) : 1000) * 1000000ll;
    uint64_t lo = g_pla60_ns / 2, hi = 33333334u, dt = real > ahead ? real - ahead : 0;
    ahead = dt < lo ? lo - dt : 0;
    if (dt < lo) dt = lo;
    dt += behind;
    behind = dt > hi ? dt - hi : 0;
    if (behind > (uint64_t)max_behind) {
        static int logged;
        if (logged++ < 20) LOG("[fps60] a %.0f ms stall: %.0f ms of it not made up (a load)\n", (double)real / 1e6, (double)(behind - (uint64_t)max_behind) / 1e6);
        behind = (uint64_t)max_behind;
    }
    return dt > hi ? hi : dt;
}

// each present: the table entry, once the game is loaded (read-only data: the page made writable first)
void pla60_present(void) {
    { extern void fscan_present(uint64_t frame); extern uint64_t renderer_present_count(void); fscan_present(renderer_present_count()); }
    { extern void pla_options_present(void); pla_options_present(); }
    { void pla_step_stats(void); pla_step_stats(); }
    if (g_pla60_entry && g_pla60_patched) {
        // Real-time steps (Settings Frame Timing, the default; Unlimited always): the world steps by the time that
        // really passed since the last present, not the frame rate's fixed period - a slow frame (a hitch, a dip below
        // the frame rate) no longer slows the game down. real_step: never more than 33.3 ms, the game's own step at 30
        // fps, the most it was made for. The step sites,
        // the scaled amounts and the 30 fps ticks below follow. LARECOMP_DYN_DT=0/1 overrides the setting.
        static int env = -2;
        if (env == -2) env = getenv("LARECOMP_DYN_DT") ? atoi(getenv("LARECOMP_DYN_DT")) != 0 : -1;
        bool dyn = env >= 0 ? env : !g_settings.fixed_steps || g_settings.fps >= FPS_UNLIMITED;
        static uint64_t last;
        uint64_t now = plat_now_ns(), dt = g_pla60_ns;
        if (dyn && last) dt = real_step(now - last);
        // Whole periods only, the rest carried (the world still keeps to the real time: a long frame's step is two
        // periods). Some of the games' timers count whole ticks and round up: a step a little over the period (16.9 ms
        // at 60) counted as two - Z-A's intro cinematics ran ~1.8x fast (Prism Tower lit before the talk ended, the
        // train outran the field's tree streaming). LARECOMP_DT_QUANT=0: raw.
        static int quant = -1;
        if (quant < 0) quant = !(getenv("LARECOMP_DT_QUANT") && atoi(getenv("LARECOMP_DT_QUANT")) == 0);
        if (dyn && last && quant && g_pla60_ns) {
            static int64_t bal;
            bal += (int64_t)dt;
            int64_t k = (bal + (int64_t)g_pla60_ns / 2) / (int64_t)g_pla60_ns;
            if (k < 1) k = 1;
            if ((uint64_t)k * g_pla60_ns > 33333334u) k = 33333334u / g_pla60_ns ? 33333334u / g_pla60_ns : 1;
            dt = (uint64_t)k * g_pla60_ns;
            bal -= (int64_t)dt;
            if (bal < -(int64_t)g_pla60_ns) bal = -(int64_t)g_pla60_ns;
        }
        if (dt != g_pla60_dt) *(volatile uint64_t *)(uintptr_t)g_pla60_entry = dt, g_pla60_dt = dt;
        g_world_prev = g_world_now, g_world_now += dt;
        last = now;
        return;
    }
    if (!g_pla60_entry) return;
    volatile uint64_t *ft = (volatile uint64_t *)(uintptr_t)g_pla60_entry;
    if (*ft != 33333334u) return;
    plat_protect(g_pla60_entry & ~(PLAT_PAGE - 1), PLAT_PAGE, PLAT_PROT_RW);
    *ft = g_pla60_ns;
    g_pla60_dt = g_pla60_ns;
    g_pla60_patched = true;
}

// The 1/30 s steps Legends: Arceus builds in code (tools/patches.py PLA_STEP_SITES): the frame time it runs at, at the
// sites that step by it on every update; 1/30 at 30 fps and at the sites left off. LARECOMP_STEP_SITES (research):
// "all", "none", or a list of site indices and ranges from the default set ("+3", "-3", "10-20", "-10-20").
#define PLA_STEP_SITES 45
static const char k_step_default[PLA_STEP_SITES + 1] =
    "111110011111111111111111110111111101000000000";  // (by index: 1 = the real frame time)
static bool g_step_on[PLA_STEP_SITES];
static _Atomic unsigned g_step_calls[PLA_STEP_SITES];

// Research (LARECOMP_STEP_STATS=1): every 5 s, how many times a second each site ran - once per update (as often as
// the frame rate: a speed that follows it) or at a rate of its own (a fixed step: right at any frame rate).
void pla_step_stats(void) {
    static int on = -1;
    static uint64_t t0;
    if (on < 0) on = getenv("LARECOMP_STEP_STATS") != NULL;
    if (!on) return;
    extern uint64_t plat_now_ns(void);
    uint64_t now = plat_now_ns();
    if (!t0) t0 = now;
    if (now - t0 < 5000000000ull) return;
    char line[2048];
    int n = 0;
    for (int k = 0; k < PLA_STEP_SITES; k++) {
        unsigned v = atomic_exchange(&g_step_calls[k], 0);
        if (v && n < (int)sizeof line - 32) n += snprintf(line + n, sizeof line - n, " %d:%.0f", k, v / ((now - t0) / 1e9));
    }
    LOG("[steps] per second:%s\n", n ? line : " none");
    t0 = now;
}
uint32_t rt_step_bits(int i) {
    static int init;
    static uint32_t bits;
    if (!init) {
        init = 1;
        for (int k = 0; k < PLA_STEP_SITES; k++) g_step_on[k] = k_step_default[k] == '1';
        const char *e = getenv("LARECOMP_STEP_SITES");
        for (const char *s = e; s && *s;) {
            while (*s == ',' || *s == ' ') s++;
            if (!strncmp(s, "all", 3)) { for (int k = 0; k < PLA_STEP_SITES; k++) g_step_on[k] = true; s += 3; continue; }
            if (!strncmp(s, "none", 4)) { for (int k = 0; k < PLA_STEP_SITES; k++) g_step_on[k] = false; s += 4; continue; }
            bool on = *s != '-';
            if (*s == '-' || *s == '+') s++;
            char *t;
            long a = strtol(s, &t, 10), b = a;
            if (t == s) break;
            if (*t == '-') b = strtol(t + 1, &t, 10);
            for (long k = a; k <= b && k < PLA_STEP_SITES; k++) if (k >= 0) g_step_on[k] = on;
            s = t;
        }
        if (e) {
            char m[PLA_STEP_SITES + 1];
            for (int k = 0; k < PLA_STEP_SITES; k++) m[k] = g_step_on[k] ? '1' : '0';
            m[PLA_STEP_SITES] = 0;
            LOG("[fps60] step sites %s\n", m);
        }
    }
    if (i < 0 || i >= PLA_STEP_SITES) return 0x3d088889u;
    atomic_fetch_add_explicit(&g_step_calls[i], 1, memory_order_relaxed);
    if (!g_pla60_patched || !g_step_on[i]) return 0x3d088889u;  // 1/30
    static uint64_t bits_ns;  // (the frame time it was worked out for: it may change while playing)
    if (bits_ns != g_pla60_dt) { float f = (float)((double)g_pla60_dt * 1e-9); memcpy(&bits, &f, 4); bits_ns = g_pla60_dt; }
    return bits;
}

// Legends: Arceus above 30 fps: 1 on the updates where 30 fps time moved on a frame (tools/patches.py _pla_tick_store),
// 0 on the others; always 1 at 30 fps. LARECOMP_PLA_TICK=0 (research): every update, as the game does.
// Research (LARECOMP_PLA_PRINT_LOG=1): the message printer printed (tools/patches.py, 0x7002b3186c): each print's time
// and printer logged, to time a line's typing at different frame rates.
void rt_pla_printed(uint64_t printer) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_PLA_PRINT_LOG") && atoi(getenv("LARECOMP_PLA_PRINT_LOG")) > 0;
    if (!on) return;
    static int force = -2;  // (LARECOMP_PLA_TEXT_SPEED=<n>: the printer's text speed byte set to n, to time a slower text)
    if (force == -2) force = getenv("LARECOMP_PLA_TEXT_SPEED") ? atoi(getenv("LARECOMP_PLA_TEXT_SPEED")) : -1;
    uint8_t *speed = (uint8_t *)(uintptr_t)(printer + 0x9d);
    if (force >= 0) *speed = (uint8_t)force;
    extern uint64_t renderer_present_count(void);
    LOG("[print] %.3f s frame %llu printer %llx speed %u\n", (double)plat_now_ns() / 1e9, (unsigned long long)renderer_present_count(),
        (unsigned long long)printer, (unsigned)*speed);
}

int rt_pla_tick(int site) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_PLA_TICK") ? atoi(getenv("LARECOMP_PLA_TICK")) : 1;
    int r = !on || !g_pla60_patched ? 1 : g_world_now / 33333334u != g_world_prev / 33333334u;  // (the world's time in this
                                                                                                 // update's steps)
    static int tlog = -1;  // (research: LARECOMP_PLA_TICK_LOG=1 - each second, per site: calls, ticks, presents)
    if (tlog < 0) tlog = getenv("LARECOMP_PLA_TICK_LOG") && atoi(getenv("LARECOMP_PLA_TICK_LOG")) > 0;
    if (tlog) {
        static struct { int site, calls, ticks; } s[8];
        static uint64_t t0, p0;
        extern uint64_t renderer_present_count(void);
        int k = 0;
        while (k < 7 && s[k].calls && s[k].site != site) k++;
        s[k].site = site, s[k].calls++, s[k].ticks += r;
        static uint64_t hp; static int hn, hist[4];  // (the printer site 0x1864: frames with 1 / 2 / 3 / 4+ calls)
        uint64_t pc = renderer_present_count();
        if (site == 0x1864) { if (pc != hp) { if (hn) hist[hn > 4 ? 3 : hn - 1]++; hp = pc, hn = 0; } hn++; }
        uint64_t now = plat_now_ns();
        if (!t0) t0 = now, p0 = renderer_present_count();
        if (now - t0 >= 1000000000ull) {
            char line[256];
            int o = snprintf(line, sizeof line, "[tick] %llu presents, patched %d:", (unsigned long long)(renderer_present_count() - p0),
                             (int)g_pla60_patched);
            for (int i = 0; i < 8 && s[i].calls; i++) o += snprintf(line + o, sizeof line - o, " site %04x %d/%d", s[i].site, s[i].ticks, s[i].calls);
            if (hist[0] + hist[1] + hist[2] + hist[3]) o += snprintf(line + o, sizeof line - o, " | printer calls per frame 1:%d 2:%d 3:%d 4+:%d", hist[0], hist[1], hist[2], hist[3]);
            memset(hist, 0, sizeof hist);
            LOG("%s\n", line);
            memset(s, 0, sizeof s), t0 = now, p0 = renderer_present_count();
        }
    }
    return r;
}

// Research (LARECOMP_DT_PROBE="<addr>[,<addr>...]"): functions shaped like f_700141f208 - this = x0, the clock set = x1:
// the timer index this->vtable[0x50]() picks and the nanoseconds (*x1)->vtable[0](index) hands out, logged.
static int dt_probe_hook(Cpu *c, uint64_t addr) {
    static _Atomic int n;
    if (atomic_fetch_add(&n, 1) > 300) return 0;
    uint64_t saved[32];
    memcpy(saved, c->x, sizeof saved);
    uint64_t self = c->x[0], set = c->x[1];
    uint64_t vt = self >= 0x7000000000ull ? *(uint64_t *)(uintptr_t)self : 0;
    if (vt >= 0x7000000000ull && set >= 0x7000000000ull) {
        c->x[0] = self;
        guest_call(c, *(uint64_t *)(uintptr_t)(vt + 0x50));
        uint64_t which = (uint32_t)c->x[0];
        uint64_t clocks = *(uint64_t *)(uintptr_t)set, cvt = clocks >= 0x7000000000ull ? *(uint64_t *)(uintptr_t)clocks : 0;
        if (cvt >= 0x7000000000ull) {
            c->x[0] = clocks, c->x[1] = which;
            guest_call(c, *(uint64_t *)(uintptr_t)cvt);
            uint64_t r = c->x[0];
            LOG("[dt] f_%llx present %llu: this %llx timer %llu -> %llx ns %lld\n", (unsigned long long)addr,
                (unsigned long long)renderer_game_frame(), (unsigned long long)self, (unsigned long long)which, (unsigned long long)r,
                r >= 0x7000000000ull ? (long long)*(int64_t *)(uintptr_t)(r + 8) : -1LL);
        }
    }
    memcpy(c->x, saved, sizeof saved);
    return 0;
}
void dt_probe_init(void) {  // (pla60_configure: once the game library is loaded)
    const char *e = getenv("LARECOMP_DT_PROBE");
    for (char *s = (char *)e; s && *s;) {
        uint64_t a = strtoull(s, &s, 16);
        if (a) { void rt_register_fnhook_at(uint64_t, int (*)(Cpu *, uint64_t)); rt_register_fnhook_at(a, dt_probe_hook); }
        while (*s == ',') s++;
        if (*s && !strchr("0123456789abcdefABCDEF", *s)) break;
    }
}

// Legends: Arceus above 30 fps: a per-update amount the game sized for 30 fps (a float's bits), scaled to the frame time
// it runs at (tools/patches.py: the camera's turn rate). LARECOMP_PLA_SCALE=0 (research): the game's own amounts.
uint32_t rt_scale30_f32(uint32_t bits) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_PLA_SCALE") ? atoi(getenv("LARECOMP_PLA_SCALE")) : 1;
    if (!on || !g_pla60_patched) return bits;
    float f;
    memcpy(&f, &bits, 4);
    f = (float)((double)f * (double)g_pla60_dt / 33333334.0);
    memcpy(&bits, &f, 4);
    return bits;
}

// Research (LARECOMP_WATCH_ARG="<fn hex>:<offset hex>:<from present>"): from that present on, the first entries of
// that function arm hardware watchpoints (hwwatch.c) on this + offset - who writes a field of the object it gets
// (up to 4 objects).
static uint64_t g_wa_fn, g_wa_off;
static long g_wa_from;
static int watch_arg_hook(Cpu *c, uint64_t addr) {
    static uint64_t seen[4];
    static int n;
    (void)addr;
    if ((long)renderer_game_frame() < g_wa_from || n >= 4) return 0;
    uint64_t a = c->x[0] + g_wa_off;
    for (int i = 0; i < n; i++) if (seen[i] == a) return 0;
    seen[n++] = a;
    void hwwatch_arm(uint64_t addr, uint64_t len);
    hwwatch_arm(a & ~3ull, 4);
    LOG("[watcharg] f_%llx this %llx: watching %llx (present %llu)\n", (unsigned long long)addr, (unsigned long long)c->x[0],
        (unsigned long long)(a & ~3ull), (unsigned long long)renderer_game_frame());
    return 0;
}
void watch_arg_init(void) {
    const char *e = getenv("LARECOMP_WATCH_ARG");
    if (!e) return;
    char *s;
    g_wa_fn = strtoull(e, &s, 16);
    if (*s == ':') g_wa_off = strtoull(s + 1, &s, 16);
    if (*s == ':') g_wa_from = strtol(s + 1, &s, 10);
    void rt_register_fnhook_at(uint64_t, int (*)(Cpu *, uint64_t));
    if (g_wa_fn) rt_register_fnhook_at(g_wa_fn, watch_arg_hook);
}
// (the inverse: a divisor that sizes a per-update amount)
uint32_t rt_unscale30_f32(uint32_t bits) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_PLA_SCALE") ? atoi(getenv("LARECOMP_PLA_SCALE")) : 1;
    if (!on || !g_pla60_patched) return bits;
    float f;
    memcpy(&f, &bits, 4);
    f = (float)((double)f * 33333334.0 / (double)g_pla60_dt);
    memcpy(&bits, &f, 4);
    return bits;
}

// Research (LARECOMP_GATE="<fn hex>[,<fn hex>...]"): those functions run only on the updates where 30 fps time moved
// on a frame (rt_pla_tick), skipped on the others - which update a speed that follows the frame rate comes from.
static int gate_hook(Cpu *c, uint64_t addr) {
    (void)c, (void)addr;
    return !rt_pla_tick(0);  // 1: skipped (the function returns at once)
}
void gate_init(void) {
    const char *e = getenv("LARECOMP_GATE");
    for (char *s = (char *)e; s && *s;) {
        uint64_t a = strtoull(s, &s, 16);
        void rt_register_fnhook_at(uint64_t, int (*)(Cpu *, uint64_t));
        if (a) rt_register_fnhook_at(a, gate_hook);
        while (*s == ',') s++;
        if (*s && !strchr("0123456789abcdefABCDEF", *s)) break;
    }
}

// Legends: Arceus above 30 fps: the 60 Hz frames an update is worth (tools/patches.py: the cutscene clock steps 2 per
// update, sized for 30 fps): 2 at 30 fps, at 144 fps 0.42 - whole frames, the fraction carried (per site).
uint32_t rt_pla_frames60(int site) {
    static double acc[8];
    if (!g_pla60_patched || site < 0 || site >= 8) return 2;
    acc[site] += 2.0 * (double)g_pla60_dt / 33333334.0;
    uint32_t n = (uint32_t)acc[site];
    acc[site] -= n;
    return n;
}

// Research (LARECOMP_ARGS_PROBE="<fn hex>[,...]"): the first calls of those functions: x0..x3 and s0..s1 (a float
// argument such as an effect system's frame rate).
static int args_probe_hook(Cpu *c, uint64_t addr) {
    static _Atomic int n;
    if (atomic_fetch_add(&n, 1) > 200) return 0;
    float s0, s1;
    uint32_t b0 = (uint32_t)c->v[0], b1 = (uint32_t)c->v[1];
    memcpy(&s0, &b0, 4), memcpy(&s1, &b1, 4);
    LOG("[args] f_%llx present %llu: x0 %llx x1 %llx x2 %llx x3 %llx s0 %g s1 %g lr %llx\n", (unsigned long long)addr,
        (unsigned long long)renderer_game_frame(), (unsigned long long)c->x[0], (unsigned long long)c->x[1],
        (unsigned long long)c->x[2], (unsigned long long)c->x[3], (double)s0, (double)s1, (unsigned long long)c->x[30]);
    return 0;
}
void args_probe_init(void) {
    const char *e = getenv("LARECOMP_ARGS_PROBE");
    for (char *s = (char *)e; s && *s;) {
        uint64_t a = strtoull(s, &s, 16);
        void rt_register_fnhook_at(uint64_t, int (*)(Cpu *, uint64_t));
        if (a) rt_register_fnhook_at(a, args_probe_hook);
        while (*s == ',') s++;
        if (*s && !strchr("0123456789abcdefABCDEF", *s)) break;
    }
}

// (the effect manager's 60 Hz frames, d = the frame time / 16.67 ms as a double's bits: the float's bits; whole frames
// unless above 30 fps)
uint32_t rt_pla_frames60_f(uint64_t dbits) {
    double d;
    memcpy(&d, &dbits, 8);
    float f = g_pla60_patched ? (float)d : (float)(int32_t)d;
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}
