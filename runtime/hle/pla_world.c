// Shared world, first prototype (Legends: Arceus 1.1.1; research, LARECOMP_WORLD=1): the other players of an online
// server (contrib/lzaserver, the relay plugin's shared-world channel larecomp_world_*) shown in the field, each drawn
// over a character of the game standing near us - a stand-in.
//
// How Arceus draws a character (LegendsVR's body.c): 0x7002a6ba60(Skeleton *sk) poses it - the bones' matrices (model
// space) and their skinning palette - and its drawn place in the world is the matrix at sk+0x410: x, y, z axes at
// +0x410/+0x420/+0x430, position at +0x440 (written from the character's body there each pose; the body object, vtable
// 0x7004187048, keeps its own position at +0x90). So the stand-in is whichever skeleton posed near us that isn't the
// player's: after each of its poses its drawn matrix is put where the other player is, turned their way. Its body, its
// collision, its talk prompt and its own animation stay as the game has them (the Z-A version's dressed townspeople,
// walk animation and collision are not ported: see runtime/hle/warp.c in LegendsZA's online-za / accounts-za).
// The player: the skeleton posed where the player's body is (warp.c's warp_player_pos); its facing = its z axis.
//
// Packets (up to 96 bytes through the server to the same game's world members): 'P' '1' u16 seq, f32 x y z yaw - 20 B,
// sent every second game frame while we have a body. LARECOMP_GHOST_SELF=<m>: a single-copy test - the "other player"
// is ourselves, <m> metres ahead (nothing sent or heard).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);
bool warp_player_pos(float out[3]);
uint64_t warp_player_body(void);
void *plugins_symbol(const char *name);

#define POSE_FN 0x7002a6ba60ull
#define SK_WORLD 0x410

static int g_on = -1;
static float g_self_ahead;  // (LARECOMP_GHOST_SELF)
static volatile uint64_t g_frame;

// skeletons posed lately (by pointer hash): the stand-in is picked among them
#define NSK 1024
static struct { volatile uint64_t sk; volatile float p[3]; volatile uint64_t frame; volatile uint32_t bones; } g_sk[NSK];
static __thread uint64_t t_posing;  // (the skeleton being posed on this thread)
static __thread uint32_t t_bones;   // (the most bones one skinning of it had: a person's body has 74 - the props' few)
static volatile uint64_t g_player_sk;
static volatile float g_me[4];  // our position and facing (radians, from the player's skeleton)
static volatile uint64_t g_me_frame;

static volatile uint64_t g_standin;
static volatile float g_draw[4];  // where the stand-in is drawn (smoothed toward the heard position)
static volatile int g_show;

static struct { uint64_t id; float tgt[4]; uint64_t heard_ns; uint16_t seq; int have; unsigned n; } g_peer;

static bool in_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX; }

// the stand-in's body object (BODY_VTABLE): the posing places the model from it (+0x60 axes, +0x90 position), so it is
// moved there before each pose; what it was is put back when the stand-in is let go
#define BODY_VTABLE 0x7004187048ull
static volatile uint64_t g_body;
static float g_body_was[16], g_body_off[3];  // (its matrix when taken; skeleton minus body position)

static void put(float *m, const float *at, float yaw) {  // (axes turned to yaw about y, keeping their lengths; position)
    float sx = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), sy = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]),
          sz = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    float cy = cosf(yaw), sn = sinf(yaw);
    m[0] = cy * sx, m[1] = 0, m[2] = -sn * sx;
    m[4] = 0, m[5] = sy, m[6] = 0;
    m[8] = sn * sz, m[9] = 0, m[10] = cy * sz;
    m[12] = at[0], m[13] = at[1], m[14] = at[2];
}

static unsigned sk_slot(uint64_t sk, uint64_t f) {  // (its entry, or a free / stale one for it)
    unsigned h = (unsigned)((sk * 0x9E3779B97F4A7C15ull) >> 54), free_at = NSK;
    for (unsigned k = 0; k < 16; k++) {
        unsigned i = (h + k) % NSK;
        if (g_sk[i].sk == sk) return i;
        if (free_at == NSK && (!g_sk[i].sk || f - g_sk[i].frame > 120)) free_at = i;
    }
    return free_at < NSK ? free_at : h;
}

