// Scripts' overlay (runtime/hud.h; the mod API's hud_* - Legends Lua's draw API, contrib/legendslua/hud.lua): each
// frame a plugin hands over its whole draw list (rectangles, lines, circles, text, images, clip rectangles) built on the
// game's thread; the render thread draws the latest one over the game's image (ui/menu.mm's overlay, after the speech
// bubbles, under the runtime's own windows). Images (PNG / JPG, stb_image) are read when asked and uploaded on their
// first draw, kept by path. Text is the overlay's font (DejaVu Sans / Segoe UI) with Japanese / Chinese (a Noto Sans CJK
// JP subset) and emoji (Noto Emoji) merged in - third_party/fonts.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <pthread.h>
#include <atomic>
#include <string>
#include <vector>

#include "imgui.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "../../third_party/stb/stb_image.h"

extern "C" {
#include "../hle.h"
#include "../hud.h"
#include "../paths.h"
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);  // (nvn/vulkan/imgui_glue.cpp)
}

// ---- fonts
// Merged into the overlay's main font (menu.mm init): the CJK subset and the emoji font, at its size. Missing files: the
// font as it was (the characters show as '?').
void hud_fonts_merge(ImGuiIO &io, ImFont *into, float size) {
    if (!into) return;
    static ImVector<ImWchar> cjk;
    if (cjk.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        b.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        static const ImWchar more[] = {0x2010, 0x205E, 0x2190, 0x21FF, 0x2460, 0x24FF, 0x25A0, 0x26FF, 0x3000, 0x30FF,
                                       0x31F0, 0x31FF, 0xFF00, 0xFFEF, 0};
        b.AddRanges(more);
        b.BuildRanges(&cjk);
    }
    static const ImWchar emoji[] = {0x2190, 0x21FF, 0x2300, 0x23FF, 0x2600, 0x27BF, 0x2B00, 0x2BFF, 0x1F000, 0x1F02F,
                                    0x1F0A0, 0x1F0FF, 0x1F100, 0x1F1FF, 0x1F300, 0x1F5FF, 0x1F600, 0x1F64F, 0x1F680,
                                    0x1F6FF, 0x1F900, 0x1F9FF, 0x1FA70, 0x1FAFF, 0};
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.OversampleH = 1, cfg.OversampleV = 1, cfg.PixelSnapH = true;
    cfg.DstFont = into;
    char p[1024];
    if (rt_resource("NotoSansCJKjp-Regular-subset.otf", "third_party/fonts/NotoSansJP/NotoSansCJKjp-Regular-subset.otf", p, sizeof p))
        io.Fonts->AddFontFromFileTTF(p, size, &cfg, cjk.Data);
    else LOG("[hud] no Japanese / Chinese font (NotoSansCJKjp-Regular-subset.otf): those characters show as '?'\n");
    if (rt_resource("NotoEmoji-Regular.ttf", "third_party/fonts/NotoEmoji/NotoEmoji-Regular.ttf", p, sizeof p))
        io.Fonts->AddFontFromFileTTF(p, size, &cfg, emoji);
    else LOG("[hud] no emoji font (NotoEmoji-Regular.ttf)\n");
}
static std::atomic<ImFont *> g_font{nullptr}, g_font_big{nullptr};
void hud_fonts_ready(ImFont *font, ImFont *big) {
    if (!g_font.load()) g_font = font, g_font_big = big;
}

// ---- the overlay's size (each drawn frame, menu.mm): the window image, and the game's interface box in it - 16:9,
// centred (a wider window - "Fill the window", ultrawide - keeps the game's interface in the middle 16:9)
static std::atomic<uint64_t> g_size{0};
void hud_frame_size(float w, float h) {
    uint64_t v = (uint64_t)(uint32_t)w << 32 | (uint32_t)h;
    if (g_size.load() != v) g_size = v;
}
extern "C" void hud_size(float out[6]) {
    uint64_t v = g_size.load();
    float w = (float)(v >> 32), h = (float)(v & 0xffffffff);
    if (w <= 0 || h <= 0) w = 1920, h = 1080;  // (not drawn yet)
    float bw = w, bh = h;
    if (w / h > 16.0f / 9.0f) bw = h * 16.0f / 9.0f;
    else bh = w * 9.0f / 16.0f;
    out[0] = w, out[1] = h, out[2] = (w - bw) * 0.5f, out[3] = (h - bh) * 0.5f, out[4] = bw, out[5] = bh;
}

