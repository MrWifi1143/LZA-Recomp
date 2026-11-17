// lzaserver's accounts and save sync (lzaserver_accounts.c; protocol: runtime/acct_proto.h)
#pragma once
#include <stdbool.h>

typedef struct {
    int port;           // TCP (the same number as the UDP port)
    char bind[64];      // the address to listen on ("" all)
    char data[512];     // the server's folder: accounts, saves, its certificate
    char cert[512], key[512];  // the operator's own certificate + key (PEM); "" = our own in <data>/tls
    bool signup;        // new accounts from the game (false: only lzaserver --add-user)
    int save_max_mb;    // one save at most
    int versions;       // versions of a game's save kept per account
    int quota_mb;       // per account, every game's versions together
    int max_accounts;
} AccountsOptions;

int accounts_start(const AccountsOptions *o);  // 0: listening (its own threads)
int accounts_add_user(const AccountsOptions *o, const char *name);
// A play ticket (JOIN, acct_proto.h) by its id: its key and account name; false: unknown or expired (any thread)
#include <stdint.h>
#include <stddef.h>
bool accounts_ticket(const uint8_t id[8], uint8_t key[16], char *name, size_t n);
