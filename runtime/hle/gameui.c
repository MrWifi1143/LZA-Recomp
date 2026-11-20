// The games' own interface layouts, edited (runtime/gameui.h): nn::ui2d's Layout::Calculate hooked - each frame,
// before a layout's panes become matrices, the edits asked for its panes are put on them (moved, scaled, hidden,
// faded, anchored, text...) and taken off again right after, so the game's animations and its own changes go on
// underneath. Its Layout::Draw hooked too: the layout's fitting on a screen that isn't 16:9 (GameUiSpec.mode) goes to
// the draws it records (cmdbuf.c CMD_UI -> nvn/vulkan/exec.c draw_, which places them in the image), and a "wide"
// layout's projection is made as wide as the screen while it is calculated and drawn.
//
// The same file in both games: what differs (where nn::ui2d is, a few fields) comes from each game's code
// (gameui_init: Z-A hle/za_fixes.c, Arceus hle/pla_wide.c). Both games' nn::ui2d keep the pane the same way:
//   Pane +0x08 its node in the parent's child list (prev, next), +0x18 parent, +0x20 the child list's root node (+0x28
//   the first child's node), +0x30 translate (x y z), +0x3c rotate, +0x48 scale (x y), +0x50 size (w h), +0x58 flags
//   (bit 0 visible, 4 its matrix to be made again, 7 shown this frame - set by Calculate), +0x59 alpha, +0x5a the alpha
//   it is drawn with, +0x5b origin (bits 0-1 x, 2-3 y; 4-5 / 6-7 its place in the parent), +0x70 its matrix in the
//   layout (3 x 4 rows, made by Calculate), +0xb0 name (24 characters).
//   TextBox +0xd8 its text (char16_t*), +0x110 the buffer's length, +0x112 the text's, +0x114 bit 2: lay it out again
//   (what TextBox::SetString does: Z-A 0x700264fbb0, Arceus 0x700014eaac).
//   Layout +0x18 the root pane, +0x28 its size (w h floats), +0x30 its name (Z-A, Arceus).
// Calculate's dirty bit: a pane's matrix is made again only when its flag 0x10 is set (or its parent's was), so an
// edited pane gets that bit each frame it is edited, and once more when the edit goes.
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../gameui.h"
#include "../hle.h"
#include "../mods.h"
#include "../platform.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);
uint64_t renderer_present_count(void);

static GameUiGame G;
static bool g_on;
static float g_screen_k = 1.0f;  // (wider than 16:9 by: gameui_set_screen)

// ---------------------------------------------------------------- the edits asked for
#define NRULE 768
typedef struct {
    char layout[48], pane[48];  // pane: "" the layout itself; "a/b": a pane b under a pane a
    int owner;
    GameUiSpec s;
} Rule;
static Rule g_rule[NRULE];
static int g_nrule;
static volatile uint32_t g_gen = 1;     // (changes with every edit: the layouts look again)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void merge(GameUiSpec *d, const GameUiSpec *s) {
    uint32_t h = s->has;
    if (h & GUI_X) d->x = s->x;
    if (h & GUI_Y) d->y = s->y;
    if (h & GUI_SX) d->sx = s->sx;
    if (h & GUI_SY) d->sy = s->sy;
    if (h & GUI_ALPHA) d->alpha = s->alpha;
    if (h & GUI_HIDE) d->hide = s->hide;
    if (h & GUI_ANCHOR) d->anchor = s->anchor;
    if (h & GUI_TEXT) memcpy(d->text, s->text, sizeof d->text);
    if (h & GUI_IMAGE) memcpy(d->image, s->image, sizeof d->image);
    if (h & GUI_MODE) d->mode = s->mode;
    if (h & GUI_FILL) d->fill = s->fill;
    if (h & GUI_FOLLOW) d->follow = s->follow;
    if (h & GUI_W) d->w = s->w;
    if (h & GUI_H) d->h = s->h;
    d->has |= h;
}

int gameui_set(const char *layout, const char *pane, const GameUiSpec *s, int owner) {
    if (!layout || !*layout || !s || owner < 0 || owner >= GUI_OWNERS) return 0;
    if (!pane) pane = "";
    pthread_mutex_lock(&g_lock);
    Rule *r = NULL;
    for (int i = 0; i < g_nrule; i++)
        if (g_rule[i].owner == owner && !strcmp(g_rule[i].layout, layout) && !strcmp(g_rule[i].pane, pane)) { r = &g_rule[i]; break; }
    if (!r && g_nrule < NRULE) {
        r = &g_rule[g_nrule++];
        memset(r, 0, sizeof *r);
        snprintf(r->layout, sizeof r->layout, "%s", layout);
        snprintf(r->pane, sizeof r->pane, "%s", pane);
        r->owner = owner;
    }
    if (r) merge(&r->s, s), g_gen++;
    pthread_mutex_unlock(&g_lock);
    return r != NULL;
}

