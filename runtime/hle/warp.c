// LARECOMP_WARP="<x> <y> <z>" (Legends Studio's "Play here"): the player put at that spot of the area once it has
// loaded. Legends: Arceus 1.1.1's character bodies (vtable 0x7004187048: the player's, the NPCs', their parts) keep
// their position at +0x90 (a rotation matrix before it, at +0x60) and the game follows it: writing the player's moves
// the player there to stay. Which body is the player's: the one standing exactly where the camera looks - the camera
// update (0x7002994b70, the camera as x0) keeps its target, the player's position, at +0x320 - among the bodies the
// physics steps (0x7002fee4e0, the body as x0). Written once it has existed for two seconds and stood still for one
// (the save's own position applied, the fade-in over), then for a few presents more; once.
// LARECOMP_WARP_AREA="<area>" (Play here in another area than the save's): first the game's own map change, as the
// triggers' map_change action does it (its handler 0x700158792c reads the area, x y z, the angle, and calls the request
// 0x7001df37d4: x0 {FNV-1a 64 of the area name, the name}, x1 the position, x2 the rotation quaternion, s0 180, w3-w5
// 0, x8 its result) - made from the camera update (the game thread), once the save's area is up; then the warp above.
// LARECOMP_STORY="<n>" (Play here at a story point): the story progress, the work WPHA_STORY (0 .. 100000: 10000 after
// the intro, 80000 late), set right before that map change is asked (without LARECOMP_WARP_AREA, before the warp), so
// the area loads with that progress' NPC placements and triggers. LARECOMP_WORKS="<name>=<value>,..." sets any works the
// same way. The works are an unordered_map<u64, u32> (at +0x1d8 of the event work object, read by 0x7000ff5878 with
// x1 -> {hash, name, length, 0} as w0) whose nodes are {next, hash, key, u32 value}, hash == key == FNV-1a 64 of the
// name: the node is looked up the way the getter does it and the value written there (no setter of the game's found).
// LARECOMP_FLAGS="<name>=<0|1>,..." / LARECOMP_FLAGS_FILE=<file of name=value lines> (the editor's: the flags the story's
// phases up to that point set and clear) the same way: the flags are the map before it, at +0x1b8, the same nodes.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));

#define BODY_VTABLE 0x7004187048ull
static float g_to[3];
static volatile uint64_t g_body, g_cam;
static int g_state = -1;  // -1 off, 0 waiting, 1 writing, 2 done
static char g_area[64];   // (LARECOMP_WARP_AREA: an area change first)
static volatile int g_area_state;  // 0 none / done, 1 waiting for the save's area, 2 asked (the camera hook makes it)

#define MAP_CHANGE_REQUEST 0x7001df37d4ull

static uint64_t fnv1a64(const char *s) {
    uint64_t h = 0xcbf29ce484222645ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}

// the game's map change into g_area at g_to, from a game-thread hook (the caller's registers kept as they were)
static void map_change(Cpu *c) {
    Cpu saved = *c;
    uint64_t sp = (c->sp - 0x800) & ~0xfull, base = sp + 0x100;
    uint8_t *m = (uint8_t *)(uintptr_t)base;
    memset(m, 0, 0x200);
    uint64_t name = base + 0x80, pos = base + 0x20, rot = base + 0x40, res = base + 0x60;
    *(uint64_t *)(m + 0) = fnv1a64(g_area);
    *(uint64_t *)(m + 8) = name;
    strcpy((char *)(m + 0x80), g_area);
    memcpy(m + 0x20, g_to, 12);
    ((float *)(m + 0x40))[3] = 1.0f;  // (no turn: the identity quaternion)
    c->sp = sp;
    c->x[0] = base, c->x[1] = pos, c->x[2] = rot, c->x[3] = 0, c->x[4] = 0, c->x[5] = 0, c->x[8] = res;
    float f = 180.0f;
    c->v[0] = 0;
    memcpy(&c->v[0], &f, 4);
    guest_call(c, MAP_CHANGE_REQUEST);
    *c = saved;
    LOG("[warp] map change asked: %s at %.2f %.2f %.2f\n", g_area, g_to[0], g_to[1], g_to[2]);
}

static bool in_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX; }

#define MAX_WORKS 4096
static struct { char name[64]; uint64_t hash; uint32_t value; uint16_t map; } g_works[MAX_WORKS];  // (LARECOMP_STORY / _WORKS / _FLAGS)
static int g_nworks;
static volatile int g_works_state;  // 0 none, 1 to write once the save's area is up, 2 written

