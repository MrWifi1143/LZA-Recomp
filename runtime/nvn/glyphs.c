// Button prompts for the controller in use (start screen "Button prompts": Auto, Nintendo Switch, Xbox, PlayStation).
// The games draw each controller button from its own small texture: a BC5 one whose green channel is the button's
// shape and whose red channel is the symbol cut out of it (dark on white), or a BC4 mask of the symbol alone (bright
// on dark, laid over a separate round base). The renderer hands every small texture's level 0 here as it uploads it
// (runtime/nvn/vulkan/textures.c); a known button icon (by the hash of its content, the table below: the names come
// from LARECOMP_BNTX_SCAN, runtime/hle/bntxscan.c) gets its symbol redrawn for the physical button that drives that
// Switch input now (runtime/hle/bindings.c, remaps included): Xbox letters / PlayStation shapes, or the Switch label of
// another button after a remap. The button's shape stays the game's. Nothing of another console's artwork is used:
// the letters come from the bundled Baloo 2 font, the shapes are drawn here.
#include "glyphs.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_gamepad.h>

#include "../hle.h"
#include "../input_bind.h"
#include "../paths.h"
#include "../settings.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "../../third_party/imgui/imstb_truetype.h"
#pragma clang diagnostic pop

int input_pad_family(void);  // runtime/hle/input.c: 0 Nintendo / other / none, 1 Xbox, 2 PlayStation

// ---- the icons: content hash (as textures.c computes it, | 1) -> the Switch input it shows
typedef struct { uint64_t hash; int8_t slot; } Icon;
static const Icon k_icons[] = {
    // Legends: Arceus 1.1.1: button_icon_<button>_00 (BC5), _01 (BC4 masks of the field HUD)
    {0xe4bb8ef52b12efe3ull, BIND_A}, {0xb4fed4ab022163e9ull, BIND_B}, {0xa4c536c17a5b636dull, BIND_X},
    {0x14cf5c793d89bcdfull, BIND_Y}, {0xa9cbd3abf2e81effull, BIND_L}, {0xd57e5dac8a5cc415ull, BIND_R},
    {0xc16c4bfbb432b645ull, BIND_L}, {0x1f1d251372b42fe5ull, BIND_R}, {0xacc1eec2bbc48fadull, BIND_R},
    {0xb9b9f9e1fd08a589ull, BIND_ZL}, {0xf7fd19722bca3907ull, BIND_ZR}, {0x379b93bbf81da2cfull, BIND_ZL},
    {0xe73bbe720a3056c7ull, BIND_ZR}, {0xe73ef073c6037d53ull, BIND_ZL}, {0xe5ef56a7fe770233ull, BIND_ZR},
    {0xe6ba8141a7c4928bull, BIND_MINUS}, {0x4571c30e47c9055dull, BIND_PLUS},
    {0xfe555ee75a19cddbull, BIND_A}, {0x5647633719c5875dull, BIND_B}, {0xd6b217384417b63dull, BIND_Y},
    {0x7c29ccb9a757c29bull, BIND_MINUS}, {0x07a1de89a01700c5ull, BIND_PLUS},
    // Legends: Z-A 0.6.0: icon_key_<button>_00 (BC5)
    {0x70ea9fc1470cd10bull, BIND_A}, {0x47f8025d9c250f75ull, BIND_B}, {0x9d768f40d5ea4219ull, BIND_X},
    {0x6758c430eb89582dull, BIND_Y}, {0x17a0045b771f7ce5ull, BIND_Y}, {0x3fdd1dab98e84e77ull, BIND_L},
    {0x2e91afa9093b7231ull, BIND_R}, {0x883d24305f75764dull, BIND_ZL}, {0x343868496aa2b7f3ull, BIND_ZR},
    {0x096f141e6a6b0531ull, BIND_MINUS}, {0xddb13a6ffa02fa95ull, BIND_PLUS},
};

// ---- what a physical button is called on each controller family
enum { SYM_TEXT, SYM_CROSS, SYM_CIRCLE, SYM_SQUARE, SYM_TRIANGLE, SYM_VIEW, SYM_MENU, SYM_CREATE, SYM_OPTIONS, SYM_PLUS, SYM_MINUS };
typedef struct { int sym; const char *text; } Label;

