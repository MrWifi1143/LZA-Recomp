// The mod API: what the game itself offers scripts (Legends Lua's npc / player / ui / events - contrib/legendslua), the
// same in both games. The runtime implements it per game (runtime/hle/modapi.c) and hands it to a plugin that exports
//     void larecomp_plugin_modapi(const LarecompModApi *api);
// right after that plugin's larecomp_plugin_init (runtime/plugin.c). Nothing here runs until a plugin calls enable():
// a game with no scripts pays nothing.
//
// Threads: every function may be called from any thread. What needs the game's own thread (making a character, moving
// one, the player put somewhere) is queued and done there within a frame or two.
// Positions are the game's world coordinates (metres, y up); yaw is in degrees, 0 facing +z, 90 facing +x.
#pragma once
#include <stdint.h>

#define LARECOMP_MODAPI_VERSION 7  // (2: npc_outfit, npc_anim, pokemon_*, game_work; 3: game_script, item_give, npc_object;
                                   //  4: npc_talk, npc_talked; 5: bubble, bubble_hide; 6: hud_*, input_*,
                                   //  text_prompt*, sound_*, save_id; 7: ui_* - the game's interface)

// npc_state answers
enum {
    MODNPC_NONE = 0,     // no such handle (removed, or never made)
    MODNPC_WAITING = 1,  // not in the world: its area isn't loaded or the player is far (made again once both are right)
    MODNPC_COMING = 2,   // asked for: the game is making it
    MODNPC_UP = 3,       // in the world, standing
    MODNPC_WALKING = 4,  // in the world, on its way somewhere (npc_walk_to)
};