#define WORKS_MAP 0x1d8
#define FLAGS_MAP 0x1b8
static void add_work(const char *name, size_t len, uint32_t value, uint16_t map) {
    if (g_nworks >= MAX_WORKS || !len || len >= sizeof g_works[0].name) return;
    memcpy(g_works[g_nworks].name, name, len), g_works[g_nworks].name[len] = 0;
    g_works[g_nworks].hash = fnv1a64(g_works[g_nworks].name), g_works[g_nworks].value = value, g_works[g_nworks].map = map;
    g_nworks++, g_works_state = 1;
}

// "NAME=value,NAME=value" (or one per line) into the list, for that map
static void add_works(const char *p, uint16_t map) {
    while (p && *p) {
        while (*p == ',' || *p == '\n' || *p == '\r' || *p == ' ') p++;
        if (!*p) break;
        const char *end = p + strcspn(p, ",\r\n"), *eq = memchr(p, '=', (size_t)(end - p));
        if (eq) add_work(p, (size_t)(eq - p), (uint32_t)strtoul(eq + 1, NULL, 0), map);
        p = end;
    }
}

// the work's node in the game's own map, as the getter 0x7000ff5878 finds it: the event work object (*(system + 0x228),
// the system **(0x7004279560)) keeps the map at +0x1d8, {buckets, their count} (libc++: the bucket of a hash is hash &
// (count - 1) for a power of two, else hash % count; a bucket holds the node before its first); NULL when not there
static uint32_t *work_node(uint64_t h, uint16_t at) {
#define RD(a) (*(const uint64_t *)(uintptr_t)(a))
    uint64_t p = RD(0x7004279560ull), sys = p ? RD(p) : 0;
    if (!in_heap(sys)) return NULL;
    uint64_t ew = RD(sys + 0x228);
    if (!in_heap(ew)) return NULL;
    uint64_t map = RD(ew + at);
    if (!in_heap(map)) return NULL;
    uint64_t buckets = RD(map), nb = RD(map + 8);
    if (!in_heap(buckets) || !nb) return NULL;
#define BUCKET(x) ((nb & (nb - 1)) == 0 ? (x) & (nb - 1) : (x) < nb ? (x) : (x) % nb)
    uint64_t i = BUCKET(h), n = RD(buckets + i * 8);
    for (n = in_heap(n) ? RD(n) : 0; in_heap(n); n = RD(n)) {
        if (RD(n + 8) == h && RD(n + 16) == h) return (uint32_t *)(uintptr_t)(n + 0x18);
        if (BUCKET(RD(n + 8)) != i) break;
    }
    return NULL;
#undef BUCKET
#undef RD
}

// the mod API's game.work: a work (map +0x1d8) or, named "flag:<name>", a flag (+0x1b8) read or written in place
int warp_work(const char *name, int set, int64_t value, int64_t *out) {
    uint16_t map = WORKS_MAP;
    if (!strncmp(name, "flag:", 5)) name += 5, map = FLAGS_MAP;
    uint32_t *v = work_node(fnv1a64(name), map);
    if (!v) return 0;
    if (out) *out = *v;
    if (set) *v = (uint32_t)value, LOG("[warp] script: %s %s = %lld\n", map == FLAGS_MAP ? "flag" : "work", name, (long long)value);
    return 1;
}

