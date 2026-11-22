// Online server accounts and save sync (runtime/acct.c; protocol runtime/acct_proto.h; server contrib/lzaserver).
// Playing needs no account; playing ON an lzaserver does (Link Play by code, the shared world). Signed in, the save
// here is linked to the account and synced (start / saves / quit; offline in between is fine).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ACCT_OP_NONE, ACCT_OP_LOGIN, ACCT_OP_CREATE, ACCT_OP_LOGOUT, ACCT_OP_FORGET_KEY, ACCT_OP_SYNC };

// The account signed in on the current server (g_settings.online_server): its name, or false
bool acct_signed_in(char *name, size_t n);
// The launcher's buttons: run in the background; acct_ui_busy() while it runs, then acct_ui_message() says how it went.
// LOGIN / CREATE link the save here to the account right after (a sync); SYNC syncs now.
void acct_ui_start(int op, const char *name, const char *password);
void acct_set_game_root(const char *root);  // (the game files: its program id, for the sync from the launcher)
bool acct_session_ended(void);              // the server ended this copy's sign-in (it was forgotten here: sign in again)
bool acct_ui_busy(void);
const char *acct_ui_message(bool *error);  // ("" none)
bool acct_ui_key_changed(void);            // the last try failed because the server's key changed (offer "Forget")
const char *acct_last_sync(void);          // the last save sync on this PC, for the launcher ("" none)

// The game: at boot (the program id known, before the game runs), at every commit of the save, at exit
void acct_boot_sync(uint64_t program_id);
void acct_save_committed(const char *fs_root);
void acct_exit(void);  // (from the exit handler, before the process ends: nvn/vulkan/device.c quick_exit_handler)

// lasrecomp --account <create | login | logout | status | sync | forget-key> [--server S] [--user NAME]
//   [--password-file F] [--program-id HEX]: the same steps without a window (tests, scripts); 0 ok
int acct_cli(int argc, char **argv);

#ifdef __cplusplus
}
#endif
