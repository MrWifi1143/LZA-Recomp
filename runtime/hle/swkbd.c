// Software keyboard (naming Link on a new file, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; our am service has no applets, so the game crashed. ShowKeyboard is replaced here: the overlay
// (runtime/ui/menu.mm menu_keyboard_run) asks for the text - typed directly with a keyboard, an on-screen keyboard
// with a controller - while this (game) thread keeps re-presenting the last frame.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

int menu_keyboard_run(const char *initial, int max_len, const char *title, char *out, int cap);  // runtime/ui/menu.mm

// UTF-16 (NUL-terminated, at most n units) -> UTF-8
static void utf16_to_8(const uint16_t *in, int n, char *out, size_t cap) {
    size_t k = 0;
    for (int i = 0; i < n && in[i] && k + 4 < cap; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[++i] - 0xDC00);
        if (cp < 0x80) out[k++] = (char)cp;
        else if (cp < 0x800) out[k++] = (char)(0xC0 | cp >> 6), out[k++] = (char)(0x80 | (cp & 0x3F));
        else if (cp < 0x10000) out[k++] = (char)(0xE0 | cp >> 12), out[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)), out[k++] = (char)(0x80 | (cp & 0x3F));
        else out[k++] = (char)(0xF0 | cp >> 18), out[k++] = (char)(0x80 | ((cp >> 12) & 0x3F)), out[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)),
             out[k++] = (char)(0x80 | (cp & 0x3F));
    }
    out[k] = 0;
}

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}
// ShowKeyboardArg (probed 2026-10-08, naming Link): +0x3ac text max length, +0x3b0 min length, +0x3c0 / +0x3c4
// initial text offset / length (UTF-16) in the work buffer at +0x4d0. The game reads the answer as UTF-8.
// Its KeyboardConfig (those offsets are libnx's SwkbdConfigCommon) has the texts the applet shows, UTF-16: +0x24 the
// header (64 + 1), +0xa6 the sub text (128 + 1), +0x1a8 the guide in the empty field (256 + 1): the overlay heads
// its keyboard with the first one set (a Link Code isn't a name).
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out), size = LD64(out + 8);
    int max_len = (int)*(const int32_t *)(uintptr_t)(arg + 0x3ac);
    uint32_t off = *(const uint32_t *)(uintptr_t)(arg + 0x3c0), n = *(const uint32_t *)(uintptr_t)(arg + 0x3c4);
    uint64_t wb = LD64(arg + 0x4d0);
    char initial[96] = "";
    for (uint32_t i = 0; wb && i < n && i < sizeof initial - 1; i++) {
        uint16_t ch = *(const uint16_t *)(uintptr_t)(wb + off + 2 * i);
        initial[i] = ch < 0x80 ? (char)ch : '?';
    }
    char header[256], sub[512], guide[1024], text[96];
    utf16_to_8((const uint16_t *)(uintptr_t)(arg + 0x24), 65, header, sizeof header);
    utf16_to_8((const uint16_t *)(uintptr_t)(arg + 0xa6), 129, sub, sizeof sub);
    utf16_to_8((const uint16_t *)(uintptr_t)(arg + 0x1a8), 257, guide, sizeof guide);
    int ok = menu_keyboard_run(initial, max_len, header[0] ? header : guide[0] ? guide : sub, text, sizeof text);
    if (!ok) snprintf(text, sizeof text, "%s", initial);  // cancelled: keep what was there
    LOG("[swkbd] text entry \"%s\" / \"%s\" / \"%s\" (max %d, initial \"%s\"): %s \"%s\"\n", header, sub, guide, max_len, initial,
        ok ? "entered" : "cancelled", text);
    if (ptr && size) {
        memset((void *)(uintptr_t)ptr, 0, size);
        if (getenv("LARECOMP_SWKBD_UTF8")) {  // (Link's Awakening read its answer as UTF-8)
            snprintf((char *)(uintptr_t)ptr, size, "%s", text);
        } else {
            // Let's Go reads UTF-16: "RRS" written as UTF-8 came back as U+5252 'S' ("?S" on screen). The text is
            // UTF-8 from the overlay: decode it and store UTF-16LE, NUL-terminated.
            uint16_t *o = (uint16_t *)(uintptr_t)ptr;
            uint64_t cap = size / 2, k = 0;
            for (const unsigned char *s = (const unsigned char *)text; *s && k + 1 < cap;) {
                uint32_t cp = *s++;
                int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
                if (more) cp &= 0x3F >> more;
                while (more-- && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
                if (cp >= 0x10000) {
                    if (k + 2 >= cap) break;
                    cp -= 0x10000;
                    o[k++] = (uint16_t)(0xD800 | (cp >> 10));
                    o[k++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
                } else {
                    o[k++] = (uint16_t)cp;
                }
            }
            o[k] = 0;
        }
    }
    c->x[0] = 0;
}
