// Legends Lua: the mod API's version 6 for scripts (runtime/modapi.h, runtime/hud.h) - the overlay's draw list built
// here (one HudCmd per call, the text in one pool) and handed to the runtime once a frame; keyboard / mouse; the text
// prompt; sounds; files of a mod's folder and the storage's (written by the watcher thread: no file writes on the game's
// threads). Included by legendslua.c; its Lua side is hud.lua / gui.lua (embedded too).
#include "../../runtime/hud.h"
#ifdef _WIN32
#include <direct.h>
#endif

// ---- the draw list
static HudCmd *g_hcmd;
static int g_hn, g_hcap;
static char *g_htext;
static int g_htn, g_htcap;
static float g_halpha = 1.0f;
static uint64_t g_hcalls;  // (lua.stats: commands handed over)

static HudCmd *hpush(lua_State *s) {
    if (g_hn == g_hcap) {
        if (g_hcap >= 65536) luaL_error(s, "draw: more than 65536 things in one frame");
        int cap = g_hcap ? g_hcap * 2 : 1024;
        HudCmd *n = realloc(g_hcmd, (size_t)cap * sizeof *n);
        if (!n) luaL_error(s, "draw: out of memory");
        g_hcmd = n, g_hcap = cap;
    }
    HudCmd *c = &g_hcmd[g_hn++];
    memset(c, 0, sizeof *c);
    return c;
}
// a colour: 0xRRGGBBAA, or a table {r, g, b, a} (0..255; fields r g b a or 1..4; a defaults to 255) -> 0xAABBGGRR,
// its alpha times the draw alpha
static uint32_t hcolor(lua_State *s, int i, uint32_t def) {
    uint32_t r, g, b, a;
    if (lua_isinteger(s, i)) {
        uint32_t v = (uint32_t)lua_tointeger(s, i);
        r = v >> 24, g = v >> 16 & 255, b = v >> 8 & 255, a = v & 255;
    } else if (lua_istable(s, i)) {
        int k[4];
        const char *f[4] = {"r", "g", "b", "a"};
        for (int j = 0; j < 4; j++) {
            lua_getfield(s, i, f[j]);
            if (lua_isnil(s, -1)) lua_pop(s, 1), lua_rawgeti(s, i, j + 1);
            k[j] = lua_isnumber(s, -1) ? (int)lua_tonumber(s, -1) : j == 3 ? 255 : 0;
            lua_pop(s, 1);
        }
        r = (uint32_t)(k[0] < 0 ? 0 : k[0] > 255 ? 255 : k[0]), g = (uint32_t)(k[1] < 0 ? 0 : k[1] > 255 ? 255 : k[1]);
        b = (uint32_t)(k[2] < 0 ? 0 : k[2] > 255 ? 255 : k[2]), a = (uint32_t)(k[3] < 0 ? 0 : k[3] > 255 ? 255 : k[3]);
    } else {
        uint32_t v = def;
        r = v >> 24, g = v >> 16 & 255, b = v >> 8 & 255, a = v & 255;
    }
    if (g_halpha < 1.0f) a = (uint32_t)((float)a * g_halpha + 0.5f);
    return a << 24 | b << 16 | g << 8 | r;
}
static float nf(lua_State *s, int i) { return (float)luaL_checknumber(s, i); }
static float of(lua_State *s, int i, float d) { return (float)luaL_optnumber(s, i, d); }