// the works written where the game keeps them: by the map above; failing that a scan of game memory for the hash twice
// (the ranges as LARECOMP_LIVE's "find" takes them: the heaps, then the game's own .data / .bss), written only when
// unique - other maps keyed by the same hashes have such nodes too, and a write into one of those crashed the game
static void works_write(void) {
    int left = 0;
    for (int i = 0; i < g_nworks; i++) {
        uint32_t *v = work_node(g_works[i].hash, g_works[i].map);
        if (!v) { left++; continue; }
        if (g_nworks <= 16 || g_works[i].map == WORKS_MAP || *v != g_works[i].value)  // (a long flag list: only the changes told)
            LOG("[warp] story: %s %s = %u at %llx (was %u)\n", g_works[i].map == FLAGS_MAP ? "flag" : "work", g_works[i].name, g_works[i].value,
                (unsigned long long)(uintptr_t)v, *v);
        *v = g_works[i].value, g_works[i].hash = 0;
    }
    g_works_state = 2;
    LOG("[warp] story: %d of %d works / flags written\n", g_nworks - left, g_nworks);
    if (!left) return;
    if (left > 16) {  // (a long list's missing ones: flags the save never had - not scanned for, one by one)
        LOG("[warp] story: %d not in the game's maps (not written)\n", left);
        return;
    }
    extern int kernel_physmem_ranges(uint64_t *lo, uint64_t *hi, int max);
    extern uint64_t kernel_heap_size(void), g_stdheap_used;
    extern int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);
    static uint64_t lo[1100], hi[1100];
    int n = kernel_physmem_ranges(lo + 1, hi + 1, 519) + 1;
    uint64_t top = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used : 0, heap = HEAP_BASE + kernel_heap_size();
    lo[0] = HEAP_BASE, hi[0] = top > heap ? top : heap;
    {
        uint64_t ra[512], rs[512];
        int m = kernel_data_regions(ra, rs, 512);
        for (int i = 0; i < m && n < 1100; i++)
            if (ra[i] < HEAP_BASE) lo[n] = ra[i], hi[n] = ra[i] + rs[i], n++;
    }
    uint32_t *at[MAX_WORKS] = {0};
    int found[MAX_WORKS] = {0};
    for (int r = 0; r < n; r++)
        for (uint64_t *q = (uint64_t *)(uintptr_t)((lo[r] + 7) & ~7ull), *e = (uint64_t *)(uintptr_t)hi[r]; q + 3 <= e; q++) {
            if (q[0] != q[1]) continue;  // (hash == key: nearly nothing else)
            for (int i = 0; i < g_nworks; i++)
                if (g_works[i].hash && q[0] == g_works[i].hash) {
                    LOG("[warp] story: %s found by scan at %llx (value %u)\n", g_works[i].name, (unsigned long long)(uintptr_t)(q + 2), *(uint32_t *)(q + 2));
                    at[i] = (uint32_t *)(q + 2), found[i]++;
                }
        }
    for (int i = 0; i < g_nworks; i++) {
        if (!g_works[i].hash) continue;
        if (found[i] == 1) LOG("[warp] story: %s = %u at %llx (was %u)\n", g_works[i].name, g_works[i].value, (unsigned long long)(uintptr_t)at[i], *at[i]), *at[i] = g_works[i].value;
        else LOG("[warp] story: the work %s (%016llx) %s - not written\n", g_works[i].name, (unsigned long long)g_works[i].hash, found[i] ? "found more than once" : "not found");
    }
}

static volatile uint64_t g_seen_body;  // (the player's body as last found, warp or not: LARECOMP_LIVE's state shows it)
static int g_world;  // (hle/pla_world.c, the shared world prototype: the player followed all the time)
static volatile int g_api;  // (the mod API: scripts' characters made here too, the player followed - warp_api_start)

static int cam_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if ((g_state == 0 || g_state == 3 || g_world || g_api) && in_heap(c->x[0])) g_cam = c->x[0];
    if (g_world || g_api) { extern void pla_npc_cam(Cpu *); pla_npc_cam(c); }  // (characters of our own are made here: hle/pla_npc.c)
    if (g_area_state == 2) {
        g_area_state = 0;
        if (g_works_state == 1) works_write();  // (the story point before the area change: it loads with it)
        map_change(c);
    }
    return 0;
}

static int body_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t o = c->x[0], cam = g_cam;
    if ((g_state != 0 && g_state != 3 && !g_world && !g_api) || !cam || !in_heap(o) || *(const uint64_t *)(uintptr_t)o != BODY_VTABLE) return 0;
    const float *p = (const float *)(uintptr_t)(o + 0x90), *t = (const float *)(uintptr_t)(cam + 0x320);
    if (fabsf(p[0] - t[0]) + fabsf(p[1] - t[1]) + fabsf(p[2] - t[2]) < 1e-3f) g_seen_body = o, g_body = g_state == 0 ? o : g_body;
    return 0;
}