static int pose_hook(Cpu *c, uint64_t addr) {
    uint64_t sk = c->x[0], body = g_body;
    bool mine = sk == g_standin && g_show && in_heap(sk);
    if (mine && body && *(const uint64_t *)(uintptr_t)body == BODY_VTABLE) {
        float at[3] = {g_draw[0] - g_body_off[0], g_draw[1] - g_body_off[1], g_draw[2] - g_body_off[2]};
        put((float *)(uintptr_t)(body + 0x60), at, g_draw[3]);
    }
    t_posing = sk, t_bones = 0;
    rt_call_original(c, addr);
    t_posing = 0;
    if (!in_heap(sk)) return 1;
    float *m = (float *)(uintptr_t)(sk + SK_WORLD);
    uint64_t f = g_frame;
    if (mine) put(m, (const float *)g_draw, g_draw[3]);
    unsigned h = sk_slot(sk, f);
    g_sk[h].sk = sk, g_sk[h].p[0] = m[12], g_sk[h].p[1] = m[13], g_sk[h].p[2] = m[14], g_sk[h].frame = f, g_sk[h].bones = t_bones;
    float bp[3];
    // (the player's: posed where its body is - the drawn model can sit up to ~0.6 m off the body's point after a warp)
    if (t_bones >= 60 && warp_player_pos(bp) && fabsf(m[12] - bp[0]) + fabsf(m[14] - bp[2]) < 0.5f && fabsf(m[13] - bp[1]) < 1.5f) {
        g_player_sk = sk;
        // (facing: the body object's z axis - the skeleton's drawn axes don't turn with the player)
        uint64_t b = warp_player_body();
        const float *bm = b ? (const float *)(uintptr_t)(b + 0x60) : m;
        g_me[0] = m[12], g_me[1] = m[13], g_me[2] = m[14], g_me[3] = atan2f(bm[8], bm[10]), g_me_frame = f;
    }
    return 1;
}

static void let_go(void) {  // (the stand-in's body back where it was)
    uint64_t b = g_body;
    g_body = 0;
    if (b && *(const uint64_t *)(uintptr_t)b == BODY_VTABLE) memcpy((float *)(uintptr_t)(b + 0x60), g_body_was, 64);
}

// the body object of the character posed at p: a heap scan for BODY_VTABLE objects whose +0x90 is within 1 m of it
static uint64_t body_at(const float *p) {
    extern uint64_t kernel_heap_size(void), g_stdheap_used;
    uint64_t top = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used : 0, heap = HEAP_BASE + kernel_heap_size();
    uint64_t hi = top > heap ? top : heap, best = 0;
    float bd = 1.0f;
    for (const uint64_t *q = (const uint64_t *)(uintptr_t)HEAP_BASE, *e = (const uint64_t *)(uintptr_t)(hi - 0xa0); q < e; q += 2) {
        if (*q != BODY_VTABLE) continue;
        const float *bp = (const float *)(q + 0x12);  // (+0x90)
        float d = fabsf(bp[0] - p[0]) + fabsf(bp[2] - p[2]);
        if (d < bd && fabsf(bp[1] - p[1]) < 2.0f) bd = d, best = (uint64_t)(uintptr_t)q;
    }
    return best;
}

// 0x70029c983c(skinning, inverse binds, bones, slots, w4 count): the palette of one skinned mesh of the skeleton posed
static int palette_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (t_posing && (uint32_t)c->x[4] > t_bones && (uint32_t)c->x[4] < 4096) t_bones = (uint32_t)c->x[4];
    return 0;
}

static int (*w_avail)(void);
static void (*w_enable)(int);
static void (*w_send)(const void *, size_t);
static int (*w_recv)(uint64_t *, void *, size_t);