static bool label_of(int family, int host, Label *o) {
    *o = (Label){SYM_TEXT, NULL};
    switch (host) {
    case SDL_GAMEPAD_BUTTON_SOUTH: *o = family == 1 ? (Label){SYM_TEXT, "A"} : family == 2 ? (Label){SYM_CROSS} : (Label){SYM_TEXT, "B"}; return true;
    case SDL_GAMEPAD_BUTTON_EAST: *o = family == 1 ? (Label){SYM_TEXT, "B"} : family == 2 ? (Label){SYM_CIRCLE} : (Label){SYM_TEXT, "A"}; return true;
    case SDL_GAMEPAD_BUTTON_WEST: *o = family == 1 ? (Label){SYM_TEXT, "X"} : family == 2 ? (Label){SYM_SQUARE} : (Label){SYM_TEXT, "Y"}; return true;
    case SDL_GAMEPAD_BUTTON_NORTH: *o = family == 1 ? (Label){SYM_TEXT, "Y"} : family == 2 ? (Label){SYM_TRIANGLE} : (Label){SYM_TEXT, "X"}; return true;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: o->text = family == 1 ? "LB" : family == 2 ? "L1" : "L"; return true;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: o->text = family == 1 ? "RB" : family == 2 ? "R1" : "R"; return true;
    case PADB_LTRIGGER: o->text = family == 1 ? "LT" : family == 2 ? "L2" : "ZL"; return true;
    case PADB_RTRIGGER: o->text = family == 1 ? "RT" : family == 2 ? "R2" : "ZR"; return true;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: o->text = family == 1 ? "LS" : family == 2 ? "L3" : "LS"; return true;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: o->text = family == 1 ? "RS" : family == 2 ? "R3" : "RS"; return true;
    case SDL_GAMEPAD_BUTTON_BACK: o->sym = family == 1 ? SYM_VIEW : family == 2 ? SYM_CREATE : SYM_MINUS; return true;
    case SDL_GAMEPAD_BUTTON_START: o->sym = family == 1 ? SYM_MENU : family == 2 ? SYM_OPTIONS : SYM_PLUS; return true;
    default: return false;  // (a d-pad direction, a paddle...: the game's own icon stays)
    }
}

// the label the game's own icon already shows: the Switch name of the input (no redraw needed for it)
static Label switch_label(int slot) {
    static const char *const t[BIND_PAD_COUNT] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR"};
    if (slot == BIND_PLUS) return (Label){SYM_PLUS};
    if (slot == BIND_MINUS) return (Label){SYM_MINUS};
    return (Label){SYM_TEXT, t[slot]};
}

static int effective_family(void) {
    int m = g_settings.button_prompts;
    if (m == PROMPTS_SWITCH) return 0;
    if (m == PROMPTS_XBOX) return 1;
    if (m == PROMPTS_PLAYSTATION) return 2;
    return input_pad_family();
}

// ---- the generation: changes whenever an icon would be drawn differently (the setting, the controller, a remap)
static _Atomic uint32_t g_gen = 1;
static uint64_t g_sig;
uint32_t glyphs_generation(void) {
    uint64_t s = (uint64_t)effective_family() + 7;
    for (int i = 0; i < BIND_PAD_COUNT; i++) s = s * 131 + (uint64_t)(bind_prompt_host(i) + 3);
    if (s != g_sig) {
        if (g_sig) g_gen++;
        g_sig = s;
    }
    return g_gen;
}

// ---- BC4 blocks (one channel; BC5 = two of them)
static void bc4_decode(const uint8_t *b, uint8_t out[16]) {
    uint8_t p[8] = {b[0], b[1]};
    if (p[0] > p[1]) for (int i = 1; i < 7; i++) p[i + 1] = (uint8_t)(((7 - i) * p[0] + i * p[1] + 3) / 7);
    else {
        for (int i = 1; i < 5; i++) p[i + 1] = (uint8_t)(((5 - i) * p[0] + i * p[1] + 2) / 5);
        p[6] = 0, p[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = p[(bits >> (3 * i)) & 7];
}

static void bc4_encode(const uint8_t v[16], uint8_t *b) {
    uint8_t lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    memset(b, 0, 8);
    b[0] = hi, b[1] = lo;
    if (hi == lo) return;  // (every index 0: the one value)
    uint8_t p[8] = {hi, lo};
    for (int i = 1; i < 7; i++) p[i + 1] = (uint8_t)(((7 - i) * hi + i * lo + 3) / 7);
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 8; k++) { int d = abs((int)p[k] - v[i]); if (d < bd) bd = d, best = k; }
        bits |= (uint64_t)best << (3 * i);
    }
    for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(bits >> (8 * i));
}