// (research) LARECOMP_HIDE_PLAYER=1: the player's model isn't drawn (pop-in A/Bs: the third-person camera looks over
// its head). 0x7002a6ba60(sk) poses a skeleton; its drawn world matrix is at +0x410 (axes) / +0x440 (position) -
// LegendsVR body.c. The one standing where the player's body is gets its axes zeroed after posing.
void rt_call_original(Cpu *c, uint64_t addr);
static __thread int t_hiding;  // (inside the player's skeleton's posing)
static int hide_player_hook(Cpu *c, uint64_t addr) {
    uint64_t sk = c->x[0], b = g_seen_body;
    bool me = false;
    if (b && in_heap(sk) && *(const uint64_t *)(uintptr_t)b == BODY_VTABLE) {
        const float *m = (const float *)(uintptr_t)(sk + 0x410), *p = (const float *)(uintptr_t)(b + 0x90);
        me = fabsf(m[12] - p[0]) + fabsf(m[13] - p[1]) + fabsf(m[14] - p[2]) < 0.5f;
    }
    t_hiding += me;
    rt_call_original(c, addr);
    t_hiding -= me;
    return 1;
}
// its palette's maker 0x70029c983c (x2 the bones' matrices, w4 how many): the player's bones collapsed for that call
// only, so what's drawn of it (and its shadow) is a point; the game's bones are put back right after (the camera and
// the rest read them: left zeroed, the 2026-11-08 beach run went purple with the camera off in the terrain)
static int hide_palette_hook(Cpu *c, uint64_t addr) {
    uint32_t n = (uint32_t)c->x[4];
    if (!t_hiding || !in_heap(c->x[2]) || n == 0 || n >= 1024) return 0;
    void *bones = (void *)(uintptr_t)c->x[2];
    void *keep = malloc((size_t)n * 64);
    if (!keep) return 0;
    memcpy(keep, bones, (size_t)n * 64);
    memset(bones, 0, (size_t)n * 64);
    rt_call_original(c, addr);
    memcpy(bones, keep, (size_t)n * 64);
    free(keep);
    return 1;
}

// (research) LARECOMP_ALLOC_BT=<size hex>,<from frame>[,n]: the guest call stacks (frame pointers) of the first n
// allocations of that size (up to 8 KB more) through the game's allocator 0x700084c5b8 (x0 the heap, x1 the size, x2 the
// alignment) from that frame on - who builds a buffer (Lua hooks there crash: it's called from every thread at once)
static volatile uint64_t g_frame_now;
static uint64_t ab_size, ab_from;
static volatile int ab_left;
static int alloc_bt_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[1] < ab_size || c->x[1] >= ab_size + 0x2000 || g_frame_now < ab_from || ab_left <= 0) return 0;
    if (__atomic_sub_fetch(&ab_left, 1, __ATOMIC_RELAXED) < 0) return 0;
    char line[600];
    uint64_t sz = c->x[1], heap = c->x[0], lr0 = c->x[30], fp = c->x[29];
    uint64_t chain[24];
    int nch = 0;
    for (uint64_t f = fp; nch < 24 && f >= GUEST_LO && f < GUEST_HI && !(f & 7); nch++) {
        chain[nch] = ((uint64_t *)(uintptr_t)f)[1];
        f = ((uint64_t *)(uintptr_t)f)[0];
        if (!chain[nch]) break;
    }
    rt_call_original(c, addr);  // (the block it returns, to match it with what the game does with it later)
    int o = snprintf(line, sizeof line, "[allocbt] frame %llu size %llx heap %llx -> %llx: lr %llx", (unsigned long long)g_frame_now,
                     (unsigned long long)sz, (unsigned long long)heap, (unsigned long long)c->x[0], (unsigned long long)lr0);
    for (int i = 0; i < nch && chain[i] && o < (int)sizeof line - 24; i++) o += snprintf(line + o, sizeof line - o, " %llx", (unsigned long long)chain[i]);
    LOG("%s\n", line);
    return 1;
}


__attribute__((constructor)) static void warp_init(void) {
    {
        const char *e = getenv("LARECOMP_ALLOC_BT");
        int n = 12;
        if (e && sscanf(e, "%llx,%llu,%d", (unsigned long long *)&ab_size, (unsigned long long *)&ab_from, &n) >= 2) {
            ab_left = n;
            rt_register_fnhook_at(0x700084c5b8ull, alloc_bt_hook);
        }
    }
    if (getenv("LARECOMP_HIDE_PLAYER"))
        rt_register_fnhook_at(0x7002a6ba60ull, hide_player_hook), rt_register_fnhook_at(0x70029c983cull, hide_palette_hook);
    extern int pla_world_enabled(void);
    g_world = pla_world_enabled() && !getenv("LARECOMP_HIDE_PLAYER");
    const char *w = getenv("LARECOMP_WARP");
    if (!w || sscanf(w, "%f %f %f", &g_to[0], &g_to[1], &g_to[2]) != 3) {
        if (!getenv("LARECOMP_LIVE") && !g_world) return;
        g_state = 3;  // (no warp: only following the player, for LARECOMP_LIVE)
    } else g_state = 0;
    const char *a = getenv("LARECOMP_WARP_AREA");
    if (g_state == 0 && a && *a && strlen(a) < sizeof g_area) strcpy(g_area, a), g_area_state = 1;
    if (g_state == 0) {
        const char *s = getenv("LARECOMP_STORY");
        if (s && *s) add_work("WPHA_STORY", 10, (uint32_t)strtoul(s, NULL, 0), WORKS_MAP);
        add_works(getenv("LARECOMP_WORKS"), WORKS_MAP);  // ("NAME=value,NAME=value")
        add_works(getenv("LARECOMP_FLAGS"), FLAGS_MAP);
        const char *ff = getenv("LARECOMP_FLAGS_FILE");
        FILE *f = ff && *ff ? fopen(ff, "rb") : NULL;
        if (f) {  // (one name=value a line)
            static char buf[1 << 20];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            add_works(buf, FLAGS_MAP);
        }
    }
    rt_register_fnhook_at(0x7002994b70ull, cam_hook);
    rt_register_fnhook_at(0x7002fee4e0ull, body_hook);
}