static void pick_standin(void) {
    uint64_t f = g_frame, best = 0, cur = g_standin, me = g_player_sk;
    float bd = 1e30f;
    bool cur_ok = false;
    float cd = 0;
    for (int i = 0; i < NSK; i++) {
        uint64_t sk = g_sk[i].sk;
        if (!sk || sk == me || f - g_sk[i].frame > 3 || g_sk[i].bones < 60) continue;  // (people only)
        float dx = g_sk[i].p[0] - g_me[0], dy = g_sk[i].p[1] - g_me[1], dz = g_sk[i].p[2] - g_me[2];
        float d = sqrtf(dx * dx + dz * dz);
        if ((d < 1.5f && sk != cur) || d > 35.0f || fabsf(dy) > 6.0f) continue;  // (ours is drawn where the other player is)
        if (sk == cur) cur_ok = true, cd = d;
        if (d < bd) bd = d, best = sk;
    }
    if (cur_ok && cd < bd + 10.0f) return;  // (it is still posed near us, not much farther than the nearest: kept)
    if (best == cur) return;
    let_go();
    g_standin = best;
    uint64_t body = 0;
    float p[3] = {0};
    for (int i = 0; best && i < NSK; i++)
        if (g_sk[i].sk == best) p[0] = g_sk[i].p[0], p[1] = g_sk[i].p[1], p[2] = g_sk[i].p[2], body = body_at(p);
    if (body) {
        memcpy(g_body_was, (const float *)(uintptr_t)(body + 0x60), 64);
        g_body_off[0] = p[0] - g_body_was[12], g_body_off[1] = p[1] - g_body_was[13], g_body_off[2] = p[2] - g_body_was[14];
        g_body = body;
    }
    LOG("[world] stand-in: skeleton %llx at %.2f %.2f %.2f, %.1f m from us%s, its body %llx at %.2f %.2f %.2f\n", (unsigned long long)best,
        p[0], p[1], p[2], bd, best ? "" : " (none near)", (unsigned long long)body, g_body_was[12], g_body_was[13], g_body_was[14]);
}