// ---- text size (any thread: the font's glyph tables are only read once built)
extern "C" void hud_measure(const char *text, int len, float size, float wrap, float out[2]) {
    ImFont *f = g_font.load();
    if (!text || len < 0) len = 0;
    if (!f) {  // (the overlay's font isn't built yet: an estimate)
        out[0] = 0.52f * size * (float)len, out[1] = size;
        return;
    }
    ImVec2 s = f->CalcTextSizeA(size, FLT_MAX, wrap > 0 ? wrap : 0.0f, text, text + len);
    out[0] = s.x, out[1] = s.y;
}

// ---- images, by path
struct Image {
    std::string path;
    int w = 0, h = 0;
    unsigned char *rgba = nullptr;  // (until uploaded)
    ImTextureID tex = 0;
    bool failed = false;
};
static std::vector<Image> g_img;  // (id = index + 1)
static pthread_mutex_t g_img_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_img_bytes;
#define IMG_MAX 256
#define IMG_BYTES_MAX (256ull << 20)
extern "C" int hud_image(const char *path, int out_wh[2]) {
    if (!path || !*path) return 0;
    pthread_mutex_lock(&g_img_lock);
    for (size_t i = 0; i < g_img.size(); i++)
        if (g_img[i].path == path) {
            int id = g_img[i].failed ? 0 : (int)i + 1;
            if (out_wh) out_wh[0] = g_img[i].w, out_wh[1] = g_img[i].h;
            pthread_mutex_unlock(&g_img_lock);
            return id;
        }
    pthread_mutex_unlock(&g_img_lock);
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load(path, &w, &h, &n, 4);
    pthread_mutex_lock(&g_img_lock);
    Image im;
    im.path = path, im.w = w, im.h = h, im.rgba = px;
    if (!px || w <= 0 || h <= 0 || w > 8192 || h > 8192 || g_img.size() >= IMG_MAX || g_img_bytes + (uint64_t)w * h * 4 > IMG_BYTES_MAX) {
        LOG("[hud] image %s: %s\n", path, !px ? stbi_failure_reason() : "too big, or too many images");
        if (px) stbi_image_free(px);
        im.rgba = nullptr, im.failed = true;
    } else g_img_bytes += (uint64_t)w * h * 4;
    g_img.push_back(im);
    int id = im.failed ? 0 : (int)g_img.size();
    if (out_wh) out_wh[0] = w, out_wh[1] = h;
    pthread_mutex_unlock(&g_img_lock);
    if (id) LOG("[hud] image %s: %dx%d (id %d)\n", path, w, h, id);
    return id;
}
static ImTextureID image_tex(int id) {  // (the render thread: uploaded the first time it's drawn)
    if (id <= 0) return 0;
    pthread_mutex_lock(&g_img_lock);
    ImTextureID t = 0;
    if ((size_t)id <= g_img.size()) {
        Image &im = g_img[(size_t)id - 1];
        if (!im.tex && im.rgba) {
            im.tex = (ImTextureID)vk_imgui_texture_rgba(im.rgba, im.w, im.h);
            stbi_image_free(im.rgba);
            im.rgba = nullptr;
            if (!im.tex) im.failed = true;
        }
        t = im.tex;
    }
    pthread_mutex_unlock(&g_img_lock);
    return t;
}