// The shared world switched on by the launcher's Online tab (hle/pla_world.c pla_world_settings, after the settings are
// read): the player followed all the time, as LARECOMP_WORLD=1 does from the start
void warp_world_start(void) {
    if (g_world) return;
    g_world = 1;
    if (g_state < 0) g_state = 3;
    rt_register_fnhook_at(0x7002994b70ull, cam_hook);
    rt_register_fnhook_at(0x7002fee4e0ull, body_hook);
}

// The mod API (hle/modapi.c): the player followed (its body found as for the warp) and scripts' characters made from
// the camera update (hle/pla_npc.c)
void warp_api_start(void) {
    if (g_api) return;
    g_api = 1;
    if (g_state < 0) g_state = 3;
    rt_register_fnhook_at(0x7002994b70ull, cam_hook);
    rt_register_fnhook_at(0x7002fee4e0ull, body_hook);
}
// where the player is and which way it faces (its body's z axis, +0x80), for scripts
bool warp_player_now(float pos[3], float *yaw) {
    uint64_t b = g_seen_body;
    if (!b || *(const uint64_t *)(uintptr_t)b != BODY_VTABLE) return false;
    memcpy(pos, (const float *)(uintptr_t)(b + 0x90), 12);
    const float *z = (const float *)(uintptr_t)(b + 0x80);
    if (yaw) *yaw = atan2f(z[0], z[2]) * 57.29578f;
    return true;
}
// a script's teleport: the body written for 10 frames (as the warp does: the game settles it there)
static float g_tp[3];
static volatile int g_tp_n;
void warp_teleport(const float p[3]) { memcpy(g_tp, p, 12), g_tp_n = 10; }
// where the player is (LARECOMP_LIVE's state file): false before the body is found
bool warp_player_pos(float out[3]) {
    uint64_t b = g_seen_body;
    if (!b || *(const uint64_t *)(uintptr_t)b != BODY_VTABLE) return false;
    memcpy(out, (const float *)(uintptr_t)(b + 0x90), 12);
    return true;
}

uint64_t warp_player_body(void) {  // (the player's body object, 0 before it is found: hle/pla_world.c)
    uint64_t b = g_seen_body;
    return b && *(const uint64_t *)(uintptr_t)b == BODY_VTABLE ? b : 0;
}