// channel c (0 / 1 of BC5, 0 of BC4) of the level <-> a w x h plane (padded to whole blocks)
static void plane_get(const uint8_t *lin, int bpb, int c, int wb, int hb, uint8_t *pl) {
    for (int by = 0; by < hb; by++)
        for (int bx = 0; bx < wb; bx++) {
            uint8_t v[16];
            bc4_decode(lin + ((size_t)by * wb + bx) * bpb + 8 * c, v);
            for (int i = 0; i < 16; i++) pl[(size_t)(by * 4 + i / 4) * wb * 4 + bx * 4 + i % 4] = v[i];
        }
}
static void plane_put(uint8_t *lin, int bpb, int c, int wb, int hb, const uint8_t *pl) {
    for (int by = 0; by < hb; by++)
        for (int bx = 0; bx < wb; bx++) {
            uint8_t v[16];
            for (int i = 0; i < 16; i++) v[i] = pl[(size_t)(by * 4 + i / 4) * wb * 4 + bx * 4 + i % 4];
            bc4_encode(v, lin + ((size_t)by * wb + bx) * bpb + 8 * c);
        }
}

// ---- drawing: coverage (0..1) at SS x the texture's resolution, box-filtered down
#define SS 4
typedef struct { float *c; int w, h; } Canvas;  // (supersampled size)

static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
    float t = l > 0 ? ((px - ax) * dx + (py - ay) * dy) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
    return sqrtf(ex * ex + ey * ey);
}
static void stroke(Canvas *cv, float ax, float ay, float bx, float by, float half) {  // a round-capped line
    int x0 = (int)fmaxf(0, fminf(ax, bx) - half - 1), x1 = (int)fminf(cv->w - 1, fmaxf(ax, bx) + half + 1);
    int y0 = (int)fmaxf(0, fminf(ay, by) - half - 1), y1 = (int)fminf(cv->h - 1, fmaxf(ay, by) + half + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (seg_dist(x + 0.5f, y + 0.5f, ax, ay, bx, by) <= half) cv->c[(size_t)y * cv->w + x] = 1;
}
static void ring(Canvas *cv, float cx, float cy, float r, float half) {
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float d = fabsf(hypotf(x + 0.5f - cx, y + 0.5f - cy) - r);
            if (d <= half) cv->c[(size_t)y * cv->w + x] = 1;
        }
}
static void poly(Canvas *cv, const float *p, int n, float half) {  // a closed outline
    for (int i = 0; i < n; i++) stroke(cv, p[2 * i], p[2 * i + 1], p[2 * ((i + 1) % n)], p[2 * ((i + 1) % n) + 1], half);
}