// ---- the draw list: the plugin's latest, swapped in whole
static std::vector<HudCmd> g_cmd[2];
static std::vector<char> g_text[2];
static int g_front;
static std::atomic<int> g_have{0};       // (commands to draw)
static std::atomic<uint64_t> g_submit_ms{0};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
extern "C" void hud_submit(const void *cmds, int n, const char *text, int text_len) {
    if (n < 0) n = 0;
    if (n > 65536) n = 65536;
    if (text_len < 0 || !text) text_len = 0;
    pthread_mutex_lock(&g_lock);
    int b = g_front ^ 1;
    g_cmd[b].assign((const HudCmd *)cmds, (const HudCmd *)cmds + n);
    g_text[b].assign(text, text + text_len);
    g_front = b;
    g_have = n > 0;
    pthread_mutex_unlock(&g_lock);
    g_submit_ms = SDL_GetTicks();
}
bool hud_wanted(void) {
    // (a script's list, kept drawn for a second after its last hand-over: scripts tick every frame while drawing)
    return g_have.load() && SDL_GetTicks() - g_submit_ms.load() < 1000;
}

static void draw_text(ImDrawList *dl, const HudCmd &c, const char *t, const char *te) {
    ImFont *f = c.r > 44.0f && g_font_big.load() ? g_font_big.load() : g_font.load();
    if (!f || t >= te) return;
    float size = c.r > 1 ? c.r : 20, wrap = c.w > 0 ? c.w : 0;
    int ha = c.align & 3;
    float x = c.x, y = c.y;
    if (c.align & (HUD_ALIGN_MIDDLE | HUD_ALIGN_BOTTOM)) {
        ImVec2 s = f->CalcTextSizeA(size, FLT_MAX, wrap, t, te);
        y -= c.align & HUD_ALIGN_BOTTOM ? s.y : s.y * 0.5f;
    }
    ImU32 col = c.color, shade = IM_COL32(0, 0, 0, (int)((c.color >> 24) * 0.75f));
    float so = fmaxf(1.0f, size / 16.0f);
    if (ha == HUD_ALIGN_LEFT && !wrap) {
        if (c.align & HUD_TEXT_SHADOW) dl->AddText(f, size, ImVec2(x + so, y + so), shade, t, te);
        dl->AddText(f, size, ImVec2(x, y), col, t, te);
        return;
    }
    // line by line (wrapped at wrap): each one placed by the alignment - x is the left edge / middle / right edge,
    // within the wrap width when there is one
    const char *s = t;
    float lh = size * 1.18f;
    while (s < te) {
        const char *e = wrap > 0 ? f->CalcWordWrapPositionA(size / f->FontSize, s, te, wrap) : te;
        const char *nl = (const char *)memchr(s, '\n', (size_t)(e - s));
        if (nl) e = nl;
        if (e == s && s < te && *s != '\n') e = s + 1;
        const char *le = e;
        while (le > s && le[-1] == ' ') le--;
        float lw = f->CalcTextSizeA(size, FLT_MAX, 0, s, le).x;
        float lx = x;
        if (ha == HUD_ALIGN_CENTER) lx = wrap > 0 ? x + (wrap - lw) * 0.5f : x - lw * 0.5f;
        else if (ha == HUD_ALIGN_RIGHT) lx = wrap > 0 ? x + wrap - lw : x - lw;
        if (c.align & HUD_TEXT_SHADOW) dl->AddText(f, size, ImVec2(lx + so, y + so), shade, s, le);
        dl->AddText(f, size, ImVec2(lx, y), col, s, le);
        y += lh;
        s = e;
        if (s < te && *s == '\n') s++;
        if (wrap > 0) while (s < te && *s == ' ') s++;
    }
}