static int x_hud_begin(lua_State *s) { (void)s; g_hn = 0, g_htn = 0, g_halpha = 1.0f; return 0; }
static int x_hud_end(lua_State *s) {
    if (M && M->version >= 6) M->hud_submit(g_hcmd, g_hn, g_htext, g_htn);
    g_hcalls += (uint64_t)g_hn;
    lua_pushinteger(s, g_hn);
    return 1;
}
static int x_hud_alpha(lua_State *s) {
    float a = (float)luaL_optnumber(s, 1, 1);
    g_halpha = a < 0 ? 0 : a > 1 ? 1 : a;
    lua_pushnumber(s, g_halpha);
    return 1;
}
static int x_hud_rect(lua_State *s) {  // (x, y, w, h, colour, rounding)
    HudCmd *c = hpush(s);
    c->op = HUD_RECT_FILL, c->x = nf(s, 1), c->y = nf(s, 2), c->w = nf(s, 3), c->h = nf(s, 4);
    c->color = hcolor(s, 5, 0xffffffff), c->r = of(s, 6, 0);
    return 0;
}
static int x_hud_rect_outline(lua_State *s) {  // (x, y, w, h, colour, rounding, thickness)
    HudCmd *c = hpush(s);
    c->op = HUD_RECT, c->x = nf(s, 1), c->y = nf(s, 2), c->w = nf(s, 3), c->h = nf(s, 4);
    c->color = hcolor(s, 5, 0xffffffff), c->r = of(s, 6, 0), c->t = of(s, 7, 1);
    return 0;
}
static int x_hud_line(lua_State *s) {  // (x1, y1, x2, y2, colour, thickness)
    HudCmd *c = hpush(s);
    c->op = HUD_LINE, c->x = nf(s, 1), c->y = nf(s, 2), c->w = nf(s, 3), c->h = nf(s, 4);
    c->color = hcolor(s, 5, 0xffffffff), c->t = of(s, 6, 1);
    return 0;
}
static int x_hud_circle(lua_State *s) {  // (x, y, radius, colour, thickness - nil / 0: filled)
    HudCmd *c = hpush(s);
    float t = of(s, 5, 0);
    c->op = t > 0 ? HUD_CIRCLE : HUD_CIRCLE_FILL, c->x = nf(s, 1), c->y = nf(s, 2), c->r = nf(s, 3), c->t = t;
    c->color = hcolor(s, 4, 0xffffffff);
    return 0;
}
static int x_hud_tri(lua_State *s) {  // (x1, y1, x2, y2, x3, y3, colour)
    HudCmd *c = hpush(s);
    c->op = HUD_TRI_FILL, c->x = nf(s, 1), c->y = nf(s, 2), c->w = nf(s, 3), c->h = nf(s, 4), c->u0 = nf(s, 5), c->v0 = nf(s, 6);
    c->color = hcolor(s, 7, 0xffffffff);
    return 0;
}
static int x_hud_grad(lua_State *s) {  // (x, y, w, h, top colour, bottom colour)
    HudCmd *c = hpush(s);
    c->op = HUD_GRAD, c->x = nf(s, 1), c->y = nf(s, 2), c->w = nf(s, 3), c->h = nf(s, 4);
    c->color = hcolor(s, 5, 0xffffffff), c->a = (int32_t)hcolor(s, 6, 0x000000ff);
    return 0;
}
static int x_hud_text(lua_State *s) {  // (text, x, y, size, colour, align flags, wrap width)
    size_t len;
    const char *t = luaL_checklstring(s, 1, &len);
    if (len > 4096) len = 4096;
    if (g_htn + (int)len > g_htcap) {
        int cap = g_htcap ? g_htcap : 16384;
        while (cap < g_htn + (int)len) cap *= 2;
        if (cap > (8 << 20)) return luaL_error(s, "draw: too much text in one frame");
        char *n = realloc(g_htext, (size_t)cap);
        if (!n) return luaL_error(s, "draw: out of memory");
        g_htext = n, g_htcap = cap;
    }
    HudCmd *c = hpush(s);
    c->op = HUD_TEXT, c->x = nf(s, 2), c->y = nf(s, 3), c->r = of(s, 4, 22), c->color = hcolor(s, 5, 0xffffffff);
    c->align = (uint8_t)luaL_optinteger(s, 6, 0), c->w = of(s, 7, 0);
    memcpy(g_htext + g_htn, t, len);
    c->a = g_htn, c->n = (int32_t)len;
    g_htn += (int)len;
    return 0;
}
static int x_hud_image(lua_State *s) {  // (id, x, y, w, h, tint, u0, v0, u1, v1, rounding)
    HudCmd *c = hpush(s);
    c->op = HUD_IMAGE, c->a = (int32_t)luaL_checkinteger(s, 1), c->x = nf(s, 2), c->y = nf(s, 3), c->w = nf(s, 4), c->h = nf(s, 5);
    c->color = hcolor(s, 6, 0xffffffff), c->u0 = of(s, 7, 0), c->v0 = of(s, 8, 0), c->u1 = of(s, 9, 1), c->v1 = of(s, 10, 1);
    c->r = of(s, 11, 0);
    return 0;
}
static int x_hud_clip(lua_State *s) {
    HudCmd *c = hpush(s);
    c->op = HUD_CLIP, c->x = nf(s, 1), c->y = nf(s, 2), c->w = nf(s, 3), c->h = nf(s, 4);
    return 0;
}
static int x_hud_unclip(lua_State *s) { HudCmd *c = hpush(s); c->op = HUD_UNCLIP; return 0; }
static void need6(lua_State *s) {
    need_m(s);
    if (M->version < 6) luaL_error(s, "this game build's mod API is too old for the overlay / input / sounds (version %d, needs 6): update the game", (int)M->version);
}
static int x_hud_size(lua_State *s) {
    float o[6] = {1920, 1080, 0, 0, 1920, 1080};
    if (M && M->version >= 6) M->hud_size(o);
    for (int i = 0; i < 6; i++) lua_pushnumber(s, o[i]);
    return 6;
}
static int x_hud_measure(lua_State *s) {  // (text, size, wrap) -> w, h
    size_t len;
    const char *t = luaL_checklstring(s, 1, &len);
    float o[2] = {0, 0}, size = of(s, 2, 22), wrap = of(s, 3, 0);
    if (M && M->version >= 6) M->hud_measure(t, (int)len, size, wrap, o);
    else o[0] = 0.52f * size * (float)len, o[1] = size;
    lua_pushnumber(s, o[0]), lua_pushnumber(s, o[1]);
    return 2;
}
static int x_hud_load_image(lua_State *s) {  // (absolute path) -> id, w, h | nil
    need6(s);
    int wh[2] = {0, 0}, id = M->hud_image(luaL_checkstring(s, 1), wh);
    if (!id) return 0;
    lua_pushinteger(s, id), lua_pushinteger(s, wh[0]), lua_pushinteger(s, wh[1]);
    return 3;
}
static int x_hud_to_screen(lua_State *s) {  // (x, y, z) -> sx, sy, metres | nil
    need6(s);
    float p[3] = {nf(s, 1), nf(s, 2), nf(s, 3)}, o[3];
    if (!M->hud_to_screen(p, o)) return 0;
    lua_pushnumber(s, o[0]), lua_pushnumber(s, o[1]), lua_pushnumber(s, o[2]);
    return 3;
}