static stbtt_fontinfo g_font;
static uint8_t *g_font_data;
static bool font_ready(void) {
    static int tried;
    if (tried) return g_font_data != NULL;
    tried = 1;
    char p[1100];
    if (!rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", p, sizeof p)) return false;
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_font_data = malloc((size_t)n);
    if (!g_font_data || fread(g_font_data, 1, (size_t)n, f) != (size_t)n ||
        !stbtt_InitFont(&g_font, g_font_data, stbtt_GetFontOffsetForIndex(g_font_data, 0))) {
        free(g_font_data);
        g_font_data = NULL;
    }
    fclose(f);
    if (!g_font_data) LOG("[glyphs] %s: not a usable font, button prompts stay the game's\n", p);
    return g_font_data != NULL;
}

// text centred on (cx, cy), cap height ch, at most maxw wide (supersampled pixels), made bolder by `bold` pixels
static void text(Canvas *cv, const char *s, float cx, float cy, float ch, float maxw, float bold) {
    int x0, y0, x1, y1;
    stbtt_GetCodepointBox(&g_font, 'H', &x0, &y0, &x1, &y1);
    float scale = ch / (float)(y1 > 0 ? y1 : 1);
    float width = 0;
    int n = (int)strlen(s);
    for (int i = 0; i < n; i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, s[i], &adv, &lsb);
        width += adv * scale + (i + 1 < n ? stbtt_GetCodepointKernAdvance(&g_font, s[i], s[i + 1]) * scale : 0);
    }
    if (width > maxw && width > 0) { scale *= maxw / width; ch *= maxw / width; width = maxw; }
    float pen = cx - width / 2, base = cy + ch / 2;
    for (int i = 0; i < n; i++) {
        int gw, gh, xo, yo;
        const int pad = 8;
        unsigned char *sdf = stbtt_GetCodepointSDF(&g_font, scale, s[i], pad, 128, 128.0f / pad, &gw, &gh, &xo, &yo);
        if (sdf) {
            float edge = 128 - bold * (128.0f / pad);  // (one SDF unit = 1 / (128 / pad) px)
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++) {
                    int tx = (int)lroundf(pen + xo + x), ty = (int)lroundf(base + yo + y);
                    if (tx < 0 || ty < 0 || tx >= cv->w || ty >= cv->h) continue;
                    if (sdf[y * gw + x] >= edge) cv->c[(size_t)ty * cv->w + tx] = 1;
                }
            stbtt_FreeSDF(sdf, NULL);
        }
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, s[i], &adv, &lsb);
        pen += adv * scale + (i + 1 < n ? stbtt_GetCodepointKernAdvance(&g_font, s[i], s[i + 1]) * scale : 0);
    }
}

// the label drawn into the canvas: centre (cx, cy), letter height ch, room maxw, the button's smaller side bs (all
// supersampled)
static void draw_label(Canvas *cv, Label l, float cx, float cy, float ch, float maxw, float bs) {
    float s = fminf(ch * 1.1f, bs * 0.5f), hw = s * 0.11f;  // (the PlayStation shapes a little larger than a letter)
    switch (l.sym) {
    case SYM_TEXT: text(cv, l.text, cx, cy, strlen(l.text) > 1 ? ch * 0.82f : ch, maxw, ch * 0.03f); break;
    case SYM_CROSS: {
        float r = s * 0.45f;
        stroke(cv, cx - r, cy - r, cx + r, cy + r, hw), stroke(cv, cx - r, cy + r, cx + r, cy - r, hw);
        break;
    }
    case SYM_CIRCLE: ring(cv, cx, cy, s * 0.46f, hw); break;
    case SYM_SQUARE: {
        float r = s * 0.42f, p[8] = {cx - r, cy - r, cx + r, cy - r, cx + r, cy + r, cx - r, cy + r};
        poly(cv, p, 4, hw);
        break;
    }
    case SYM_TRIANGLE: {
        float r = s * 0.52f, p[6] = {cx, cy - r, cx + r * 0.87f, cy + r * 0.5f, cx - r * 0.87f, cy + r * 0.5f};
        for (int i = 0; i < 6; i += 2) p[i + 1] += r * 0.12f;  // (optically centred)
        poly(cv, p, 3, hw);
        break;
    }
    case SYM_VIEW: {  // two overlapping windows
        float a = ch * 0.30f, o = ch * 0.16f, w2 = hw * 0.8f;
        float b[8] = {cx - a + o, cy - a - o, cx + a + o, cy - a - o, cx + a + o, cy + a - o, cx - a + o, cy + a - o};
        float f[8] = {cx - a - o, cy - a + o, cx + a - o, cy - a + o, cx + a - o, cy + a + o, cx - a - o, cy + a + o};
        poly(cv, b, 4, w2), poly(cv, f, 4, w2);
        break;
    }
    case SYM_MENU: case SYM_OPTIONS:  // three bars
        for (int i = -1; i <= 1; i++) stroke(cv, cx - ch * 0.38f, cy + i * ch * 0.32f, cx + ch * 0.38f, cy + i * ch * 0.32f, hw);
        break;
    case SYM_CREATE:  // three upright bars
        for (int i = -1; i <= 1; i++) stroke(cv, cx + i * ch * 0.30f, cy - ch * 0.36f, cx + i * ch * 0.30f, cy + ch * 0.36f, hw);
        break;
    case SYM_PLUS: stroke(cv, cx - ch * 0.42f, cy, cx + ch * 0.42f, cy, hw * 1.4f), stroke(cv, cx, cy - ch * 0.42f, cx, cy + ch * 0.42f, hw * 1.4f); break;
    case SYM_MINUS: stroke(cv, cx - ch * 0.42f, cy, cx + ch * 0.42f, cy, hw * 1.4f); break;
    }
}

