// Script messages and chat over the shared world (the world API's net_* / chat_*: Lua net.send / net.receive / players
// / chat, docs/lua-api.md "Networking"). The same file in both games: what differs - the packet a body travels in, who
// sent it, where the other players' stand-ins are - is each game's world code (Z-A hle/warp.c, Arceus hle/pla_world.c:
// world_net_send / world_net_up / world_self_id / world_players_list / world_player_feet, and their receive loops hand
// our packets to luanet_heard).
//
// The relay passes on packets of up to 96 bytes (contrib/steamrelay: larecomp_world_send), so a message travels in
// pieces of LUANET_PIECE bytes: {u8 kind 'M' message / 'C' chat, u8 piece k, u8 pieces n, u8 0, u16 message number,
// u16 total length, u64 to (0: everyone)} + up to 64 bytes. A message is whole once all its pieces came (2 s at most,
// else dropped); pieces lost are not asked again (the relay is UDP - scripts that need it send again).
// Caps, both ways: a message is at most WORLD_NET_MAX bytes; we send at most 30 messages and 8 KB a second (a token
// bucket: a burst of 60 / 16 KB); from one player we take at most as much (more is dropped, logged once a minute). Chat
// lines: 1 a second each way per player, BUBBLE_MAX_CHARS characters.
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../bubble.h"
#include "../hle.h"
#include "../platform.h"
#include "../worldapi.h"

// (each game's world code)
int world_net_up(void);
uint64_t world_self_id(void);
void world_net_send(const uint8_t *body, int n);  // a body of up to LUANET_PIECE bytes to everyone in the world
int world_players_list(LarecompPlayer *out, int max);
int world_player_feet(uint64_t id, float feet[3]);  // 1 their stand-in is there, 0 not now, -1 no such player
void world_bubbles_on(void);  // (hle/worldapi.c: the scene camera the bubbles are placed with, followed from now on)

#define LUANET_PIECE 80
#define HDR 16
#define DATA (LUANET_PIECE - HDR)
#define MAXPIECES ((WORLD_NET_MAX + DATA - 1) / DATA)
#define NASM 16    // (messages being put together at once)
#define NQ 64      // (whole messages waiting for the scripts)
#define NCHATQ 16
#define NSENDERS 32

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static double now_s(void) { return (double)plat_now_ns() / 1e9; }

// ---- rate caps: a token bucket per direction (ours) and per sender (theirs)
typedef struct { double t, msgs, bytes, chat_t; uint64_t id; unsigned dropped; double told; } Bucket;
static Bucket g_out;
static Bucket g_in[NSENDERS];
static bool take(Bucket *b, int bytes) {
    double t = now_s();
    if (b->t == 0) b->t = t, b->msgs = 60, b->bytes = 16384;
    double dt = t - b->t;
    b->t = t;
    b->msgs = fmin(60, b->msgs + dt * 30), b->bytes = fmin(16384, b->bytes + dt * 8192);
    if (b->msgs < 1 || b->bytes < bytes) return false;
    b->msgs -= 1, b->bytes -= bytes;
    return true;
}
static Bucket *sender(uint64_t id) {
    Bucket *free_b = NULL, *old = &g_in[0];
    for (int i = 0; i < NSENDERS; i++) {
        if (g_in[i].id == id) return &g_in[i];
        if (!g_in[i].id && !free_b) free_b = &g_in[i];
        if (g_in[i].t < old->t) old = &g_in[i];
    }
    Bucket *b = free_b ? free_b : old;
    memset(b, 0, sizeof *b);
    b->id = id;
    return b;
}

// ---- putting messages together
typedef struct {
    uint64_t from;
    uint16_t num, len;
    uint8_t n, kind;
    uint32_t have;  // (pieces come: bit k)
    double t;
    uint8_t d[WORLD_NET_MAX];
} Asm;
static Asm g_asm[NASM];
typedef struct { uint64_t from; int len; uint8_t d[WORLD_NET_MAX]; } Msg;
static Msg g_q[NQ];
static int g_qh, g_qn;
typedef struct { uint64_t from; char text[4 * BUBBLE_MAX_CHARS + 8]; } Chat;
static Chat g_cq[NCHATQ];
static int g_cqh, g_cqn;
static uint16_t g_num;
static unsigned g_sent, g_heard;