// ---- keyboard / mouse, the text prompt
static int x_input_grab(lua_State *s) { need6(s); M->input_grab(lua_toboolean(s, 1)); return 0; }
static int x_input_events(lua_State *s) {
    need6(s);
    HudInputEvent ev[64];
    int n = M->input_events(ev, 64);
    if (!n) return 0;  // (nothing: no table made each frame)
    lua_createtable(s, n, 0);
    static const char *types[] = {"", "key_down", "key_up", "mouse_down", "mouse_up", "wheel", "text", "move"};
    for (int i = 0; i < n; i++) {
        lua_createtable(s, 0, 8);
        lua_pushstring(s, ev[i].type < 8 ? types[ev[i].type] : "?"), lua_setfield(s, -2, "type");
        if (ev[i].type == HUD_KEY_DOWN || ev[i].type == HUD_KEY_UP) {
            lua_pushstring(s, ev[i].name), lua_setfield(s, -2, "key");
            lua_pushboolean(s, ev[i].repeat), lua_setfield(s, -2, "rep");
            lua_pushboolean(s, ev[i].mods & 0x3), lua_setfield(s, -2, "shift");
            lua_pushboolean(s, ev[i].mods & 0xc0), lua_setfield(s, -2, "ctrl");
            lua_pushboolean(s, ev[i].mods & 0x300), lua_setfield(s, -2, "alt");
        }
        if (ev[i].type == HUD_TEXT_INPUT) lua_pushstring(s, ev[i].name), lua_setfield(s, -2, "text");
        if (ev[i].type == HUD_MOUSE_DOWN || ev[i].type == HUD_MOUSE_UP) {
            static const char *b[] = {"?", "left", "middle", "right", "x1", "x2"};
            lua_pushstring(s, ev[i].button < 6 ? b[ev[i].button] : "?"), lua_setfield(s, -2, "button");
        }
        if (ev[i].type == HUD_WHEEL) lua_pushnumber(s, ev[i].wheel), lua_setfield(s, -2, "wheel");
        lua_pushnumber(s, ev[i].x), lua_setfield(s, -2, "x");
        lua_pushnumber(s, ev[i].y), lua_setfield(s, -2, "y");
        lua_rawseti(s, -2, i + 1);
    }
    return 1;
}
static int x_input_mouse(lua_State *s) {
    need6(s);
    float o[3];
    M->input_mouse(o);
    lua_pushnumber(s, o[0]), lua_pushnumber(s, o[1]), lua_pushinteger(s, (lua_Integer)o[2]);
    return 3;
}
static int x_text_prompt(lua_State *s) {
    need6(s);
    lua_pushboolean(s, M->text_prompt(luaL_optstring(s, 1, ""), luaL_optstring(s, 2, ""), (int)luaL_optinteger(s, 3, 60)));
    return 1;
}
static int x_text_prompt_poll(lua_State *s) {
    need6(s);
    char out[512];
    int r = M->text_prompt_poll(out, sizeof out);
    lua_pushinteger(s, r);
    if (r == 1) { lua_pushstring(s, out); return 2; }
    return 1;
}

