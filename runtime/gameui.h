// The games' own interface layouts, edited (runtime/hle/gameui.c, the same in both games): any pane of any layout
// moved, scaled, hidden, faded, re-anchored to the real screen's edges, its text or image changed, or copied as a new
// pane. Both games build their interface with Nintendo's nn::ui2d (BFLYT layouts in .arc archives: Z-A ui/data/<group>/
// <name>.arc, Arceus bin/appli/<group>/bin/<name>.arc): a layout is a tree of named panes (Pane, Picture, TextBox,
// Window, Parts...), each with a translate / rotate / scale / size / alpha / visibility the game animates; once a
// frame each layout's Layout::Calculate turns them into the matrices its draw uses. The edits are made just before that
// and taken back right after, so the game's animations and its own changes go on as before underneath.
//
// Who edits: the runtime (ultrawide: the field HUD anchored to the real screen edges with "Fill the window"), mods
// without code (mods/<name>/ui/<layout>.ini) and scripts (the mod API's gameui_*: Lua gameui.set...). Any thread.
//
// Layout names are the BFLYT's ("hud_minimap_field_00"); pane names the layout's own ("N_minimap", "T_name"...).
// gameui_layouts / gameui_panes list what is on screen now, to find them.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What an edit sets (GameUiSpec.has)
enum {
    GUI_X = 1 << 0,       // x / y: moved by that many layout units (a 1920 x 1080 space, y up)
    GUI_Y = 1 << 1,
    GUI_SX = 1 << 2,      // sx / sy: its scale times that
    GUI_SY = 1 << 3,
    GUI_ALPHA = 1 << 4,   // alpha 0..1: its own alpha times that
    GUI_HIDE = 1 << 5,    // hide 1: not drawn (0: drawn even where the game hides it - "show")
    GUI_ANCHOR = 1 << 6,  // anchor: kept that far from the real screen's edge as from the 16:9 layout's (GUI_ANCHOR_*)
    GUI_TEXT = 1 << 7,    // text: a TextBox's text (UTF-8)
    GUI_IMAGE = 1 << 8,   // image: a Picture's texture, by the name of a texture the game has loaded (the layout's own,
                          // or another layout's on screen)
    GUI_MODE = 1 << 9,    // (the layout itself, pane "") mode: how it is fitted to a screen wider than 16:9 (GUI_MODE_*)
    GUI_FILL = 1 << 10,   // fill 1: stretched across the whole screen's width (a backdrop, a dimming); on the layout
                          // itself, fill 0: none of its panes is (a pane as big as the layout is, by default)
    GUI_FOLLOW = 1 << 11, // follow 1: its position follows the world (a bubble over a character: the game places it for
                          // a 16:9 screen - the place is spread to the real screen's width)
    GUI_W = 1 << 12,      // w / h: its size set (layout units)
    GUI_H = 1 << 13,
};
enum { GUI_ANCHOR_LEFT = 1, GUI_ANCHOR_RIGHT = 2, GUI_ANCHOR_TOP = 4, GUI_ANCHOR_BOTTOM = 8,
       GUI_ANCHOR_AUTO = 16 };  // (auto: to the edge it is nearest - what is shown of it left of a fifth of the layout
                                // from the middle goes left, right of it right, else stays; each frame)
// A layout on a screen wider (or taller) than 16:9
enum {
    GUI_MODE_BOX = 0,      // (default) the whole layout in a 16:9 box in the middle of the screen; anchor on the
                           // layout itself (pane "") moves the box to that edge
    GUI_MODE_WIDE = 1,     // the layout's space made as wide as the screen: its panes where they were (around the
                           // middle), the ones anchored at the edges; a pane as big as the layout fills the screen
    GUI_MODE_STRETCH = 2,  // stretched across the whole screen (as the game itself would draw it)
};

typedef struct {
    uint32_t has;           // GUI_*
    float x, y, sx, sy, alpha, w, h;
    uint8_t hide, anchor, mode, fill, follow;
    char text[256];         // UTF-8
    char image[200];        // (a .png / .dds: a path from the game folder)
} GameUiSpec;