// a chat line over its sender's stand-in: the bubble asks where it stands each frame (ctx = a slot holding their id)
static uint64_t g_chat_ids[NSENDERS];
static int chat_where(void *ctx, float feet[3]) { return world_player_feet(*(uint64_t *)ctx, feet); }
static void chat_bubble(uint64_t id, const char *text, float sec) {
    int k = -1;
    for (int i = 0; i < NSENDERS && k < 0; i++) if (g_chat_ids[i] == id) k = i;
    for (int i = 0; i < NSENDERS && k < 0; i++) if (!g_chat_ids[i]) k = i;
    if (k < 0) k = (int)(id % NSENDERS);
    g_chat_ids[k] = id;
    BubbleTarget t = {.kind = BUBBLE_FN, .where = chat_where, .ctx = &g_chat_ids[k], .owner = BUBBLE_OWNER_MP};
    world_bubbles_on();
    bubble_show(&t, text, sec);
}

static void deliver(const Asm *a) {
    if (a->kind == 'C') {
        char text[4 * BUBBLE_MAX_CHARS + 8];
        int n = a->len < (int)sizeof text - 1 ? a->len : (int)sizeof text - 1;
        memcpy(text, a->d, (size_t)n), text[n] = 0;
        Chat *c = &g_cq[(g_cqh + g_cqn) % NCHATQ];
        if (g_cqn == NCHATQ) g_cqh = (g_cqh + 1) % NCHATQ; else g_cqn++;  // (full: the oldest goes)
        c->from = a->from;
        snprintf(c->text, sizeof c->text, "%s", text);
        LOG("[net] chat from %016llx: %s\n", (unsigned long long)a->from, text);
        chat_bubble(a->from, text, 0);
        return;
    }
    if (g_qn == NQ) { g_qh = (g_qh + 1) % NQ, g_qn--; }  // (the scripts don't read them: the oldest goes)
    Msg *m = &g_q[(g_qh + g_qn) % NQ];
    m->from = a->from, m->len = a->len;
    memcpy(m->d, a->d, a->len);
    g_qn++;
    g_heard++;
}

// a piece heard (each game's receive loop): from = the sender's id in the world
void luanet_heard(uint64_t from, const uint8_t *b, int n) {
    if (n < HDR || !from) return;
    uint8_t kind = b[0], k = b[1], cnt = b[2];
    uint16_t num, len;
    uint64_t to;
    memcpy(&num, b + 4, 2), memcpy(&len, b + 6, 2), memcpy(&to, b + 8, 8);
    if ((kind != 'M' && kind != 'C') || !cnt || cnt > MAXPIECES || k >= cnt || !len || len > WORLD_NET_MAX ||
        (len + DATA - 1) / DATA != cnt) return;
    uint64_t self = world_self_id();
    if (to && to != self) return;
    int dl = n - HDR, want = k + 1 < cnt ? DATA : len - k * DATA;
    if (dl < want) return;
    pthread_mutex_lock(&g_lock);
    double t = now_s();
    Asm *a = NULL, *slot = NULL;
    for (int i = 0; i < NASM; i++) {
        Asm *x = &g_asm[i];
        if (x->have && t - x->t > 2.0) x->have = 0;  // (pieces missing for 2 s: dropped)
        if (x->have && x->from == from && x->num == num && x->kind == kind) a = x;
        if (!x->have && !slot) slot = x;
    }
    if (!a && k == 0) {  // (a new message: the sender's cap counted once, at its first piece)
        Bucket *s = sender(from);
        bool ok = take(s, len);
        if (ok && kind == 'C') ok = t - s->chat_t >= 1.0, s->chat_t = ok ? t : s->chat_t;
        if (!ok) {
            s->dropped++;
            if (t - s->told > 60) LOG("[net] player %016llx sends too much: %u dropped\n", (unsigned long long)from, s->dropped), s->told = t;
            pthread_mutex_unlock(&g_lock);
            return;
        }
        if (!slot) { pthread_mutex_unlock(&g_lock); return; }
        a = slot;
        memset(a, 0, offsetof(Asm, d));
        a->from = from, a->num = num, a->len = len, a->n = cnt, a->kind = kind, a->t = t;
    }
    if (!a || a->len != len || a->n != cnt) { pthread_mutex_unlock(&g_lock); return; }
    memcpy(a->d + k * DATA, b + HDR, (size_t)want);
    a->have |= 1u << k;
    if (a->have == (cnt == 32 ? 0xffffffffu : (1u << cnt) - 1)) deliver(a), a->have = 0;
    pthread_mutex_unlock(&g_lock);
}