// ---- Is the online server there? (the relay plugin's larecomp_server_state: every hello it sends is answered -
// lzaserver - so it knows within ~8 s). The player is told when it can't be reached, when it drops, when this copy has
// to sign in to play there (an lzaserver lets only signed-in players in: runtime/acct.c) and when it's back.
void menu_notice_for(const char *text, unsigned ms);
static void server_notice_frame(uint64_t frame) {
    static int (*state)(char *, size_t);
    static uint64_t at, told_at;
    static int was = -1, tries;
    if (frame - at < 60) return;
    at = frame;
    if (!state) {
        if (tries > 30) return;  // (no relay plugin with it - an older one, or none: looked for 30 s, then not again)
        tries++;
        void *plugins_symbol(const char *name);
        if (!(state = (int (*)(char *, size_t))plugins_symbol("larecomp_server_state"))) return;
    }
    char name[256] = "";
    int st = state(name, sizeof name);
    bool away = st == 3 || st == 4 || st == 5;
    if (st == was && !(away && frame - told_at >= 7200)) return;
    char t[400];
    const char *lt = "";  // (the log's words: never the server's address)
    t[0] = 0;
    if (st == 3) snprintf(t, sizeof t, "Can't reach the online server (%s) - online play waits for it. Is the address right, and the server running?", name), lt = "can't reach the server";
    else if (st == 4) snprintf(t, sizeof t, "Lost the connection to the online server (%s) - trying again...", name), lt = "lost the server";
    else if (st == 5) snprintf(t, sizeof t, "The online server's name (%s) can't be found - check the address (launcher > Options > Online) and your connection.", name), lt = "the server's name doesn't resolve";
    else if (st == 6) snprintf(t, sizeof t, "Sign in to %s to play online there: launcher > Options > Online. (Playing offline needs no account.)", name), lt = "sign in to play on that server";
    else if (st == 2 && was == 6) snprintf(t, sizeof t, "Signed in on %s: online play is on.", name), lt = "signed in: online play on";
    // (back after it couldn't be reached: online play just carries on - nothing to tell, the notice said it waits for it)
    if (t[0]) menu_notice_for(t, st == 2 ? 5000 : st == 6 ? 12000 : 10000), told_at = frame, LOG("[online] told: %s\n", lt);
    else if (st != was) LOG("[online] the server: %s\n", st == 1 ? "asked, no answer yet" : st != 2 ? "not in use" : was == 4 ? "answering again (not told)" : was == 3 || was == 5 ? "reached (not told)" : "answering");
    was = st;
}

void warp_frame(uint64_t frame) {  // (each game frame: runtime/hle/input.c)
    g_frame_now = frame;
    if (g_api) { extern void pla_npc_api_frame(uint64_t); pla_npc_api_frame(frame); }
    if (g_tp_n > 0) {  // (a script's teleport)
        uint64_t b = g_seen_body;
        if (!b || *(const uint64_t *)(uintptr_t)b != BODY_VTABLE) g_tp_n = 0, LOG("[warp] teleport: the player has no body yet\n");
        else if (memcpy((void *)(uintptr_t)(b + 0x90), g_tp, 12), --g_tp_n == 0) LOG("[warp] the player put at %.2f %.2f %.2f\n", g_tp[0], g_tp[1], g_tp[2]);
    }
    server_notice_frame(frame);
    { extern void pla_slot_watch(uint64_t); pla_slot_watch(frame); }  // (research: hle/pla_drawdist.c)
    {   // (research) LARECOMP_LOG_PLAYER=<n>: the player's position every n frames ("[player] frame f x y z")
        static long every = -2;
        if (every == -2) every = getenv("LARECOMP_LOG_PLAYER") ? atol(getenv("LARECOMP_LOG_PLAYER")) : 0;
        float p[3];
        if (every > 0 && frame % (uint64_t)every == 0 && warp_player_pos(p))
            LOG("[player] frame %llu %.2f %.2f %.2f\n", (unsigned long long)frame, p[0], p[1], p[2]);
    }
    { extern void pla_world_frame(uint64_t); pla_world_frame(frame); }
    if (g_state < 0 || g_state >= 2) return;
    static uint64_t body, since, still_since, writes;
    static float last[3];
    uint64_t b = g_body;
    if (!b) return;
    float *pos = (float *)(uintptr_t)(b + 0x90);
    if (b != body || *(const uint64_t *)(uintptr_t)b != BODY_VTABLE) {  // (a new body: the area loaded again)
        if (b != body) LOG("[warp] the player's body %llx, at %.2f %.2f %.2f\n", (unsigned long long)b, pos[0], pos[1], pos[2]);
        body = b, since = still_since = frame, memcpy(last, pos, 12), g_state = 0;
        return;
    }
    if (g_state == 0) {
        if (fabsf(pos[0] - last[0]) + fabsf(pos[1] - last[1]) + fabsf(pos[2] - last[2]) > 1e-3f) still_since = frame, memcpy(last, pos, 12);
        if (frame - since < 120 || frame - still_since < 60) return;
        if (g_area_state == 1) {  // (the save's area is up: into the other one first, then the warp once it's there)
            g_area_state = 2, body = 0, g_body = 0;
            return;
        }
        if (g_works_state == 1) works_write();  // (no area change: the story point before the warp)
        g_state = 1, writes = 0;
        LOG("[warp] the player moved from %.2f %.2f %.2f to %.2f %.2f %.2f\n", pos[0], pos[1], pos[2], g_to[0], g_to[1], g_to[2]);
    }
    memcpy(pos, g_to, 12);
    if (++writes >= 10) g_state = 2;
}
