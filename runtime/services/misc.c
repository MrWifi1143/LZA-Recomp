// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[svc] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[pctl] IParentalControlService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ aoc:u (no DLC)
SERVICE("aoc:u", aoc_u) {
    (void)s;
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;   // CountAddOnContent
    case 3: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;   // ListAddOnContent
    case 5: OUT(uint64_t, 0) = 0x01006BB00C6F1000ull; resp_size(resp, 8); break;  // GetAddOnContentBaseId
    case 8: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    default: ok_logger(s, req, resp);
    }
}

// ------------------------------------------------------------------ nn::ngc::ProfanityFilter (in the game)
// Arceus sets up the SDK's own filter when a Link Trade connects: Initialize mounts the console's word list (system
// data 0100000000000823, NgWord2) and aborts without it (2002-1001) - both copies died the moment they met. The game
// then asks for the list's version (GetContentVersion: a file of it - abort 2002-6905). Every public call is answered
// here without the list: set up, version 1, no word matched, nothing masked.
// (hooked at the SDK's functions themselves - ngc_configure, from main.c: not every call goes through the game's
// imports, GetContentVersion's doesn't; a hook returns 1: the SDK's own code is skipped)
static int ngc_ok(Cpu *c, uint64_t a) { (void)a; c->x[0] = 0; return 1; }       // Initialize / Finalize / SetMaskMode
static int ngc_version(Cpu *c, uint64_t a) { (void)a; c->x[0] = 1; return 1; }  // GetContentVersion
// CheckProfanityWords(this, u32 *out, u32 patterns, const T **words, size_t count): out[i] = 0 (no pattern hit)
static int ngc_check(Cpu *c, uint64_t a) {
    (void)a;
    uint64_t out = c->x[1], n = c->x[4];
    if (out && n < 4096) memset((void *)(uintptr_t)out, 0, n * 4);
    c->x[0] = 0;
    return 1;
}
// MaskProfanityWordsInText(this, int *count, T *text, u32 patterns): the text stays, 0 words masked
static int ngc_mask(Cpu *c, uint64_t a) {
    (void)a;
    if (c->x[1]) *(int32_t *)(uintptr_t)c->x[1] = 0;
    c->x[0] = 0;
    return 1;
}
uint64_t loader_find_symbol(const char *name);
void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void ngc_configure(void) {
    static const struct { const char *name; int (*fn)(Cpu *, uint64_t); } k[] = {
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", ngc_ok},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", ngc_ok},
        {"_ZN2nn3ngc15ProfanityFilter8FinalizeEv", ngc_ok},
        {"_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", ngc_ok},
        {"_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", ngc_version},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", ngc_check},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", ngc_check},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm", ngc_check},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", ngc_mask},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", ngc_mask},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", ngc_mask},
    };
    int n = 0;
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        uint64_t addr = loader_find_symbol(k[i].name);
        if (addr) rt_register_fnhook_at(addr, k[i].fn), n++;
    }
    LOG("[ngc] profanity filter: %d functions answered without the word list\n", n);
}
// ------------------------------------------------------------------ ngc:u (the profanity filter)
// The names a trade brings (the Pokemon's nickname, its trainer's) are checked here: nothing is filtered. (Z-A: the
// stub's empty answer left the game re-checking for good, on a white screen after the trade.)
SERVICE("ngc:u", ngc_u) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;  // GetContentVersion
    case 1: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // Check: no word matched
    case 2: {                                                  // Mask: the text as it is, nothing masked
        IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
        if (in && out && in->addr && out->addr)
            memcpy((void *)(uintptr_t)out->addr, (const void *)(uintptr_t)in->addr, in->size < out->size ? in->size : out->size);
        OUT(uint32_t, 0) = 0;
        resp_size(resp, 4);
        break;
    }
    default: LOG("[ngc] cmd %u (ok)\n", req->cmd);
    }
}
