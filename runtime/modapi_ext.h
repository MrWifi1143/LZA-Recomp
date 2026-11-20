// The mod API's version 6 (runtime/modapi.h): what both games' hle/modapi.c hand over the same way - the overlay
// (ui/hud.mm), scripts' keyboard / mouse (hle/scriptinput.c), the text prompt (ui/menu.mm), sounds (hle/modsound.c),
// the save's id (hle/modapi_ext.c). LARECOMP_MODAPI_V6 goes in each game's LarecompModApi initializer.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void hud_submit(const void *cmds, int n, const char *text, int text_len);
void hud_size(float out[6]);
void hud_measure(const char *text, int len, float size, float wrap, float out[2]);
int hud_image(const char *path, int out_wh[2]);
int modapi_hud_to_screen(const float pos[3], float out[3]);
void scriptinput_grab(int on);
int scriptinput_events(void *out, int max);
void scriptinput_mouse(float out[3]);
int menu_text_prompt(const char *title, const char *text, int max_chars);
int menu_text_prompt_poll(char *out, int cap);
int modsound_load(const char *path);
int modsound_play(int id, float volume, float pitch, const float pos[3], float range, int loop);
void modsound_stop(int voice);
void modsound_set(int voice, float volume, const float pos[3]);
int modsound_playing(int voice);
int modapi_save_id(char *out, int cap);
uint32_t modapi_save_commits(void);
int modapi_save_path(char *out, int cap);
void modapi_save_committed(void);  // (services/fs.c: the game committed its save)

#define LARECOMP_MODAPI_V6                                                                                                 \
    .hud_submit = hud_submit, .hud_size = hud_size, .hud_measure = hud_measure, .hud_image = hud_image,                    \
    .hud_to_screen = modapi_hud_to_screen, .input_grab = scriptinput_grab, .input_events = scriptinput_events,             \
    .input_mouse = scriptinput_mouse, .text_prompt = menu_text_prompt, .text_prompt_poll = menu_text_prompt_poll,           \
    .sound_load = modsound_load, .sound_play = modsound_play, .sound_stop = modsound_stop, .sound_set = modsound_set,      \
    .sound_playing = modsound_playing, .save_id = modapi_save_id
// (version 8: in each game's initializer after LARECOMP_MODAPI_V6)
#define LARECOMP_MODAPI_V8 .save_commits = modapi_save_commits, .save_path = modapi_save_path