static int send_pieces(uint8_t kind, const void *msg, int len, uint64_t to) {
    if (len <= 0 || len > WORLD_NET_MAX || !world_net_up()) return 0;
    pthread_mutex_lock(&g_lock);
    if (!take(&g_out, len)) { pthread_mutex_unlock(&g_lock); return 0; }
    uint16_t num = ++g_num, l16 = (uint16_t)len;
    int cnt = (len + DATA - 1) / DATA;
    pthread_mutex_unlock(&g_lock);
    for (int k = 0; k < cnt; k++) {
        uint8_t b[LUANET_PIECE] = {kind, (uint8_t)k, (uint8_t)cnt, 0};
        memcpy(b + 4, &num, 2), memcpy(b + 6, &l16, 2), memcpy(b + 8, &to, 8);
        int m = k + 1 < cnt ? DATA : len - k * DATA;
        memcpy(b + HDR, (const uint8_t *)msg + k * DATA, (size_t)m);
        world_net_send(b, HDR + m);
    }
    g_sent++;
    return 1;
}

// ---- the world API's side
int luanet_up(void) { return world_net_up(); }
uint64_t luanet_self(void) { return world_self_id(); }
int luanet_send(const void *msg, int len, uint64_t to) { return send_pieces('M', msg, len, to); }
int luanet_recv(uint64_t *from, void *msg, int cap) {
    pthread_mutex_lock(&g_lock);
    int n = 0;
    if (g_qn) {
        Msg *m = &g_q[g_qh];
        n = m->len < cap ? m->len : cap;
        *from = m->from;
        memcpy(msg, m->d, (size_t)n);
        g_qh = (g_qh + 1) % NQ, g_qn--;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}
int luanet_players(LarecompPlayer *out, int max) { return world_players_list(out, max); }
int luanet_chat_send(const char *text) {
    static double last;
    double t = now_s();
    if (!text || !text[0] || t - last < 1.0) return 0;  // (one line a second)
    char line[4 * BUBBLE_MAX_CHARS + 8];
    snprintf(line, sizeof line, "%s", text);
    int len = (int)strlen(line);
    if (!send_pieces('C', line, len, 0)) return 0;
    last = t;
    BubbleTarget me = {.kind = BUBBLE_PLAYER, .owner = BUBBLE_OWNER_MP};
    world_bubbles_on();
    bubble_show(&me, line, 0);  // (ours over our own player)
    LOG("[net] chat sent: %s\n", line);
    return 1;
}
int luanet_chat_recv(uint64_t *from, char *text, int cap) {
    pthread_mutex_lock(&g_lock);
    int n = 0;
    if (g_cqn) {
        Chat *c = &g_cq[g_cqh];
        *from = c->from;
        snprintf(text, (size_t)cap, "%s", c->text);
        n = (int)strlen(text);
        g_cqh = (g_cqh + 1) % NCHATQ, g_cqn--;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}
int luanet_bubble_over(uint64_t id, const char *text, float sec) {
    float f[3];
    if (world_player_feet(id, f) < 0) return 0;
    chat_bubble(id, text, sec);
    return 1;
}
void luanet_stats(unsigned *sent, unsigned *heard) { *sent = g_sent, *heard = g_heard; }
