// Speech bubbles over characters' heads (runtime/hle/bubble.c, the same in both games): a line of text in a white
// bubble with a tail, drawn over the game's image where a character stands - the townspeople's look - and following it.
// For scripts (the mod API's bubble: Lua ui.bubble, n:say) and for a multiplayer chat: a remote player's message over
// their stand-in. Any thread. Drawn by the runtime's overlay (ui/menu.mm), placed with the game's scene camera (each
// game's camera update: bubble_camera) - the games' own bubbles can't be put over just anything (2026-11-19: Z-A's are
// the townspeople's own C++ component, built from their NPC data; see docs/lua-api.md).
#pragma once
#include <stdint.h>

enum {
    BUBBLE_POS = 0,      // a spot in the world (pos)
    BUBBLE_PLAYER = 1,   // the player
    BUBBLE_NPC = 2,      // a script's character (the mod API's handle)
    BUBBLE_POKEMON = 3,  // a script's Pokémon (the mod API's handle; Arceus)
    BUBBLE_OBJECT = 4,   // a game object by its name (Z-A: "luapoke_1", "luanpc_001", a townsperson's)
    BUBBLE_FN = 5,       // where(ctx, feet) asked each frame (a multiplayer stand-in: its body's position)
};
enum { BUBBLE_OWNER_SCRIPT = 0, BUBBLE_OWNER_MP = 1 };  // (who made it: a script reload clears the scripts' only)

typedef struct {
    int kind;
    int handle;          // BUBBLE_NPC / BUBBLE_POKEMON
    char object[48];     // BUBBLE_OBJECT
    float pos[3];        // BUBBLE_POS: the spot (the bubble's tail points there)
    float height;        // metres over the target's feet (0: the kind's default - a person's head)
    int (*where)(void *ctx, float feet[3]);  // BUBBLE_FN: 1 there now, 0 not now (hidden), -1 gone (the bubble too)
    void *ctx;
    int owner;           // BUBBLE_OWNER_*
} BubbleTarget;

#define BUBBLE_MAX_CHARS 100  // (longer text is cut there, "..." at its end)
#define BUBBLE_MAX 32         // (at once; a new one past that takes the oldest's place)

// Shows text (UTF-8; "\n" breaks a line, long lines wrap) over the target for that long (seconds <= 0: by its length,
// 3 to 12 s). A bubble already over the same target is replaced (its id kept). Answers the bubble's id (> 0), 0 none.
int bubble_show(const BubbleTarget *t, const char *utf8, float seconds);
int bubble_hide(int id);                       // 1 hidden, 0 no such bubble
void bubble_hide_target(const BubbleTarget *t);
void bubble_clear(int owner);                  // every bubble of that owner (-1: all)
int bubble_count(void);

// ---- the runtime side
// The scene camera, from each game's camera update: eye -> the camera's eye (x, y, z), followed by its view's three
// columns (X.x Y.x Z.x 0, X.y Y.y Z.y 0, X.z Y.z Z.z 0); 0x24 / 0x20 bytes before it its vertical field of view
// (radians) and aspect - the same layout in both games' render cameras (LegendsVR camera.c).
void bubble_camera(const float *eye);
// Where a target stands (per game, hle/modapi.c): 1 there, 0 not now, -1 gone
int bubble_target_where(const BubbleTarget *t, float feet[3]);
// The bubbles to draw this frame, placed in a w x h image (the overlay: ui/menu.mm), farthest first
typedef struct {
    float x, y;    // the tail's tip, in pixels
    float alpha;   // 0..1 (fading in / out, far away)
    float dist;    // metres from the camera
    int spot;      // (over a spot, not a target: a sign - no tail, no line down; centred on the spot)
    char text[4 * BUBBLE_MAX_CHARS + 8];
} BubbleDraw;
int bubble_draw_list(BubbleDraw *out, int max, float w, float h);