static void world_tick(uint64_t frame) {
    static bool init;
    if (!init) {
        init = true;
        if (!g_self_ahead) {
            w_avail = (int (*)(void))plugins_symbol("larecomp_world_available");
            w_enable = (void (*)(int))plugins_symbol("larecomp_world_enable");
            w_send = (void (*)(const void *, size_t))plugins_symbol("larecomp_world_send");
            w_recv = (int (*)(uint64_t *, void *, size_t))plugins_symbol("larecomp_world_recv");
            LOG("[world] shared world prototype: %s\n", w_avail && w_avail() ? "through the online server" : "no online server (relay plugin + online_server needed)");
        } else LOG("[world] shared world prototype: single-copy test, ourselves %.1f m ahead\n", g_self_ahead);
    }
    uint64_t now = plat_now_ns();
    bool me_ok = frame - g_me_frame < 10;
    if (w_avail && w_avail()) {
        static bool enabled;
        if (!enabled) w_enable(1), enabled = true;
        static uint16_t seq;
        if (me_ok && !(frame & 1)) {
            uint8_t b[20] = {'P', '1'};
            float v[4] = {g_me[0], g_me[1], g_me[2], g_me[3]};
            seq++;
            memcpy(b + 2, &seq, 2), memcpy(b + 4, v, 16);
            w_send(b, sizeof b);
        }
        uint8_t b[96];
        uint64_t from;
        int n;
        while ((n = w_recv(&from, b, sizeof b)) > 0) {
            if (n < 20 || b[0] != 'P' || b[1] != '1') continue;
            float v[4];
            memcpy(v, b + 4, 16);
            if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]) || !isfinite(v[3]) || fabsf(v[0]) > 1e5f || fabsf(v[1]) > 1e5f || fabsf(v[2]) > 1e5f) continue;
            if (g_peer.have && from != g_peer.id && now - g_peer.heard_ns < 5000000000ull) continue;  // (one other player in this prototype)
            uint16_t s;
            memcpy(&s, b + 2, 2);
            if (g_peer.have && from == g_peer.id && (int16_t)(s - g_peer.seq) <= 0) continue;  // (late: an older one)
            if (!g_peer.have || from != g_peer.id) LOG("[world] hearing player %016llx at %.2f %.2f %.2f\n", (unsigned long long)from, v[0], v[1], v[2]);
            g_peer.id = from, g_peer.seq = s, memcpy(g_peer.tgt, v, 16), g_peer.heard_ns = now, g_peer.have = 1, g_peer.n++;
        }
    } else if (g_self_ahead && me_ok) {
        g_peer.tgt[0] = g_me[0] + sinf(g_me[3]) * g_self_ahead, g_peer.tgt[1] = g_me[1];
        g_peer.tgt[2] = g_me[2] + cosf(g_me[3]) * g_self_ahead, g_peer.tgt[3] = g_me[3];
        g_peer.heard_ns = now, g_peer.have = 1, g_peer.n++;
    }
    bool live = g_peer.have && now - g_peer.heard_ns < 5000000000ull && me_ok;
    if (!live) {
        if (g_show) LOG("[world] the other player went quiet: stand-in let go\n"), let_go(), g_standin = 0;
        g_show = 0;
        return;
    }
    if (frame % 15 == 0 || !g_standin) pick_standin();
    // smoothing toward the newest heard (a jump over 8 m: at once)
    static uint64_t last_ns;
    float dt = last_ns ? (float)(now - last_ns) * 1e-9f : 0.033f;
    last_ns = now;
    float k = 1.0f - expf(-dt * 12.0f);
    float dx = g_peer.tgt[0] - g_draw[0], dy = g_peer.tgt[1] - g_draw[1], dz = g_peer.tgt[2] - g_draw[2];
    if (!g_show || dx * dx + dy * dy + dz * dz > 64.0f) k = 1.0f;
    g_draw[0] += dx * k, g_draw[1] += dy * k, g_draw[2] += dz * k;
    float dyaw = remainderf(g_peer.tgt[3] - g_draw[3], 6.2831853f);
    g_draw[3] = k >= 1.0f ? g_peer.tgt[3] : g_draw[3] + dyaw * k;
    if (!g_show && g_standin) LOG("[world] showing the other player on skeleton %llx at %.2f %.2f %.2f\n", (unsigned long long)g_standin, g_draw[0], g_draw[1], g_draw[2]);
    g_show = g_standin != 0;
    static uint64_t last_log;
    if (frame - last_log >= 300) {
        last_log = frame;
        static int list = -1;  // (research LARECOMP_WORLD_LIST=1: the skeletons posed near us)
        if (list < 0) list = getenv("LARECOMP_WORLD_LIST") != NULL;
        for (int i = 0; list && i < NSK; i++) {
            uint64_t sk = g_sk[i].sk;
            if (!sk || frame - g_sk[i].frame > 3) continue;
            float dx = g_sk[i].p[0] - g_me[0], dy = g_sk[i].p[1] - g_me[1], dz = g_sk[i].p[2] - g_me[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > 40) continue;
            const float *m = (const float *)(uintptr_t)(sk + SK_WORLD);
            LOG("[world]   skeleton %llx at %.2f %.2f %.2f (%.1f m)%s yaw %.0f scale %.2f bones %u\n", (unsigned long long)sk, g_sk[i].p[0], g_sk[i].p[1], g_sk[i].p[2], d,
                sk == g_player_sk ? " = player" : sk == g_standin ? " = stand-in" : "", atan2f(m[8], m[10]) * 57.2958f, sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), g_sk[i].bones);
        }
        LOG("[world] me %.2f %.2f %.2f yaw %.0f | other %.2f %.2f %.2f yaw %.0f (%u heard) | stand-in %llx\n", g_me[0], g_me[1], g_me[2],
            g_me[3] * 57.2958f, g_draw[0], g_draw[1], g_draw[2], g_draw[3] * 57.2958f, g_peer.n, (unsigned long long)g_standin);
    }
}

void pla_world_frame(uint64_t frame) {  // (each game frame: warp.c's warp_frame)
    if (g_on <= 0) return;
    g_frame = frame;
    world_tick(frame);
}

int pla_world_enabled(void) {
    if (g_on < 0) {
        const char *e = getenv("LARECOMP_WORLD"), *s = getenv("LARECOMP_GHOST_SELF");
        g_self_ahead = s ? (float)atof(s) : 0;
        g_on = (e && atoi(e) > 0) || g_self_ahead != 0;
    }
    return g_on;
}

__attribute__((constructor)) static void pla_world_init(void) {
    if (!pla_world_enabled()) return;
    if (getenv("LARECOMP_HIDE_PLAYER")) { g_on = 0; return; }  // (that research hooks the same posing)
    rt_register_fnhook_at(POSE_FN, pose_hook);
    rt_register_fnhook_at(0x70029c983cull, palette_hook);
}