void gameui_unset(const char *layout, const char *pane, int owner) {
    pthread_mutex_lock(&g_lock);
    int k = 0;
    for (int i = 0; i < g_nrule; i++) {
        Rule *r = &g_rule[i];
        bool drop = r->owner == owner && (!layout || !strcmp(r->layout, layout)) && (!pane || !strcmp(r->pane, pane));
        if (!drop) g_rule[k++] = *r;
    }
    if (k != g_nrule) g_nrule = k, g_gen++;
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------- reading the game's panes
static inline uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static inline float rdf(uint64_t a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static inline void wrf(uint64_t a, float v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline uint8_t *b8(uint64_t a) { return (uint8_t *)(uintptr_t)a; }
static bool addr_ok(uint64_t a) { return a >= 0x7000000000ull && a < 0x8000000000ull && !(a & 7); }
static const char *pane_name(uint64_t p) { return (const char *)(uintptr_t)(p + 0xb0); }
static bool name_is(uint64_t p, const char *n) { return !strncmp(pane_name(p), n, 24); }

// the pane's type, by its class table (the game's own subclasses: by the function a type's table has at a slot)
static const char *k_types[] = {"Pane", "Picture", "TextBox", "Window", "Parts", "Bounding", "Capture", "Shape", "Layout"};
enum { T_PANE, T_PICTURE, T_TEXTBOX, T_WINDOW, T_PARTS, T_BOUNDING, T_CAPTURE, T_SHAPE, T_UNKNOWN = -1 };
static int pane_type(uint64_t p) {
    uint64_t vt = rd64(p);
    static struct { uint64_t vt; int t; } cache[64];
    unsigned h = (unsigned)((vt >> 3) * 0x9E3779B1u) >> 26;
    if (cache[h].vt == vt) return cache[h].t;
    int t = T_UNKNOWN;
    for (int i = 0; i < 8 && t < 0; i++)
        if (G.vt[i] && vt == G.vt[i]) t = i;
    // a subclass of the game's (Z-A's text boxes): a slot holding the base class's own function
    static const int slots[] = {41, 36, 30, 29, 28, 26, 25, 24, 22};
    for (int i = 1; t < 0 && i < 8; i++) {
        if (!G.vt[i] || !addr_ok(vt)) continue;
        for (size_t k = 0; k < sizeof slots / sizeof *slots; k++) {
            uint64_t a = rd64(vt + 8 * slots[k]), b = rd64(G.vt[i] + 8 * slots[k]);
            bool own = true;  // (a function only that class has - not Pane's)
            if (G.vt[0]) own = rd64(G.vt[0] + 8 * slots[k]) != b;
            if (a == b && own && a) { t = i; break; }
        }
    }
    if (t < 0) t = T_PANE;
    cache[h].vt = vt, cache[h].t = t;
    return t;
}

// children: the list's nodes are the panes' +0x08
#define FOR_CHILDREN(p, c)                                                                                              \
    for (uint64_t root_##c = (p) + 0x20, n_##c = rd64((p) + 0x28), c, k_##c = 0;                                         \
         n_##c && n_##c != root_##c && k_##c < 4096 && ((c = n_##c - 8), 1); n_##c = rd64(n_##c + 8), k_##c++)

// ---------------------------------------------------------------- the layouts seen
#define NLAY 256
#define NPE 96
typedef struct {
    uint64_t pane;
    char name[24];             // (its name when found: still it?)
    GameUiSpec s;
    float t[3], sc[2], sz[2];  // (what the game had: put back after Calculate)
    uint8_t flag, alpha;
    uint16_t game_text[128];   // (its text before ours: given back when the edit goes)
    int16_t game_len;          // (-1: not taken)
} PaneEdit;
typedef struct {
    uint64_t lay, root, name;
    char lname[48];
    float lw, lh;
    uint64_t frame;            // (calculated last at)
    uint32_t gen;
    bool has_lay;              // an edit of the layout itself
    GameUiSpec ls;             // (merged)
    int mode;                  // its fitting (GUI_MODE_*: the layout's own, else wide when a pane is anchored)
    bool fit;                  // (fitted by us: a mode or anchor - its draws tagged)
    int npe;
    PaneEdit pe[NPE];
    int npanes;
} Lay;
static Lay g_lay[NLAY];
static pthread_mutex_t g_lay_lock = PTHREAD_MUTEX_INITIALIZER;  // (the listing against the game thread)

static Lay *lay_of(uint64_t lay, bool make) {
    unsigned h = (unsigned)((lay >> 4) * 0x9E3779B1u) % NLAY;
    for (int i = 0; i < NLAY; i++) {
        Lay *l = &g_lay[(h + i) % NLAY];
        if (l->lay == lay) return l;
        if (!l->lay) {
            if (!make) return NULL;
            memset(l, 0, sizeof *l);
            l->lay = lay;
            return l;
        }
    }
    if (!make) return NULL;
    // full: the one not calculated for the longest time goes (its slot reused; probes still find the others - a slot is
    // never emptied)
    Lay *old = &g_lay[h];
    for (int i = 0; i < NLAY; i++)
        if (g_lay[i].frame < old->frame) old = &g_lay[i];
    memset(old, 0, sizeof *old);
    old->lay = lay;
    return old;
}

// does pane p answer to the edit's name? "b": a pane named b; "a/b": named b, its parent named a (and so on up:
// "x/a/b"); a name ending in "*" is a beginning ("L_icon_*")
static bool name_like(uint64_t p, const char *n, size_t len) {
    if (len && n[len - 1] == '*') return !strncmp(pane_name(p), n, len - 1);
    return len <= 24 && !strncmp(pane_name(p), n, len) && (len == 24 || pane_name(p)[len] == 0);
}
static bool pane_matches(uint64_t p, const char *spec) {
    size_t n = strlen(spec);
    for (int depth = 0; p && depth < 48; depth++) {
        const char *slash = NULL;
        for (size_t i = n; i > 0 && !slash; i--)
            if (spec[i - 1] == '/') slash = spec + i - 1;
        size_t at = slash ? (size_t)(slash - spec) + 1 : 0;
        if (!name_like(p, spec + at, n - at)) return false;
        if (!slash) return true;
        n = at - 1;
        p = rd64(p + 0x18);
        if (!addr_ok(p)) return false;
    }
    return false;
}

static void collect(Lay *l, uint64_t p, const Rule *const *rs, int nr, int depth) {
    if (depth > 48) return;
    for (int i = 0; i < nr; i++)
        if (pane_matches(p, rs[i]->pane)) {
            if (l->npe >= NPE) {  // (a name many panes have: "a/b" picks the right ones)
                static int said;
                if (said++ < 8) LOG("[gameui] %s: more than %d panes edited (\"%s\"...) - the rest left as they are\n", l->lname, NPE, rs[i]->pane);
                break;
            }
            PaneEdit *e = NULL;
            for (int k = 0; k < l->npe; k++)
                if (l->pe[k].pane == p) e = &l->pe[k];
            if (!e) {
                e = &l->pe[l->npe++];
                memset(e, 0, sizeof *e);
                e->pane = p;
                memcpy(e->name, pane_name(p), 24);
                e->game_len = -1;
            }
            merge(&e->s, &rs[i]->s);
        }
    l->npanes++;
    FOR_CHILDREN(p, c) collect(l, c, rs, nr, depth + 1);
}

static void give_back_text(PaneEdit *e);
// the edits for this layout found again (an edit changed, or its panes were made anew)
static uint64_t g_resolves;
static void resolve(Lay *l) {
    g_resolves++;
    for (int k = 0; k < l->npe; k++) {  // (the panes edited until now: made again as the game has them)
        PaneEdit *e = &l->pe[k];
        if (name_is(e->pane, e->name)) b8(e->pane)[0x58] |= 0x10;
        if (e->game_len >= 0) give_back_text(e);
    }
    l->npe = 0, l->npanes = 0, l->has_lay = false;
    memset(&l->ls, 0, sizeof l->ls);
    const Rule *rs[NRULE];
    int nr = 0;
    pthread_mutex_lock(&g_lock);
    l->gen = g_gen;
    for (int o = 0; o < GUI_OWNERS; o++)  // (the runtime's, then mods' data, then scripts': later ones win)
        for (int i = 0; i < g_nrule; i++) {
            const Rule *r = &g_rule[i];
            if (r->owner != o || strcmp(r->layout, l->lname)) continue;
            if (!r->pane[0]) merge(&l->ls, &r->s), l->has_lay = true;
            else rs[nr++] = r;
        }
    // (a copy of the matching rules: the table may change while the tree is walked)
    static Rule copy[NRULE];
    const Rule *cp[NRULE];
    for (int i = 0; i < nr; i++) copy[i] = *rs[i], cp[i] = &copy[i];
    pthread_mutex_unlock(&g_lock);
    if (l->root) collect(l, l->root, cp, nr, 0);
    bool pane_fit = false;
    for (int k = 0; k < l->npe; k++) pane_fit |= (l->pe[k].s.has & (GUI_ANCHOR | GUI_FILL | GUI_FOLLOW)) != 0;
    l->mode = (l->ls.has & GUI_MODE) ? l->ls.mode : pane_fit ? GUI_MODE_WIDE : GUI_MODE_BOX;
    l->fit = (l->ls.has & (GUI_MODE | GUI_ANCHOR | GUI_FILL)) || pane_fit;
}

// ---------------------------------------------------------------- text
static int utf8_to_16(const char *s, uint16_t *out, int cap) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < cap - 1;) {
        uint32_t c = *p++;
        if (c >= 0xc0) {
            int k = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            c &= 0x3f >> k;
            for (int i = 0; i < k && (*p & 0xc0) == 0x80; i++) c = (c << 6) | (*p++ & 0x3f);
        }
        if (c == '\\' && *p == 'n') c = '\n', p++;
        if (c > 0xffff) c = '?';
        out[n++] = (uint16_t)c;
    }
    out[n] = 0;
    return n;
}
static int utf16_to_8(const uint16_t *s, int n, char *out, int cap) {
    int k = 0;
    for (int i = 0; i < n && s[i] && k < cap - 4; i++) {
        uint32_t c = s[i];
        if (c < 0x80) out[k++] = (char)c;
        else if (c < 0x800) out[k++] = (char)(0xc0 | c >> 6), out[k++] = (char)(0x80 | (c & 0x3f));
        else out[k++] = (char)(0xe0 | c >> 12), out[k++] = (char)(0x80 | ((c >> 6) & 0x3f)), out[k++] = (char)(0x80 | (c & 0x3f));
    }
    out[k] = 0;
    return k;
}
// TextBox::SetString's work: the text into its buffer (cut at the buffer's length), laid out again
static void tb_set(uint64_t p, const uint16_t *s, int n) {
    uint64_t buf = rd64(p + 0xd8);
    uint16_t cap = *(uint16_t *)(uintptr_t)(p + 0x110);
    if (!addr_ok(buf & ~7ull) || cap < 1) return;
    if (n > cap - 1) n = cap - 1;
    memcpy((void *)(uintptr_t)buf, s, (size_t)n * 2);
    ((uint16_t *)(uintptr_t)buf)[n] = 0;
    *(uint16_t *)(uintptr_t)(p + 0x112) = (uint16_t)n;
    *(uint16_t *)(uintptr_t)(p + 0x114) |= 4;
    b8(p)[0x5c] &= (uint8_t)~0x10;
}
static bool tb_same(uint64_t p, const uint16_t *s, int n) {
    uint64_t buf = rd64(p + 0xd8);
    uint16_t cap = *(uint16_t *)(uintptr_t)(p + 0x110), len = *(uint16_t *)(uintptr_t)(p + 0x112);
    if (!addr_ok(buf & ~7ull) || cap < 1) return true;  // (no buffer: nothing to do)
    if (n > cap - 1) n = cap - 1;
    return len == n && !memcmp((const void *)(uintptr_t)buf, s, (size_t)n * 2);
}
static void give_back_text(PaneEdit *e) {
    if (name_is(e->pane, e->name) && pane_type(e->pane) == T_TEXTBOX) tb_set(e->pane, e->game_text, e->game_len);
    e->game_len = -1;
}
static void apply_text(PaneEdit *e) {
    uint16_t s[256];
    int n = utf8_to_16(e->s.text, s, 256);
    if (tb_same(e->pane, s, n)) return;
    uint64_t buf = rd64(e->pane + 0xd8);
    if (e->game_len < 0 && addr_ok(buf & ~7ull)) {  // (the game's own, for later)
        uint16_t len = *(uint16_t *)(uintptr_t)(e->pane + 0x112);
        if (len > 127) len = 127;
        memcpy(e->game_text, (const void *)(uintptr_t)buf, (size_t)len * 2);
        e->game_len = (int16_t)len;
    }
    tb_set(e->pane, s, n);
}

// ---------------------------------------------------------------- per frame (the game's thread)
// the layout's shape on screen: wider than 16:9 by kx (or taller by ky)
static void screen_k(uint64_t di, float lw, float lh, float *kx, float *ky) {
    float a = 0;
    if (G.di_target) {
        int32_t w = *(int32_t *)(uintptr_t)(di + G.di_target), h = *(int32_t *)(uintptr_t)(di + G.di_target + 4);
        if (w > 0 && h > 0 && w < 32768 && h < 32768) a = (float)w / (float)h;
    } else if (g_screen_k > 1.0f) {
        a = g_screen_k * 16.0f / 9.0f;
    }
    float la = lh > 0 ? lw / lh : 16.0f / 9.0f;
    *kx = *ky = 1.0f;
    if (a > la * 1.005f) *kx = a / la;
    else if (a > 0 && a < la / 1.005f) *ky = la / a;
}

// the pane's parent's matrix (the last frame's): its scale and place in the layout
static void parent_xf(uint64_t p, float *sx, float *sy, float *tx, float *ty) {
    uint64_t par = rd64(p + 0x18);
    *sx = *sy = 1, *tx = *ty = 0;
    if (!par || !addr_ok(par)) return;
    float a = rdf(par + 0x70), b = rdf(par + 0x84);  // (rows: m00 m01 m02 m03 / m10 m11 m12 m13)
    if (fabsf(a) > 1e-4f) *sx = a;
    if (fabsf(b) > 1e-4f) *sy = b;
    *tx = rdf(par + 0x7c), *ty = rdf(par + 0x8c);
}

static void edit_on(Lay *l, PaneEdit *e, float kx, float ky, bool wide) {
    uint64_t p = e->pane;
    const GameUiSpec *s = &e->s;
    memcpy(e->t, (void *)(uintptr_t)(p + 0x30), 12);
    memcpy(e->sc, (void *)(uintptr_t)(p + 0x48), 8);
    memcpy(e->sz, (void *)(uintptr_t)(p + 0x50), 8);
    e->flag = b8(p)[0x58], e->alpha = b8(p)[0x59];
    float psx, psy, ptx, pty;
    parent_xf(p, &psx, &psy, &ptx, &pty);
    float t0 = e->t[0], t1 = e->t[1];
    bool moved = false;
    float dx = (s->has & GUI_X) ? s->x : 0, dy = (s->has & GUI_Y) ? s->y : 0;  // (layout units)
    if (wide && (s->has & GUI_FOLLOW) && s->follow) {  // (the place spread to the screen: x_layout' = k x_layout)
        float gx = ptx + psx * t0, gy = pty + psy * t1;
        dx += gx * (kx - 1), dy += gy * (ky - 1);
    }
    if (wide && (s->has & GUI_ANCHOR)) {
        float ex = l->lw * (kx - 1) * 0.5f, ey = l->lh * (ky - 1) * 0.5f;
        if (s->anchor & GUI_ANCHOR_LEFT) dx -= ex;
        if (s->anchor & GUI_ANCHOR_RIGHT) dx += ex;
        if (s->anchor & GUI_ANCHOR_TOP) dy += ey;
        if (s->anchor & GUI_ANCHOR_BOTTOM) dy -= ey;
    }
    if (dx != 0 || dy != 0) {
        wrf(p + 0x30, t0 + dx / psx), wrf(p + 0x34, t1 + dy / psy);
        moved = true;
    }
    float sx = e->sc[0], sy = e->sc[1];
    if (s->has & GUI_SX) sx *= s->sx;
    if (s->has & GUI_SY) sy *= s->sy;
    if (wide && (s->has & GUI_FILL) && s->fill) sx *= kx, sy *= ky;
    if (sx != e->sc[0] || sy != e->sc[1]) wrf(p + 0x48, sx), wrf(p + 0x4c, sy), moved = true;
    if (s->has & GUI_W) wrf(p + 0x50, s->w), moved = true;
    if (s->has & GUI_H) wrf(p + 0x54, s->h), moved = true;
    uint8_t f = e->flag;
    if (s->has & GUI_HIDE) f = s->hide ? (uint8_t)(f & ~1) : (uint8_t)(f | 1);
    if (moved) f |= 0x10;
    b8(p)[0x58] = f;
    if (s->has & GUI_ALPHA) {
        float a = e->alpha * fmaxf(0.0f, fminf(1.0f, s->alpha));
        b8(p)[0x59] = (uint8_t)(a + 0.5f);
    }
    if ((s->has & GUI_TEXT) && pane_type(p) == T_TEXTBOX) apply_text(e);
}
static void edit_off(PaneEdit *e) {
    uint64_t p = e->pane;
    memcpy((void *)(uintptr_t)(p + 0x30), e->t, 12);
    memcpy((void *)(uintptr_t)(p + 0x48), e->sc, 8);
    memcpy((void *)(uintptr_t)(p + 0x50), e->sz, 8);
    uint8_t f = b8(p)[0x58];  // (Calculate's own bits kept; the visibility as the game had it)
    b8(p)[0x58] = (uint8_t)((f & ~1) | (e->flag & 1));
    b8(p)[0x59] = e->alpha;
}

static void list_panes(uint64_t p, int depth, char *out, int cap, int *len) {
    if (depth > 48 || *len >= cap - 160) return;
    float w = rdf(p + 0x50), h = rdf(p + 0x54);
    // (its place: its matrix's translation; its size there: size x its scale in the layout)
    float m00 = rdf(p + 0x70), m11 = rdf(p + 0x84), x = rdf(p + 0x7c), y = rdf(p + 0x8c);
    int t = pane_type(p);
    *len += snprintf(out + *len, (size_t)(cap - *len), "%d %.24s %s %.1f %.1f %.1f %.1f %d\n", depth, pane_name(p),
                     t >= 0 ? k_types[t] : "?", x, y, fabsf(w * m00), fabsf(h * m11), (b8(p)[0x58] & 0x80) ? 1 : 0);
    FOR_CHILDREN(p, c) list_panes(c, depth + 1, out, cap, len);
}
static __thread uint32_t t_draw_mode;
uint32_t gameui_draw_mode(void) { return t_draw_mode; }

static uint64_t g_cost_ns, g_cost_n;  // (LARECOMP_GAMEUI_COST: the hooks' own time)

static int calc_hook(Cpu *c, uint64_t addr) {
    uint64_t t0 = plat_now_ns();
    uint64_t lay = c->x[0], di = c->x[1];
    if (!addr_ok(lay)) return 0;
    uint64_t root = rd64(lay + 0x18), name = G.lay_name ? rd64(lay + G.lay_name) : 0;
    pthread_mutex_lock(&g_lay_lock);
    Lay *l = lay_of(lay, true);
    if (l->name != name || l->root != root) {  // (new, or that memory is another layout now)
        l->name = name, l->root = root, l->gen = 0, l->npe = 0;
        snprintf(l->lname, sizeof l->lname, "%s", name && addr_ok(name & ~7ull) ? (const char *)(uintptr_t)name : "?");
    }
    l->lw = rdf(lay + 0x28), l->lh = rdf(lay + 0x2c);
    l->frame = renderer_present_count();
    if (l->gen != g_gen) resolve(l);
    bool any = l->npe || l->has_lay;
    if (!any || !root) {
        pthread_mutex_unlock(&g_lay_lock);
        return 0;
    }
    for (int k = 0; k < l->npe; k++)  // (a pane not there any more - the layout made its panes anew: look again next frame)
        if (!name_is(l->pe[k].pane, l->pe[k].name)) {
            l->gen = 0;
            pthread_mutex_unlock(&g_lay_lock);
            return 0;
        }
    float kx, ky;
    screen_k(di, l->lw, l->lh, &kx, &ky);
    bool wide = l->mode == GUI_MODE_WIDE && (kx > 1 || ky > 1);
    for (int k = 0; k < l->npe; k++) edit_on(l, &l->pe[k], kx, ky, wide);
    uint8_t rflag = b8(root)[0x58], ralpha = b8(root)[0x59];
    if (l->has_lay && (l->ls.has & GUI_HIDE)) b8(root)[0x58] = l->ls.hide ? (uint8_t)(rflag & ~1) : (uint8_t)(rflag | 1);
    if (l->has_lay && (l->ls.has & GUI_ALPHA)) b8(root)[0x59] = (uint8_t)(ralpha * fmaxf(0, fminf(1, l->ls.alpha)) + 0.5f);
    float proj[2];
    if (wide && G.di_proj) {  // (the layout's space as wide as the screen while its matrices are made)
        memcpy(proj, (void *)(uintptr_t)(di + G.di_proj), 4), memcpy(proj + 1, (void *)(uintptr_t)(di + G.di_proj + 0x14), 4);
        wrf(di + G.di_proj, proj[0] / kx), wrf(di + G.di_proj + 0x14, proj[1] / ky);
    }
    pthread_mutex_unlock(&g_lay_lock);
    uint64_t t1 = plat_now_ns();
    rt_call_original(c, addr);
    uint64_t t2 = plat_now_ns();
    pthread_mutex_lock(&g_lay_lock);
    if (wide && G.di_proj) wrf(di + G.di_proj, proj[0]), wrf(di + G.di_proj + 0x14, proj[1]);
    if (l->lay == lay && l->root == root) {
        for (int k = 0; k < l->npe; k++) edit_off(&l->pe[k]);
        uint8_t f = b8(root)[0x58];
        b8(root)[0x58] = (uint8_t)((f & ~1) | (rflag & 1));
        b8(root)[0x59] = ralpha;
    }
    pthread_mutex_unlock(&g_lay_lock);
    g_cost_ns += (t1 - t0) + (plat_now_ns() - t2), g_cost_n++;
    return 1;
}

static int draw_hook(Cpu *c, uint64_t addr) {
    uint64_t lay = c->x[0], di = c->x[1];
    if (!addr_ok(lay)) return 0;
    pthread_mutex_lock(&g_lay_lock);
    Lay *l = lay_of(lay, false);
    bool fit = l && l->fit && l->root;
    uint32_t mode = 0;
    float kx = 1, ky = 1, proj[2];
    bool wide = false;
    if (fit) {
        mode = (uint32_t)(1 + l->mode) | (uint32_t)((l->ls.has & GUI_ANCHOR) ? l->ls.anchor : 0) << 8;
        if ((l->ls.has & GUI_FILL) && !l->ls.fill) mode |= 0x10u << 8;  // (fill 0: no pane stretched over the screen)
        screen_k(di, l->lw, l->lh, &kx, &ky);
        wide = (mode & 0xff) == 1 + GUI_MODE_WIDE && (kx > 1 || ky > 1) && G.di_proj;
        if (wide) {
            memcpy(proj, (void *)(uintptr_t)(di + G.di_proj), 4), memcpy(proj + 1, (void *)(uintptr_t)(di + G.di_proj + 0x14), 4);
            wrf(di + G.di_proj, proj[0] / kx), wrf(di + G.di_proj + 0x14, proj[1] / ky);
        }
    }
    pthread_mutex_unlock(&g_lay_lock);
    if (!fit) return 0;
    uint32_t was = t_draw_mode;
    t_draw_mode = mode;
    rt_call_original(c, addr);
    t_draw_mode = was;
    if (wide) wrf(di + G.di_proj, proj[0]), wrf(di + G.di_proj + 0x14, proj[1]);
    return 1;
}

// ---------------------------------------------------------------- listing (any thread)
// (the tree read where the question is asked - the mod API's Lua runs on the game's own thread, which can't wait for
// its own next Calculate - of a layout calculated in the last frames: one being let go meanwhile is the risk, small)
static int ask(int kind, const char *layout, const char *pane, char *out, int cap) {
    if (!g_on || !out || cap < 2) return -1;
    out[0] = 0;
    int len = -1;
    uint64_t now = renderer_present_count();
    pthread_mutex_lock(&g_lay_lock);
    Lay *best = NULL;
    for (int i = 0; i < NLAY; i++) {
        Lay *l = &g_lay[i];
        if (l->lay && l->root && l->frame + 3 >= now && !strcmp(l->lname, layout) && (!best || l->frame > best->frame)) best = l;
    }
    if (best && kind == 0) {
        len = 0;
        list_panes(best->root, 0, out, cap, &len);
    } else if (best) {
        uint64_t stack[512];
        int sp = 0;
        stack[sp++] = best->root;
        while (sp) {
            uint64_t p = stack[--sp];
            if (pane_matches(p, pane) && pane_type(p) == T_TEXTBOX) {
                uint64_t buf = rd64(p + 0xd8);
                uint16_t n = *(uint16_t *)(uintptr_t)(p + 0x112);
                len = addr_ok(buf & ~7ull) ? utf16_to_8((const uint16_t *)(uintptr_t)buf, n, out, cap) : 0;
                break;
            }
            FOR_CHILDREN(p, c) if (sp < 512) stack[sp++] = c;
        }
    }
    pthread_mutex_unlock(&g_lay_lock);
    return len;
}
int gameui_panes(const char *layout, char *out, int cap) { return layout ? ask(0, layout, NULL, out, cap) : -1; }
int gameui_text(const char *layout, const char *pane, char *out, int cap) {
    return layout && pane ? ask(1, layout, pane, out, cap) : -1;
}

int gameui_layouts(char *out, int cap) {
    if (!out || cap < 2) return 0;
    out[0] = 0;
    int len = 0;
    uint64_t now = renderer_present_count();
    pthread_mutex_lock(&g_lay_lock);
    for (int i = 0; i < NLAY && len < cap - 96; i++) {
        Lay *l = &g_lay[i];
        if (!l->lay || l->frame + 60 < now) continue;
        bool dup = false;  // (one line a name: a layout used several times)
        for (int k = 0; k < i && !dup; k++) dup = g_lay[k].lay && g_lay[k].frame + 60 >= now && !strcmp(g_lay[k].lname, l->lname);
        if (dup) continue;
        len += snprintf(out + len, (size_t)(cap - len), "%s %d %.0fx%.0f\n", l->lname, l->npanes, l->lw, l->lh);
    }
    pthread_mutex_unlock(&g_lay_lock);
    return len;
}

// the game's pane-tree copy isn't wired yet in either game: a new pane is refused (0)
int gameui_clone(const char *layout, const char *src, const char *name, int owner) {
    (void)layout, (void)src, (void)name, (void)owner;
    return 0;
}

// ---------------------------------------------------------------- mods' data: mods/<name>/ui/<layout>.ini
// [layout] (or [*]) the layout itself, [<pane>] a pane ("a/b" for a b under an a); key = value:
//   x y (layout units; y up)  sx sy scale (scale = both)  w h  alpha (0..1)  hide (1/0; show = 1 the same as hide = 0)
//   anchor (left right top bottom, e.g. "left top")  mode (box | wide | stretch)  fill (1)  follow (1)  text  image
static float num(const char *v) { return (float)atof(v); }
static bool truthy(const char *v) { return *v == '1' || *v == 'y' || *v == 'Y' || *v == 't' || *v == 'T' || !strcmp(v, "on"); }
// one key = value into a spec (shared with the mod API's gameui_set): 0 unknown key
int gameui_spec_kv(GameUiSpec *s, const char *k, const char *v) {
    if (!strcmp(k, "x")) s->x = num(v), s->has |= GUI_X;
    else if (!strcmp(k, "y")) s->y = num(v), s->has |= GUI_Y;
    else if (!strcmp(k, "sx")) s->sx = num(v), s->has |= GUI_SX;
    else if (!strcmp(k, "sy")) s->sy = num(v), s->has |= GUI_SY;
    else if (!strcmp(k, "scale")) s->sx = s->sy = num(v), s->has |= GUI_SX | GUI_SY;
    else if (!strcmp(k, "w")) s->w = num(v), s->has |= GUI_W;
    else if (!strcmp(k, "h")) s->h = num(v), s->has |= GUI_H;
    else if (!strcmp(k, "alpha")) s->alpha = num(v), s->has |= GUI_ALPHA;
    else if (!strcmp(k, "hide")) s->hide = truthy(v), s->has |= GUI_HIDE;
    else if (!strcmp(k, "show")) s->hide = !truthy(v), s->has |= GUI_HIDE;
    else if (!strcmp(k, "fill")) s->fill = truthy(v), s->has |= GUI_FILL;
    else if (!strcmp(k, "follow")) s->follow = truthy(v), s->has |= GUI_FOLLOW;
    else if (!strcmp(k, "text")) snprintf(s->text, sizeof s->text, "%s", v), s->has |= GUI_TEXT;
    else if (!strcmp(k, "image")) snprintf(s->image, sizeof s->image, "%s", v), s->has |= GUI_IMAGE;
    else if (!strcmp(k, "mode")) {
        s->mode = !strcmp(v, "wide") ? GUI_MODE_WIDE : !strcmp(v, "stretch") ? GUI_MODE_STRETCH : GUI_MODE_BOX;
        s->has |= GUI_MODE;
    } else if (!strcmp(k, "anchor")) {
        s->anchor = (strstr(v, "left") ? GUI_ANCHOR_LEFT : 0) | (strstr(v, "right") ? GUI_ANCHOR_RIGHT : 0) |
                    (strstr(v, "top") ? GUI_ANCHOR_TOP : 0) | (strstr(v, "bottom") ? GUI_ANCHOR_BOTTOM : 0);
        s->has |= GUI_ANCHOR;
    } else return 0;
    return 1;
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
static int load_ini(const char *path, const char *layout) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[512], pane[48] = "";
    GameUiSpec s;
    memset(&s, 0, sizeof s);
    int n = 0;
    #define FLUSH() do { if (s.has) n += gameui_set(layout, pane, &s, GUI_OWNER_DATA); memset(&s, 0, sizeof s); } while (0)
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line);
        if (!*l || *l == ';' || *l == '#') continue;
        if (*l == '[') {
            FLUSH();
            char *e = strchr(l, ']');
            if (e) *e = 0;
            char *nm = trim(l + 1);
            snprintf(pane, sizeof pane, "%s", !strcmp(nm, "layout") || !strcmp(nm, "*") ? "" : nm);
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(l), *v = trim(eq + 1);
        if (*v == '"' && strlen(v) >= 2 && v[strlen(v) - 1] == '"') v[strlen(v) - 1] = 0, v++;
        for (char *q = k; *q; q++) *q = (char)tolower((unsigned char)*q);
        if (!strcmp(k, "clone")) { FLUSH(); gameui_clone(layout, v, pane, GUI_OWNER_DATA); continue; }
        if (!gameui_spec_kv(&s, k, v)) LOG("[gameui] %s: unknown key \"%s\"\n", path, k);
    }
    FLUSH();
    #undef FLUSH
    fclose(f);
    return n;
}
static time_t g_mods_stamp;
static time_t mods_stamp(void) {  // (the newest of the ui/ folders and their files)
    char names[MODS_MAX][128];
    int n = mods_active(names, MODS_MAX);
    time_t t = 0;
    for (int i = 0; i < n; i++) {
        char dir[256];
        snprintf(dir, sizeof dir, "mods/%s/ui", names[i]);
        struct stat sb;
        if (stat(dir, &sb)) continue;
        if (sb.st_mtime > t) t = sb.st_mtime;
        DIR *d = opendir(dir);
        for (struct dirent *e; d && (e = readdir(d));) {
            char p[512];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if (!stat(p, &sb) && sb.st_mtime > t) t = sb.st_mtime;
        }
        if (d) closedir(d);
    }
    return t;
}
void gameui_load_mods(void) {
    gameui_unset(NULL, NULL, GUI_OWNER_DATA);
    char names[MODS_MAX][128];
    int n = mods_active(names, MODS_MAX), total = 0, files = 0;
    for (int i = n - 1; i >= 0; i--) {  // (the first mod wins: loaded last)
        char dir[256];
        snprintf(dir, sizeof dir, "mods/%s/ui", names[i]);
        DIR *d = opendir(dir);
        for (struct dirent *e; d && (e = readdir(d));) {
            size_t len = strlen(e->d_name);
            if (len < 5 || strcmp(e->d_name + len - 4, ".ini")) continue;
            char layout[48], path[512];
            snprintf(layout, sizeof layout, "%.*s", (int)(len - 4), e->d_name);
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            total += load_ini(path, layout), files++;
        }
        if (d) closedir(d);
    }
    g_mods_stamp = mods_stamp();
    if (files) LOG("[gameui] mods' interface edits: %d files, %d panes\n", files, total);
}
static void *watch_mods(void *arg) {  // (a mod's ui/ edited while the game runs: read again)
    (void)arg;
    for (;;) {
        plat_sleep_until_ns(plat_now_ns() + 2000000000ull);
        if (mods_stamp() != g_mods_stamp) gameui_load_mods();
        if (ENV("LARECOMP_GAMEUI_COST") && g_cost_n) {
            static uint64_t at;
            uint64_t f = renderer_present_count();
            if (f > at + 600) {
                LOG("[gameui] %.2f us a frame (%llu calls, %llu layouts looked through)\n", g_cost_ns / 1000.0 / (double)(f - at),
                    (unsigned long long)g_cost_n, (unsigned long long)g_resolves);
                g_resolves = 0;
                g_cost_ns = g_cost_n = 0, at = f;
            }
        }
    }
    return NULL;
}

// ---------------------------------------------------------------- the mod API's (runtime/modapi.h ui_*): a script's
int gameui_api_set(const char *layout, const char *pane, const char *key, const char *value) {
    GameUiSpec s;
    memset(&s, 0, sizeof s);
    if (!layout || !key || !value || !gameui_spec_kv(&s, key, value)) return 0;
    return gameui_set(layout, pane ? pane : "", &s, GUI_OWNER_SCRIPT);
}
void gameui_api_unset(const char *layout, const char *pane) { gameui_unset(layout, pane, GUI_OWNER_SCRIPT); }
int gameui_api_clone(const char *layout, const char *src, const char *name) {
    return layout && src && name ? gameui_clone(layout, src, name, GUI_OWNER_SCRIPT) : 0;
}

// ---------------------------------------------------------------- the runtime's own: ultrawide
// "Fill the window" on a screen wider than 16:9: the field's HUD at the real screen's edges (the rest of the
// interface stays in the middle 16:9). Per game: the layouts and how (gameui.h's ini keys).
typedef struct { const char *layout, *pane, *kv; } Builtin;
static const Builtin k_wide_za[] = {
    {"hud_minimap_field_00", "", "anchor=left"},
    {"hud_minimap_top_00", "", "anchor=left"},
    {"hud_minimap_dungeon_a_00", "", "anchor=left"},
    {"hud_minimap_dungeon_c_00", "", "anchor=left"},
    {"hud_pokelist_00", "", "anchor=left"},
    // (world markers: the field's action icons and the aiming reticle - placed by the game where the thing is on a
    // 16:9 screen: spread to the real one)
    {"field_action_help_00", "", "mode=wide"},
    {"field_action_help_00", "all/L_*", "follow=1"},
    {"btl_app_focus_00", "", "mode=wide"},
    {"btl_app_focus_00", "RootPane/all", "follow=1"},
    {"hud_guide_00", "", "anchor=right"},
    // (the map: its space as wide as the screen - more of the map's icons; the header and shading across it)
    {"map_top_00", "", "mode=wide"},
    {"map_top_00", "N_header_00/P_header_*", "fill=1"},
    {"map_top_00", "N_shadow_00/P_shadow_*", "fill=1"},
    {"map_top_00", "N_header_00/N_title_00", "anchor=left"},
    {"map_top_00", "N_header_00/L_zoom_00", "anchor=right"},
    {"map_top_00", "N_header_00/N_weather_00", "anchor=right"},
    {"map_top_00", "N_header_00/N_comp_on_of_00", "anchor=right"},
    {"map_top_00", "N_inout_01/L_info_guide_00", "anchor=left"},
    // (the Settings sheet: wider than the layout, it was stretched over the screen with the dimming - kept in the box)
    {"option_gamesetting_00", "", "fill=0"},
    {NULL, NULL, NULL},
};
static const Builtin k_wide_pla[] = {
    {NULL, NULL, NULL},
};
static void builtin_wide(void) {
    const Builtin *b = !strcmp(G.game, "za") ? k_wide_za : k_wide_pla;
    gameui_unset(NULL, NULL, GUI_OWNER_BUILTIN);
    for (; b->layout; b++) {
        GameUiSpec s;
        memset(&s, 0, sizeof s);
        char kv[256];
        snprintf(kv, sizeof kv, "%s", b->kv);
        for (char *tok = strtok(kv, ";"); tok; tok = strtok(NULL, ";")) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            gameui_spec_kv(&s, trim(tok), trim(eq + 1));
        }
        gameui_set(b->layout, b->pane, &s, GUI_OWNER_BUILTIN);
    }
}
void gameui_set_screen(float k) {
    g_screen_k = k > 1.0f ? k : 1.0f;
    if (!g_on) return;  // (gameui_init does it)
    if (g_screen_k > 1.005f && !ENV("LARECOMP_GAMEUI_NOWIDE")) builtin_wide();
    else gameui_unset(NULL, NULL, GUI_OWNER_BUILTIN);
}

void gameui_init(const GameUiGame *g) {
    if (g_on || !g || !g->layout_calc) return;
    G = *g;
    g_on = true;
    rt_register_fnhook_at(G.layout_calc, calc_hook);
    if (G.layout_draw) rt_register_fnhook_at(G.layout_draw, draw_hook);
    gameui_load_mods();
    gameui_set_screen(g_screen_k);
    pthread_t th;
    if (!pthread_create(&th, NULL, watch_mods, NULL)) pthread_detach(th);
    LOG("[gameui] on: Layout::Calculate %llx, Draw %llx\n", (unsigned long long)G.layout_calc, (unsigned long long)G.layout_draw);
}