static const Icon *find(uint64_t h) {
    for (size_t i = 0; i < sizeof k_icons / sizeof *k_icons; i++) if (k_icons[i].hash == h) return &k_icons[i];
    return NULL;
}

int glyphs_patch(uint64_t hash, uint32_t format, uint32_t w, uint32_t h, uint8_t *lin) {
    const Icon *ic = find(hash);
    if (!ic) return 0;
    bool bc5 = format == 0x4B, bc4 = format == 0x49;
    if (!bc5 && !bc4) return 1;
    int fam = effective_family(), host = bind_prompt_host(ic->slot);
    Label want, orig = switch_label(ic->slot);
    if (!label_of(fam, host, &want)) return 1;
    if (want.sym == orig.sym && (want.sym != SYM_TEXT || !strcmp(want.text, orig.text))) return 1;  // (the game's own)
    if (want.sym == SYM_TEXT && !font_ready()) return 1;
    int wb = (int)(w + 3) / 4, hb = (int)(h + 3) / 4, pw = wb * 4, ph = hb * 4, bpb = bc5 ? 16 : 8;
    uint8_t *sym = malloc((size_t)pw * ph), *shape = bc5 ? malloc((size_t)pw * ph) : NULL;
    plane_get(lin, bpb, 0, wb, hb, sym);
    if (bc5) plane_get(lin, bpb, 1, wb, hb, shape);
    // where the game's symbol and shape are
    int gx0 = pw, gy0 = ph, gx1 = -1, gy1 = -1, sx0 = pw, sy0 = ph, sx1 = -1, sy1 = -1;
    for (int y = 0; y < (int)h; y++)
        for (int x = 0; x < (int)w; x++) {
            size_t i = (size_t)y * pw + x;
            bool in_shape = bc5 ? shape[i] >= 128 : true, glyph = bc5 ? in_shape && sym[i] < 128 : sym[i] >= 128;
            if (in_shape && bc5) { if (x < sx0) sx0 = x; if (x > sx1) sx1 = x; if (y < sy0) sy0 = y; if (y > sy1) sy1 = y; }
            if (glyph) { if (x < gx0) gx0 = x; if (x > gx1) gx1 = x; if (y < gy0) gy0 = y; if (y > gy1) gy1 = y; }
        }
    if (!bc5) sx0 = 0, sy0 = 0, sx1 = (int)w - 1, sy1 = (int)h - 1;
    if (gx1 < 0 || sx1 < 0) { free(sym), free(shape); return 1; }
    float shape_h = (float)(sy1 - sy0 + 1), shape_w = (float)(sx1 - sx0 + 1);
    // the letter height: the game's letter, or (for its + / -) half the button
    bool orig_text = orig.sym == SYM_TEXT;
    float ch = orig_text ? (float)(gy1 - gy0 + 1) : shape_h * (bc5 ? 0.46f : 0.42f);
    float cx = orig_text ? (gx0 + gx1 + 1) / 2.0f : (sx0 + sx1 + 1) / 2.0f, cy = orig_text ? (gy0 + gy1 + 1) / 2.0f : (sy0 + sy1 + 1) / 2.0f;
    float maxw = shape_w * 0.74f;
    Canvas cv = {calloc((size_t)pw * ph * SS * SS, sizeof(float)), pw * SS, ph * SS};
    draw_label(&cv, want, cx * SS, cy * SS, ch * SS, maxw * SS, fminf(shape_w, shape_h) * SS);
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            float a = 0;
            for (int j = 0; j < SS; j++)
                for (int i = 0; i < SS; i++) a += cv.c[(size_t)(y * SS + j) * cv.w + x * SS + i];
            a /= SS * SS;
            size_t k = (size_t)y * pw + x;
            if (bc5) { if (shape[k]) sym[k] = (uint8_t)lroundf(255 * (1 - a)); }  // (dark symbol inside the shape)
            else sym[k] = (uint8_t)lroundf(255 * a);
        }
    plane_put(lin, bpb, 0, wb, hb, sym);
    free(cv.c), free(sym), free(shape);
    return 2;
}