// ---- sounds
static int x_sound_load(lua_State *s) {
    need6(s);
    int id = M->sound_load(luaL_checkstring(s, 1));
    if (!id) return 0;
    lua_pushinteger(s, id);
    return 1;
}
static int x_sound_play(lua_State *s) {  // (id, volume, pitch, x, y, z (nil: 2D), range, loop)
    need6(s);
    float p[3];
    bool at = lua_isnumber(s, 4);
    if (at) p[0] = nf(s, 4), p[1] = nf(s, 5), p[2] = nf(s, 6);
    lua_pushinteger(s, M->sound_play((int)luaL_checkinteger(s, 1), of(s, 2, 1), of(s, 3, 1), at ? p : NULL, of(s, 7, 30),
                                     lua_toboolean(s, 8)));
    return 1;
}
static int x_sound_stop(lua_State *s) { need6(s); M->sound_stop((int)luaL_optinteger(s, 1, 0)); return 0; }
static int x_sound_set(lua_State *s) {  // (voice, volume (nil: kept), x, y, z)
    need6(s);
    float p[3];
    bool at = lua_isnumber(s, 3);
    if (at) p[0] = nf(s, 3), p[1] = nf(s, 4), p[2] = nf(s, 5);
    M->sound_set((int)luaL_checkinteger(s, 1), lua_isnumber(s, 2) ? nf(s, 2) : -1.0f, at ? p : NULL);
    return 0;
}
static int x_sound_playing(lua_State *s) { need6(s); lua_pushboolean(s, M->sound_playing((int)luaL_checkinteger(s, 1))); return 1; }