void hud_render(ImGuiIO &io, float scale) {
    (void)scale;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();  // (over the speech bubbles, under the runtime's windows)
    pthread_mutex_lock(&g_lock);
    const std::vector<HudCmd> &cmds = g_cmd[g_front];
    const std::vector<char> &text = g_text[g_front];
    int clips = 0;
    dl->PushClipRect(ImVec2(0, 0), io.DisplaySize, false);
    clips++;
    for (const HudCmd &c : cmds) {
        ImVec2 a(c.x, c.y), b(c.x + c.w, c.y + c.h);
        switch (c.op) {
        case HUD_RECT_FILL: dl->AddRectFilled(a, b, c.color, c.r); break;
        case HUD_RECT: dl->AddRect(a, b, c.color, c.r, 0, c.t > 0 ? c.t : 1.0f); break;
        case HUD_LINE: dl->AddLine(a, ImVec2(c.w, c.h), c.color, c.t > 0 ? c.t : 1.0f); break;
        case HUD_CIRCLE_FILL: dl->AddCircleFilled(a, c.r, c.color); break;
        case HUD_CIRCLE: dl->AddCircle(a, c.r, c.color, 0, c.t > 0 ? c.t : 1.0f); break;
        case HUD_TRI_FILL: dl->AddTriangleFilled(a, ImVec2(c.w, c.h), ImVec2(c.u0, c.v0), c.color); break;
        case HUD_GRAD: dl->AddRectFilledMultiColor(a, b, c.color, c.color, (ImU32)c.a, (ImU32)c.a); break;
        case HUD_TEXT:
            if (c.a >= 0 && c.n > 0 && (size_t)c.a + (size_t)c.n <= text.size()) draw_text(dl, c, text.data() + c.a, text.data() + c.a + c.n);
            break;
        case HUD_IMAGE: {
            ImTextureID t = image_tex(c.a);
            if (!t) break;
            if (c.r > 0) dl->AddImageRounded(t, a, b, ImVec2(c.u0, c.v0), ImVec2(c.u1, c.v1), c.color, c.r);
            else dl->AddImage(t, a, b, ImVec2(c.u0, c.v0), ImVec2(c.u1, c.v1), c.color);
            break;
        }
        case HUD_CLIP:
            if (clips < 64) dl->PushClipRect(a, b, true), clips++;
            break;
        case HUD_UNCLIP:
            if (clips > 1) dl->PopClipRect(), clips--;
            break;
        default: break;
        }
    }
    while (clips-- > 0) dl->PopClipRect();
    pthread_mutex_unlock(&g_lock);
}

// The overlay's cost on the render thread (menu.mm, each drawn frame: building the speech bubbles' and scripts' ImGui
// lists, and the whole overlay - ImGui's frame, those, its draw data recorded). LARECOMP_HUD_STATS=<frames>: averages
// and maxima logged every that many frames ("[hud] render ..."), with the draw list's size.
void hud_render_stats(uint64_t bubbles_ns, uint64_t hud_ns, uint64_t all_ns, int vertices, bool menu) {
    static long every = -1;
    if (every < 0) every = getenv("LARECOMP_HUD_STATS") ? atol(getenv("LARECOMP_HUD_STATS")) : 0;
    if (every <= 0) return;
    static uint64_t n, sb, sh, sa, mb, mh, ma, sv;
    static int menus;
    n++, sb += bubbles_ns, sh += hud_ns, sa += all_ns, sv += (uint64_t)vertices, menus += menu;
    if (bubbles_ns > mb) mb = bubbles_ns;
    if (hud_ns > mh) mh = hud_ns;
    if (all_ns > ma) ma = all_ns;
    if ((long)n < every) return;
    pthread_mutex_lock(&g_lock);
    size_t cmds = g_cmd[g_front].size();
    pthread_mutex_unlock(&g_lock);
    LOG("[hud] render over %llu frames: scripts' list %.1f us (max %.1f, %zu commands), bubbles %.1f us (max %.1f), the "
        "whole overlay %.1f us (max %.1f), %llu vertices%s\n", (unsigned long long)n, sh / 1e3 / n, mh / 1e3, cmds, sb / 1e3 / n,
        mb / 1e3, sa / 1e3 / n, ma / 1e3, (unsigned long long)(sv / n), menus ? " (the runtime's menu up some of it)" : "");
    n = sb = sh = sa = mb = mh = ma = sv = 0, menus = 0;
}
