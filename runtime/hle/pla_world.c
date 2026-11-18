// Shared world (Legends: Arceus 1.1.1; research, LARECOMP_WORLD=1): the other players of an online server
// (contrib/lzaserver, the relay plugin's shared-world channel larecomp_world_*) shown in the field.
//
// Each of the nearest SHOW players within OWN_RANGE gets a person of our own, made by the game (hle/pla_npc.c: a copy
// of one of the level's placements, its character the survey corps senior of that player's gender - the default player
// look) and walked by the game's own navigation to where they are drawn: a little in their past, between the two
// heard positions around that moment (the drawn delay follows the packets' lateness, 60..400 ms), walking or running by
// their speed. A player who leaves or goes far: their person walks off and is kept for the next one of that look.
//
// Fallback (a level without placements to copy, or before ours is up): a townsperson standing near borrowed for the
// nearest such player, as the first prototype did: Arceus places a character's drawn model from its body object
// (vtable 0x7004187048: +0x60 axes, +0x90 position) when it poses its skeleton (0x7002a6ba60(sk); drawn matrix at
// sk+0x410/+0x440), so that body is moved there before each pose and its matrix after (body found by a heap scan).
// The player: the skeleton posed where the player's body is (warp.c's warp_player_pos); its facing = the body's z axis.
//
// Packets (through the server to the same game's world members): 'P' '2' u16 seq, u32 sender ms, f32 x y z yaw speed,
// u8 body (0 p1, 1 p2) - 29 B on a 50 ms beat while we have a body. LARECOMP_GHOST_SELF=<m>: a single-copy test - the
// "other player" is ourselves, <m> metres ahead (LARECOMP_GHOST_GENDER=0|1 its look; nothing sent or heard).
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
    // kept while it is still posed (we move its body, so wherever the other player goes it comes along); else the
    // person posed within 40 m of us nearest to where the other player is
    uint64_t f = g_frame, best = 0, cur = g_standin, me = g_player_sk;
    float bd = 1e30f;
    for (int i = 0; i < NSK; i++) {
        uint64_t sk = g_sk[i].sk;
        if (sk == cur && sk != me && f - g_sk[i].frame < 60) return;  // (a person right by the camera isn't posed every frame)
        if (!sk || sk == me || f - g_sk[i].frame > 3 || g_sk[i].bones < 60) continue;  // (people only)
        float dx = g_sk[i].p[0] - g_me[0], dy = g_sk[i].p[1] - g_me[1], dz = g_sk[i].p[2] - g_me[2];
        float d = sqrtf(dx * dx + dz * dz);
        if (d < 1.5f || d > 40.0f || fabsf(dy) > 8.0f) continue;
        float ex = g_sk[i].p[0] - g_peer.tgt[0], ez = g_sk[i].p[2] - g_peer.tgt[2], e = sqrtf(ex * ex + ez * ez);
        if (e < bd) bd = e, best = sk;
    }
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
    LOG("[world] stand-in: skeleton %llx at %.2f %.2f %.2f, %.1f m from the other player%s, its body %llx at %.2f %.2f %.2f\n", (unsigned long long)best,
        p[0], p[1], p[2], bd, best ? "" : " (none near)", (unsigned long long)body, g_body_was[12], g_body_was[13], g_body_was[14]);
}

// ---- the other players: heard positions, drawn a little in their past, each walked by a person of our own
// (hle/pla_npc.c: made by the game, the survey corps senior of their gender, its navigator sent where they are)
#define MAXPEER 8
#define NSNAP 24
#define SHOW 4            // (the nearest SHOW players within OWN_RANGE get a person)
#define OWN_RANGE 50.0f
typedef struct { uint32_t ms; float p[3], yaw, speed; } Snap;
typedef struct {
    uint64_t id, heard_ns, first_ns;
    int have, gender, own, n_snap, borrowed, own_up;
    uint64_t retry_ns;     // (no new person asked for before then: the last never came up)
    uint16_t seq;
    Snap snap[NSNAP];      // (newest last)
    int64_t off_min;       // (min over the newest of local ms - sender ms)
    float lat[32];         // (lateness of the newest packets past that)
    int nlat;
    float behind;          // (ms drawn in their past)
    float tgt[5];          // (x y z yaw speed, drawn now)
    uint64_t order_frame;
    float ordered[3];
    unsigned heard;
} Peer;
static Peer g_peers[MAXPEER];