typedef struct {
    uint32_t version;  // LARECOMP_MODAPI_VERSION
    const char *game;  // "za" (Legends: Z-A) or "pla" (Legends: Arceus)
    // The hooks the API needs switched on (the game's characters and the player followed). Call once, before the game's
    // first frame if possible (the level's own people are noted as it loads - what new ones are made like).
    void (*enable)(void);

    // ---- characters of a script's own, made by the game
    // A new one wanted at pos, facing yaw. model: what it looks like - Z-A: part of an NPC data name the level has
    // ("npc_A04_z02_02_0002", "A04"; NULL / "" the nearest townsperson's); Arceus: a character id ("tr0036_00_senpai_m"),
    // a hash ("0x2d992cf818c93910") or a placement name of the level ("area00_npc_g_002": that one's look). label: the
    // script's name for it (logs). Answers its handle (> 0), 0 none free. It is made when its spot is near the player
    // and its area is loaded, and made again after a trip away (MODNPC_WAITING meanwhile).
    int (*npc_spawn)(const char *model, const float pos[3], float yaw, const char *label);
    void (*npc_remove)(int h);                                 // gone for good (the game lets it go)
    void (*npc_remove_all)(void);                              // every one (a script reload)
    void (*npc_walk_to)(int h, const float pos[3], int run);   // walks (run: runs) there, on the ground
    void (*npc_stop)(int h);                                   // stands where it is
    void (*npc_set_pos)(int h, const float pos[3], float yaw); // put there at once
    // state (MODNPC_*); pos / yaw (may be NULL): where it is now (or where it will be made)
    int (*npc_state)(int h, float pos[3], float *yaw);
    // the looks one can be made with now (one per line, "name - what"), into out; answers the length
    int (*npc_models)(char *out, int cap);

    // ---- the player
    int (*player_pos)(float pos[3], float *yaw);  // 0: not known yet (no level)
    void (*player_teleport)(const float pos[3]);

    // ---- the world
    // A number that changes when an area (Arceus: a level's people; Z-A: a part of the city, a building) has loaded,
    // and its name when known ("" not): scripts re-check what they made then.
    uint64_t (*area)(char *name, int cap);
    uint64_t (*buttons)(void);  // the Switch buttons held (bit = A B X Y LSTICK RSTICK L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN)
    void (*notify)(const char *text, unsigned ms);  // a notice on screen
    // the folders (relative to the game folder) of the mods that are on and carry scripts: mods/<name>/lua
    int (*mod_lua_dirs)(char (*dirs)[256], int max);
    double (*now)(void);  // seconds, a steady clock

    // ---- version 2
    // What a character wears, given before it is made (0: not taken): "player" - the player's own clothes (Z-A: the
    // parts the player character's model was made with). Arceus also "<slot>=<item>,..." over the player's (items of dressup_table_p1/p2:
    // "0=p1_hrs0001_00_default_blond") and "body=p1" / "body=p2". Arceus: a dressed character walks as a player does
    // (its own steps, steering each frame), the most reliable walk there.
    int (*npc_outfit)(int h, const char *spec);
    // One of its animation's parameters set (the game's own names: Z-A "sit_bool", "sit_int", "once_action_trigger";
    // Arceus "set_squat_bool", "move_front"...): kind 'f' float, 'i' int, 'b' bool, 't' trigger (value ignored). Done on
    // the game's thread within a frame; 0: no such character.
    int (*npc_anim)(int h, const char *name, int kind, float value);
    // A Pokémon of a script's own (0: not in this game / none free): species number, form, shiny. Made like the
    // characters, near the player in a loaded area; pokemon_state answers as npc_state.
    int (*pokemon_spawn)(int species, const float pos[3], float yaw, int form, int shiny, const char *label);
    void (*pokemon_remove)(int h);
    int (*pokemon_state)(int h, float pos[3], float *yaw);
    // A story work (a number the game keeps by name: Z-A "SYS_WORK_PHASE", Arceus "WPHA_STORY"...): read (set 0) or
    // written (set 1). Answers 1 when the game has it.
    int (*game_work)(const char *name, int set, int64_t value, int64_t *out);

    // ---- version 3
    // Lua text run in the game's own Lua (Z-A: its event framework, Lua 5.4 - hle/za_script.c) at the game's next
    // frame: a job number (0: this game has none, or too many waiting). script_result: 0 still waiting, 1 ran (out: what
    // it returned, as text), 2 failed (out: the error), -1 no such job (its result taken already).
    int (*game_script)(const char *code);
    int (*script_result)(int id, char *out, int cap);
    // Items into the bag by the game's own add: item number, how many. 1 done (Z-A: queued for the next frame), 0 not.
    int (*item_give)(int item, int count);
    // The name the game knows a script's character by (its object: Z-A "luanpc_001"), for game_script; 0 none yet.
    int (*npc_object)(int h, char *out, int cap);
    // The time of day: the hour (0-24) the game's clock is set to at its next frame. 1 asked, 0 not in this game.
    int (*time_set)(float hour);
    // A message window's own text: reserved line `slot` (0-15) of every message file - label "lza_say_<slot>" (Z-A,
    // hle/za_script.c) - is this UTF-8 text. 1 set, 0 not in this game.
    int (*message_text)(int slot, const char *text);
    // An animation parameter of an object the game made by its name (Z-A: pokemon.spawn's "luapoke_<n>"), as npc_anim:
    // set at its next update. 1 asked, 0 not in this game.
    int (*object_anim)(const char *object, const char *param, int kind, float value);
    // (version 3 also: npc_walk_to's run | 2 - Z-A: the game's own navigation walks it, game_script orders the walk)

    // ---- version 4
    // The game's own Talk prompt over a script's character (Arceus: "A <its name>", as over the game's people): on 1
    // shows it while the player faces it, 0 not. h 0 asks whether this game has it (1 yes, 0 no - Z-A's townspeople have
    // no prompt). npc_talked: a character the player talked to through that prompt since the last call (0 none).
    int (*npc_talk)(int h, int on);
    int (*npc_talked)(void);

    // ---- version 5
    // A speech bubble over a target's head (runtime/bubble.h): kind 0 a spot (pos), 1 the player, 2 a script's character
    // (handle), 3 a script's Pokémon (handle, Arceus), 4 a game object by its name (object, Z-A: "luapoke_1"...); height
    // metres over its feet (0: a head's); text UTF-8, cut at 100 characters; seconds (0: by its length). Answers its id
    // (0 none); a bubble already over that target is replaced. bubble_hide(id) fades it out; id 0: every script's.
    int (*bubble)(int kind, int handle, const char *object, const float pos[3], float height, const char *text, float seconds);
    int (*bubble_hide)(int id);

    // ---- version 6 (lua-za / lua-pla 2026-11-20: scripts' overlay, keyboard / mouse, sounds, storage - runtime/hud.h,
    // runtime/modapi_ext.h; the same in both games)
    // The overlay: this frame's draw list (HudCmd x n, their text) replaces the last; its size (w, h, the game's 16:9
    // interface box x, y, w, h); a text's size; an image file's id (0 not read; out its w, h); a point of the world on
    // the overlay (1 in front of the camera: out x, y, metres)
    void (*hud_submit)(const void *cmds, int n, const char *text, int text_len);
    void (*hud_size)(float out[6]);
    void (*hud_measure)(const char *text, int len, float size, float wrap, float out[2]);
    int (*hud_image)(const char *path, int out_wh[2]);
    int (*hud_to_screen)(const float pos[3], float out[3]);
    // Keyboard and mouse: a script's menu holds the game's input (1) or lets go (0); events since the last call
    // (HudInputEvent); the pointer (x, y in overlay pixels, buttons: 1 left 2 right 4 middle)
    void (*input_grab)(int on);
    int (*input_events)(void *out, int max);
    void (*input_mouse)(float out[3]);
    // Text typed in the runtime's dialog (a keyboard, or the on-screen one with a controller): opened (1; 0 one is up);
    // poll: 0 typing, 1 confirmed (out), -1 cancelled, -2 none open
    int (*text_prompt)(const char *title, const char *text, int max_chars);
    int (*text_prompt_poll)(char *out, int cap);
    // A mod's sounds (.wav / .ogg): load -> id; play (pos NULL: 2D; range metres) -> voice; stop (0: all); set volume /
    // position (volume < 0: kept); playing
    int (*sound_load)(const char *path);
    int (*sound_play)(int id, float volume, float pitch, const float pos[3], float range, int loop);
    void (*sound_stop)(int voice);
    void (*sound_set)(int voice, float volume, const float pos[3]);
    int (*sound_playing)(int voice);
    // The save the game has (scripts' storage per save): its folder's name ("save_0"), "" none
    int (*save_id)(char *out, int cap);

    // ---- version 7 (ui-za / ui-pla 2026-11-19, numbered 6 on those branches; the same in both games)
    // The game's own interface layouts (runtime/gameui.h, hle/gameui.c): a pane of a layout edited, one key at a time -
    // the keys of a mod's ui/<layout>.ini (x y sx sy scale w h alpha hide show anchor mode fill follow text image); pane
    // "" the layout itself. 1 taken, 0 not (an unknown key, the table full). A script's edits beat a mod's data and the
    // runtime's own; ui_unset drops a script's edits of that pane (pane NULL: of the layout; layout NULL: all of them).
    int (*ui_set)(const char *layout, const char *pane, const char *key, const char *value);
    void (*ui_unset)(const char *layout, const char *pane);
    // What is on screen: the layouts ("<name> <panes> <w>x<h>" a line), a layout's panes ("<depth> <name> <type> <x> <y>
    // <w> <h> <shown>" a line, tree order), a TextBox's text (UTF-8). Answer the length, -1: no such layout on screen.
    int (*ui_layouts)(char *out, int cap);
    int (*ui_panes)(const char *layout, char *out, int cap);
    int (*ui_text)(const char *layout, const char *pane, char *out, int cap);
    // A new pane, a copy of src (with what is under it) named name; 1 asked, 0 not (gameui_clone)
    int (*ui_clone)(const char *layout, const char *src, const char *name);
} LarecompModApi;

// ---- runtime side
const LarecompModApi *modapi_get(void);  // (runtime/hle/modapi.c)