enum { GUI_OWNER_BUILTIN = 0, GUI_OWNER_DATA = 1, GUI_OWNER_SCRIPT = 2, GUI_OWNERS };

// An edit of layout `layout`'s pane `pane` ("" or NULL: the layout itself - mode, anchor, alpha, hide). The fields set
// in s->has replace that owner's earlier ones for that pane; scripts' edits beat mods' data, which beat the runtime's
// own. Answers 1 (0: the table is full). gameui_unset drops an owner's edits of that pane (pane NULL: of the whole
// layout; layout NULL: all of them).
int gameui_set(const char *layout, const char *pane, const GameUiSpec *s, int owner);
void gameui_unset(const char *layout, const char *pane, int owner);
// A new pane: a copy of `src` (and what is under it) of that layout, named `name`, drawn as the game draws `src`
// (moved / scaled / its own text then by gameui_set on `name`). Answers 1 asked, 0 not (no such layout open, the game
// has no copy function, the table is full).
int gameui_clone(const char *layout, const char *src, const char *name, int owner);
// The layouts calculated in the last second, one a line: "<name> <panes> <width>x<height>"; answers the length.
int gameui_layouts(char *out, int cap);
// A layout's panes (it must be on screen), one a line, tree order: "<depth> <name> <type> <x> <y> <w> <h> <shown>" -
// type Pane / Picture / TextBox / Window / Parts / Bounding / Capture / Shape / ?; x y its place in the layout (its
// centre, layout units), w h its size there. 0 / -1: not on screen (in the last second).
int gameui_panes(const char *layout, char *out, int cap);
// A TextBox's text now (UTF-8), 0: none
int gameui_text(const char *layout, const char *pane, char *out, int cap);
// Reads mods/<name>/ui/*.ini of the mods that are on (again: a file changed). Called at boot.
void gameui_load_mods(void);

// ---- runtime side
// Per game (Z-A hle/za_fixes.c, Arceus hle/pla_wide.c): where nn::ui2d is in its code, and its fields
typedef struct {
    const char *game;            // "za" / "pla"
    uint64_t layout_calc;        // nn::ui2d::Layout::Calculate(this, DrawInfo&, bool)
    uint64_t layout_draw;        // nn::ui2d::Layout::Draw(this, DrawInfo&, CommandBuffer&)
    uint32_t lay_name;           // Layout: its name (const char*)
    uint32_t di_proj;            // DrawInfo: the projection (4 x 4 rows)
    uint32_t di_target;          // DrawInfo: the target's width, height (int32; 0: not known - the screen's)
    uint64_t vt[9];              // vtables: Pane Picture TextBox Window Parts Bounding Capture Shape(Circle) Layout
    uint32_t tb_buf, tb_cap, tb_len;  // TextBox: its text (char16_t*), the buffer's length, the text's (uint16)
    uint32_t tb_dirty;           // TextBox: the byte whose bit tb_dirty_bit says "lay the text out again"
    uint8_t tb_dirty_bit;
    uint64_t clone_tree;         // (unused: the game's own pane-tree copy isn't used - copies are drawn again, below)
    uint64_t pane_draw;          // nn::ui2d::Pane::Draw(this, DrawInfo&, CommandBuffer&) - every pane type's (images, copies)
} GameUiGame;
void gameui_init(const GameUiGame *g);
// The screen's shape for the interface: wider than 16:9 by k (1: 16:9 or narrower). Set by each game's ultrawide code.
void gameui_set_screen(float k);
// (cmdbuf.c) what else the draws recorded now carry - a pane's image (id, gameui_image_path) and a copy's placement
// (f: x / y offset and scale in the layout's projected space -1..1, the pane's centre x / y there); changes number v
uint32_t gameui_draw_extra(uint32_t *img, int *copy, float f[5]);
const char *gameui_image_path(uint32_t id);  // (nvn/vulkan: the file an image id names; NULL none)
// (cmdbuf.c) the fitting of the layout being drawn on this thread (0 none - not a layout; 1 + GUI_MODE_*, high byte
// the box's anchor): recorded before the draws as CMD_UI, read by nvn/vulkan/exec.c draw_
uint32_t gameui_draw_mode(void);

#ifdef __cplusplus
}
#endif