static uint64_t now_ms(void) { return plat_now_ns() / 1000000ull; }

static void peer_take(Peer *p, uint32_t ms, const float v[4], float speed, int gender) {
    uint64_t now = now_ms();
    int64_t off = (int64_t)now - (int64_t)ms;
    if (!p->n_snap || off < p->off_min) p->off_min = off;
    p->lat[p->nlat++ % 32] = (float)(off - p->off_min);
    if (p->n_snap && (int32_t)(ms - p->snap[p->n_snap - 1].ms) > 1000) p->n_snap = 0, p->off_min = off;  // (a gap: menus, a load)
    if (p->n_snap == NSNAP) memmove(p->snap, p->snap + 1, sizeof(Snap) * (NSNAP - 1)), p->n_snap--;
    Snap *s = &p->snap[p->n_snap++];
    s->ms = ms, memcpy(s->p, v, 12), s->yaw = v[3], s->speed = speed;
    p->gender = gender;
}

// where to draw them now: between the two snapshots around (now - behind) in their clock
static void peer_draw(Peer *p) {
    if (!p->n_snap) return;
    float want = 0;
    int n = p->nlat < 32 ? p->nlat : 32;
    for (int i = 0; i < n; i++) want = p->lat[i] > want ? p->lat[i] : want;
    float gap = p->n_snap > 1 ? (float)(p->snap[p->n_snap - 1].ms - p->snap[p->n_snap - 2].ms) : 70;
    want = want + 1.2f * gap + 20;
    want = want < 60 ? 60 : want > 400 ? 400 : want;
    p->behind = p->behind ? p->behind + (want - p->behind) * 0.1f : want;
    int64_t t = (int64_t)now_ms() - p->off_min - (int64_t)p->behind;
    const Snap *a = &p->snap[0], *b = NULL;
    for (int i = 0; i < p->n_snap; i++) {
        if ((int64_t)p->snap[i].ms <= t) a = &p->snap[i];
        else { b = &p->snap[i]; break; }
    }
    if (!b || (int64_t)a->ms > t) {  // (before the first or past the newest: there)
        const Snap *s = b && (int64_t)a->ms > t ? a : &p->snap[p->n_snap - 1];
        memcpy(p->tgt, s->p, 12), p->tgt[3] = s->yaw, p->tgt[4] = s->speed;
        return;
    }
    float k = (float)(t - (int64_t)a->ms) / (float)(b->ms - a->ms ? b->ms - a->ms : 1);
    for (int j = 0; j < 3; j++) p->tgt[j] = a->p[j] + (b->p[j] - a->p[j]) * k;
    p->tgt[3] = a->yaw + remainderf(b->yaw - a->yaw, 6.2831853f) * k, p->tgt[4] = a->speed + (b->speed - a->speed) * k;
}

static void peer_let_go(Peer *p, const char *why) {
    if (p->own >= 0) {
        extern void pla_npc_order(int, const float *, int);
        extern void pla_npc_free(int);
        // (it walks off away from us, the way they were, and is kept for whoever comes next)
        float dx = p->tgt[0] - g_me[0], dz = p->tgt[2] - g_me[2], d = sqrtf(dx * dx + dz * dz);
        float to[3] = {p->tgt[0] + (d > 0.1f ? dx / d : 1) * 25, p->tgt[1], p->tgt[2] + (d > 0.1f ? dz / d : 0) * 25};
        pla_npc_order(p->own, to, 0);
        pla_npc_free(p->own);
        LOG("[world] player %016llx %s: its person walks off\n", (unsigned long long)p->id, why);
    }
    p->own = -1, p->own_up = 0;
}

