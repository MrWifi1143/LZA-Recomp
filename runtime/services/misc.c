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

// ------------------------------------------------------------------ aoc:u
// Add-on content: Z-A's Mega Dimension is add-on 2 of base id 0100F43008C45000, owned when the game folder has it
// (services/fs.c fs_aoc_present: dlc/ or dlc.bin beside romfs) - then the game shows no "downloadable content
// available" (its eShop link) and mounts it (fsp-srv OpenDataStorageByDataId).
#define ZA_AOC_BASE_ID 0x0100F43008C45000ull
#define ZA_AOC_INDEX 2u
SERVICE("aoc:u", aoc_u) {
    extern bool fs_aoc_present(void);
    static uint32_t seen[8];
    if (req->cmd < 256 && !(seen[req->cmd >> 5] & (1u << (req->cmd & 31))))
        seen[req->cmd >> 5] |= 1u << (req->cmd & 31), LOG("[aoc] cmd %u (add-on %s)\n", req->cmd, fs_aoc_present() ? "present" : "absent");
    switch (req->cmd) {
    case 0: case 2: OUT(uint32_t, 0) = fs_aoc_present() ? 1 : 0; resp_size(resp, 4); break;  // CountAddOnContent(ByApplicationId)
    case 1: case 3: {  // ListAddOnContent(ByApplicationId)(offset, count) -> count, B buffer of u32 indices
        uint32_t off = IN(uint32_t, 0), max = IN(uint32_t, 4), n = 0;
        IpcBuf *b = out_buf(req, 0);
        if (fs_aoc_present() && off == 0 && max > 0 && b && b->size >= 4) *(uint32_t *)(uintptr_t)b->addr = ZA_AOC_INDEX, n = 1;
        OUT(uint32_t, 0) = n;
        resp_size(resp, 4);
        break;
    }
    case 4: case 5: OUT(uint64_t, 0) = ZA_AOC_BASE_ID; resp_size(resp, 8); break;  // GetAddOnContentBaseId(ByApplicationId)
    case 8: case 10: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;  // GetAddOnContentListChangedEvent
    default: ok_logger(s, req, resp);  // (PrepareAddOnContent, NotifyMount..., CheckAddOnContentMountStatus: ok)
    }
}