// ---- files: a mod's folder read (the game's thread: small files, at a script's load), writes queued for the watcher
static int x_save_id(lua_State *s) {
    char id[128] = "";
    if (M && M->version >= 6) M->save_id(id, sizeof id);
    lua_pushstring(s, id);
    return 1;
}
static int x_file_read(lua_State *s) {  // (absolute path, max bytes) -> text | nil, error
    const char *p = luaL_checkstring(s, 1);
    lua_Integer max = luaL_optinteger(s, 2, 4 << 20);
    FILE *f = fopen(p, "rb");
    if (!f) { lua_pushnil(s), lua_pushstring(s, "no such file"); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > max) { fclose(f); lua_pushnil(s), lua_pushstring(s, "too big"); return 2; }
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return luaL_error(s, "out of memory"); }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    lua_pushlstring(s, b, got);
    free(b);
    return 1;
}
static int x_file_exists(lua_State *s) {
    struct stat sb;
    lua_pushboolean(s, !stat(luaL_checkstring(s, 1), &sb));
    return 1;
}
#define NWRITE 32
static struct { char path[1024]; char *data; size_t len; } g_wq[NWRITE];
static int g_nwq;
static pthread_mutex_t g_wq_lock = PTHREAD_MUTEX_INITIALIZER;
static int x_file_write_later(lua_State *s) {  // (absolute path, text): written within half a second (the same path: the newest)
    const char *p = luaL_checkstring(s, 1);
    size_t len;
    const char *d = luaL_checklstring(s, 2, &len);
    if (len > (16u << 20)) return luaL_error(s, "file too big (16 MB at most)");
    char *copy = malloc(len ? len : 1);
    if (!copy) return luaL_error(s, "out of memory");
    memcpy(copy, d, len);
    pthread_mutex_lock(&g_wq_lock);
    int i = 0;
    while (i < g_nwq && strcmp(g_wq[i].path, p)) i++;
    if (i == g_nwq && g_nwq == NWRITE) { pthread_mutex_unlock(&g_wq_lock); free(copy); return luaL_error(s, "too many files waiting to be written"); }
    if (i == g_nwq) g_nwq++, snprintf(g_wq[i].path, sizeof g_wq[i].path, "%s", p);
    else free(g_wq[i].data);
    g_wq[i].data = copy, g_wq[i].len = len;
    pthread_mutex_unlock(&g_wq_lock);
    return 0;
}
static void mkdirs(const char *path) {  // (the folders of a file's path)
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') {
            char k = *c;
            *c = 0;
#ifdef _WIN32
            _mkdir(p);
#else
            mkdir(p, 0755);
#endif
            *c = k;
        }
}
static void ext_flush_writes(void) {  // (the watcher thread)
    for (;;) {
        pthread_mutex_lock(&g_wq_lock);
        if (!g_nwq) { pthread_mutex_unlock(&g_wq_lock); return; }
        char path[1024];
        snprintf(path, sizeof path, "%s", g_wq[0].path);
        char *data = g_wq[0].data;
        size_t len = g_wq[0].len;
        g_nwq--;
        memmove(&g_wq[0], &g_wq[1], (size_t)g_nwq * sizeof g_wq[0]);
        pthread_mutex_unlock(&g_wq_lock);
        mkdirs(path);
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        bool ok = f && fwrite(data, 1, len, f) == len;
        if (f) ok = !fclose(f) && ok;
        if (ok) {
#ifdef _WIN32
            remove(path);
#endif
            ok = !rename(tmp, path);
        }
        if (!ok) A->log("[lua] storage: %s not written\n", path);
        free(data);
    }
}

static void ext_register(lua_State *s) {  // (into the table at the top of the stack: api.lua's rt)
    static const luaL_Reg r[] = {
        {"hud_begin", x_hud_begin}, {"hud_end", x_hud_end}, {"hud_alpha", x_hud_alpha}, {"hud_rect", x_hud_rect},
        {"hud_rect_outline", x_hud_rect_outline}, {"hud_line", x_hud_line}, {"hud_circle", x_hud_circle}, {"hud_tri", x_hud_tri},
        {"hud_grad", x_hud_grad}, {"hud_text", x_hud_text}, {"hud_image", x_hud_image}, {"hud_clip", x_hud_clip},
        {"hud_unclip", x_hud_unclip}, {"hud_size", x_hud_size}, {"hud_measure", x_hud_measure}, {"hud_load_image", x_hud_load_image},
        {"hud_to_screen", x_hud_to_screen}, {"input_grab", x_input_grab}, {"input_events", x_input_events},
        {"input_mouse", x_input_mouse}, {"text_prompt", x_text_prompt}, {"text_prompt_poll", x_text_prompt_poll},
        {"sound_load", x_sound_load}, {"sound_play", x_sound_play}, {"sound_stop", x_sound_stop}, {"sound_set", x_sound_set},
        {"sound_playing", x_sound_playing}, {"save_id", x_save_id}, {"file_read", x_file_read}, {"file_exists", x_file_exists},
        {"file_write_later", x_file_write_later}, {NULL, NULL}};
    luaL_setfuncs(s, r, 0);
    lua_pushstring(s, A->data_root()), lua_setfield(s, -2, "data_root");
}