static void world_tick(uint64_t frame) {
    static bool init;
    extern int pla_npc_gender(void), pla_npc_new(const float *, int), pla_npc_state(int), pla_npc_take(int);
    extern void pla_npc_order(int, const float *, int);
    if (!init) {
        init = true;
        for (int i = 0; i < MAXPEER; i++) g_peers[i].own = -1;
        if (!g_self_ahead) {
            w_avail = (int (*)(void))plugins_symbol("larecomp_world_available");
            w_enable = (void (*)(int))plugins_symbol("larecomp_world_enable");
            w_send = (void (*)(const void *, size_t))plugins_symbol("larecomp_world_send");
            w_recv = (int (*)(uint64_t *, void *, size_t))plugins_symbol("larecomp_world_recv");
            LOG("[world] shared world: %s\n", w_avail && w_avail() ? "through the online server" : "no online server (relay plugin + online_server needed)");
        } else LOG("[world] shared world: single-copy test, ourselves %.1f m ahead\n", g_self_ahead);
    }
    uint64_t now = plat_now_ns();
    bool me_ok = g_me_frame && frame - g_me_frame < 10;
    // our speed (m/s, eased)
    static float last_me[3], my_speed;
    static uint64_t last_me_ns;
    if (me_ok) {
        float dt = last_me_ns ? (float)(now - last_me_ns) * 1e-9f : 0;
        if (dt > 0.02f) {
            float dx = g_me[0] - last_me[0], dz = g_me[2] - last_me[2], v = sqrtf(dx * dx + dz * dz) / dt;
            my_speed += ((v > 20 ? my_speed : v) - my_speed) * 0.3f;
            last_me[0] = g_me[0], last_me[1] = g_me[1], last_me[2] = g_me[2], last_me_ns = now;
        } else if (!last_me_ns) last_me[0] = g_me[0], last_me[1] = g_me[1], last_me[2] = g_me[2], last_me_ns = now;
    }
    int gender = pla_npc_gender();
    if (w_avail && w_avail()) {
        static bool enabled;
        if (!enabled) w_enable(1), enabled = true;
        static uint16_t seq;
        static uint64_t sent_ms;
        uint64_t ms = now_ms();
        if (me_ok && ms - sent_ms >= 50) {  // (on a 50 ms beat)
            sent_ms = ms;
            uint8_t b[32] = {'P', '2'};
            uint32_t m32 = (uint32_t)ms;
            float v[5] = {g_me[0], g_me[1], g_me[2], g_me[3], my_speed};
            seq++;
            memcpy(b + 2, &seq, 2), memcpy(b + 4, &m32, 4), memcpy(b + 8, v, 20);
            b[28] = gender > 0;
            w_send(b, 29);
        }
        uint8_t b[96];
        uint64_t from;
        int n;
        while ((n = w_recv(&from, b, sizeof b)) > 0) {
            if (n < 29 || b[0] != 'P' || b[1] != '2') continue;
            float v[5];
            uint16_t s;
            uint32_t sm;
            memcpy(&s, b + 2, 2), memcpy(&sm, b + 4, 4), memcpy(v, b + 8, 20);
            bool bad = false;
            for (int j = 0; j < 5; j++) bad |= !isfinite(v[j]) || fabsf(v[j]) > 1e5f;
            if (bad || v[4] < 0 || v[4] > 100) continue;
            Peer *p = NULL, *fr = NULL;
            for (int i = 0; i < MAXPEER; i++) {
                if (g_peers[i].have && g_peers[i].id == from) { p = &g_peers[i]; break; }
                if (!g_peers[i].have && !fr) fr = &g_peers[i];
            }
            if (!p) {
                if (!fr) continue;  // (MAXPEER heard already)
                p = fr;
                memset(p, 0, sizeof *p);
                p->id = from, p->have = 1, p->own = -1, p->first_ns = now;
                LOG("[world] hearing player %016llx at %.2f %.2f %.2f (%s body)\n", (unsigned long long)from, v[0], v[1], v[2], b[28] ? "p2" : "p1");
            } else if ((int16_t)(s - p->seq) <= 0) continue;  // (an older one, overtaken)
            p->seq = s, p->heard_ns = now, p->heard++;
            peer_take(p, sm, v, v[4], b[28] != 0);
        }
    } else if (g_self_ahead && me_ok) {  // (the single-copy test: ourselves, g_self_ahead m ahead, on the same beat)
        static uint64_t sent_ms;
        uint64_t ms = now_ms();
        if (ms - sent_ms >= 50) {
            sent_ms = ms;
            Peer *p = &g_peers[0];
            if (!p->have) memset(p, 0, sizeof *p), p->have = 1, p->id = 1, p->own = -1, p->first_ns = now;
            float v[4] = {g_me[0] + sinf(g_me[3]) * g_self_ahead, g_me[1], g_me[2] + cosf(g_me[3]) * g_self_ahead, g_me[3]};
            static int sg = -2;
            if (sg == -2) sg = getenv("LARECOMP_GHOST_GENDER") ? atoi(getenv("LARECOMP_GHOST_GENDER")) : -1;
            p->heard_ns = now, p->heard++;
            peer_take(p, (uint32_t)ms, v, my_speed, sg >= 0 ? sg : gender > 0);
        }
    }
    // (people are made only once we have stood in the world a while: not mid-load, not right after a warp)
    static float jump_at[3];
    static uint64_t jump_ns, valid_ns;
    if (!me_ok) valid_ns = 0;
    else if (!valid_ns) valid_ns = now;
    if (me_ok && fabsf(g_me[0] - jump_at[0]) + fabsf(g_me[2] - jump_at[2]) > 8.0f) jump_at[0] = g_me[0], jump_at[2] = g_me[2], jump_ns = now;
    bool settled = valid_ns && now - valid_ns > 8000000000ull && now - jump_ns > 3000000000ull;
    // who is shown: the nearest SHOW within OWN_RANGE
    int order[MAXPEER], no = 0;
    for (int i = 0; i < MAXPEER; i++) {
        Peer *p = &g_peers[i];
        if (!p->have) continue;
        if (now - p->heard_ns > 5000000000ull) {
            LOG("[world] player %016llx went quiet\n", (unsigned long long)p->id);
            peer_let_go(p, "left");
            if (p->borrowed) let_go(), g_standin = 0, g_show = 0;
            p->have = 0;
            continue;
        }
        peer_draw(p);
        order[no++] = i;
    }
    for (int i = 1; i < no; i++)  // (nearest first)
        for (int j = i; j > 0; j--) {
            Peer *a = &g_peers[order[j - 1]], *b = &g_peers[order[j]];
            float da = (a->tgt[0] - g_me[0]) * (a->tgt[0] - g_me[0]) + (a->tgt[2] - g_me[2]) * (a->tgt[2] - g_me[2]);
            float db = (b->tgt[0] - g_me[0]) * (b->tgt[0] - g_me[0]) + (b->tgt[2] - g_me[2]) * (b->tgt[2] - g_me[2]);
            if (db < da) { int t = order[j]; order[j] = order[j - 1], order[j - 1] = t; }
        }
    Peer *borrow = NULL;
    for (int r = 0; r < no; r++) {
        Peer *p = &g_peers[order[r]];
        float dx = p->tgt[0] - g_me[0], dy = p->tgt[1] - g_me[1], dz = p->tgt[2] - g_me[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        bool want = r < SHOW && d < OWN_RANGE && me_ok;
        if (!want) { if (p->own >= 0) peer_let_go(p, d >= OWN_RANGE ? "is far" : "isn't among the nearest"); continue; }
        if (p->own >= 0 && !pla_npc_state(p->own)) {  // (gone with a level, or never made)
            LOG("[world] player %016llx's person is gone%s\n", (unsigned long long)p->id, p->own_up ? ": another" : " before it came up: another in 10 s");
            if (!p->own_up) p->retry_ns = now + 10000000000ull;
            p->own = -1, p->own_up = 0;
        }
        if (p->own >= 0 && pla_npc_state(p->own) == 3) p->own_up = 1;
        if (p->own < 0 && settled && now >= p->retry_ns) {
            int t = pla_npc_take(p->gender);
            p->own = t >= 0 ? t : pla_npc_new(p->tgt, p->gender);
            if (p->own >= 0) LOG("[world] player %016llx gets %s person %d (the %s senior) at %.2f %.2f %.2f, %.1f m from us\n", (unsigned long long)p->id,
                                 t >= 0 ? "the free" : "a new", p->own, p->gender ? "female" : "male", p->tgt[0], p->tgt[1], p->tgt[2], d);
            p->order_frame = 0;
        }
        if (p->own >= 0 && pla_npc_state(p->own) == 3 && frame - p->order_frame >= 6) {  // (5 orders a second)
            float mx = p->tgt[0] - p->ordered[0], mz = p->tgt[2] - p->ordered[2];
            if (!p->order_frame || mx * mx + mz * mz > 0.01f || frame - p->order_frame >= 60) {
                pla_npc_order(p->own, p->tgt, p->tgt[4] > 3.6f);
                memcpy(p->ordered, p->tgt, 12), p->order_frame = frame;
            }
        }
        // (a level without placements to copy: a townsperson borrowed for the nearest such player - the old way)
        if (p->own < 0 && !borrow && now - p->first_ns > 3000000000ull) borrow = p;
    }
    // the borrowed stand-in (fallback): as before, for one player
    for (int i = 0; i < MAXPEER; i++) g_peers[i].borrowed = &g_peers[i] == borrow;
    if (!borrow) {
        if (g_show) LOG("[world] borrowed stand-in let go\n"), let_go(), g_standin = 0;
        g_show = 0;
    } else {
        memcpy(g_peer.tgt, borrow->tgt, 16);
        if (frame % 15 == 0) pick_standin();
        static uint64_t last_ns;
        float dt = last_ns ? (float)(now - last_ns) * 1e-9f : 0.033f;
        last_ns = now;
        float k = 1.0f - expf(-dt * 12.0f);
        float dx = g_peer.tgt[0] - g_draw[0], dy = g_peer.tgt[1] - g_draw[1], dz = g_peer.tgt[2] - g_draw[2];
        if (!g_show || dx * dx + dy * dy + dz * dz > 64.0f) k = 1.0f;
        g_draw[0] += dx * k, g_draw[1] += dy * k, g_draw[2] += dz * k;
        float dyaw = remainderf(g_peer.tgt[3] - g_draw[3], 6.2831853f);
        g_draw[3] = k >= 1.0f ? g_peer.tgt[3] : g_draw[3] + dyaw * k;
        if (!g_show && g_standin) LOG("[world] no person of our own for player %016llx: a townsperson stands in (skeleton %llx)\n", (unsigned long long)borrow->id,
                                      (unsigned long long)g_standin);
        g_show = g_standin != 0;
    }
    static uint64_t last_log;
    if (frame - last_log >= 300) {
        last_log = frame;
        for (int r = 0; r < no; r++) {
            Peer *p = &g_peers[order[r]];
            // (the senior-looking skeleton nearest where they are: 231 bones)
            float best = 1e9f;
            for (int i = 0; i < NSK; i++) {
                if (!g_sk[i].sk || frame - g_sk[i].frame > 10 || g_sk[i].bones < 200 || g_sk[i].sk == g_player_sk) continue;
                float ex = g_sk[i].p[0] - p->tgt[0], ez = g_sk[i].p[2] - p->tgt[2], e = sqrtf(ex * ex + ez * ez);
                best = e < best ? e : best;
            }
            LOG("[world] player %016llx at %.2f %.2f %.2f yaw %.0f speed %.1f (drawn %.0f ms behind, %u heard) | person %d state %d, a senior %.2f m from there | us %.2f %.2f %.2f speed %.1f\n",
                (unsigned long long)p->id, p->tgt[0], p->tgt[1], p->tgt[2], p->tgt[3] * 57.2958f, p->tgt[4], p->behind, p->heard, p->own, pla_npc_state(p->own),
                best < 1e8f ? best : -1.0f, g_me[0], g_me[1], g_me[2], my_speed);
        }
    }
}

void pla_world_list_near(float r) {  // (research: <live>/npc "near": the skeletons posed lately within r m)
    uint64_t f = g_frame;
    for (int i = 0; i < NSK; i++) {
        uint64_t sk = g_sk[i].sk;
        if (!sk || f - g_sk[i].frame > 10) continue;
        float dx = g_sk[i].p[0] - g_me[0], dy = g_sk[i].p[1] - g_me[1], dz = g_sk[i].p[2] - g_me[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > r) continue;
        LOG("[world]   skeleton %llx at %.2f %.2f %.2f (%.1f m) bones %u%s\n", (unsigned long long)sk, g_sk[i].p[0], g_sk[i].p[1], g_sk[i].p[2], d, g_sk[i].bones,
            sk == g_player_sk ? " = player" : "");
    }
}

void pla_world_frame(uint64_t frame) {  // (each game frame: warp.c's warp_frame)
    if (g_on <= 0) return;
    g_frame = frame;
    { extern void pla_npc_frame(uint64_t); pla_npc_frame(frame); }
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
