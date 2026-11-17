// Online server accounts and save sync: the protocol between the game (runtime/acct.c, PC and PS5) and the server
// (contrib/lzaserver/lzaserver_accounts.c). Shared by both; plain C, no runtime headers.
//
// TCP over TLS 1.3, on the server's port number (the same number as its UDP Link Play port: 39200 by default). The
// server's certificate is its own (made at its first start, self-signed) or the operator's (from a certificate
// authority). The game pins it on first use (the SHA-256 of its public key, per server address): a different key later
// is refused unless a certificate authority vouches for it (runtime/acct.c).
//
// Every message, both ways: u32 header length, u32 body length (little-endian), the header (text, one line, fields
// separated by single spaces), the body (bytes; empty for most). Requests may follow each other on one connection.
//   CREATE <name> <password hex> <device>        -> OK <token>                  (a new account, signed in)
//   LOGIN <name> <password hex> <device>         -> OK <token>
//   LOGOUT <token>                               -> OK                          (that session ends on the server)
//   WHOAMI <token>                               -> OK <name>
//   INFO <token> <game>                          -> OK <save>  |  OK 0          (the latest save of that game, or none)
//   GET <token> <game> <version>                 -> OK <save>, body = the save's blob
//   PUT <token> <game> <base> <counter> <saved> <device> <content hash> <blob sha256>, body = blob
//                                                -> OK <save>  |  CONFLICT <save>   (base: the version this upload
//                                                   replaces - not the latest: refused, nothing changes)
//   LIST <token> <game>                          -> OK <n>, body = n lines "<save>" (newest first: the versions kept)
// <save> = <version> <counter> <saved> <device> <content hash> <blob sha256> <size> <uploaded>
// Errors: ERR <code> <text...>; codes: bad, wrong (name or password), taken, closed (no new accounts), wait (too many
// tries: <text> starts with the seconds), signin (the token isn't valid: sign in again), big, quota, full, corrupt, server.
// <name>: 3-24 of a-z 0-9 _ . -; password: 8-256 bytes; <device>: 1-24 of A-Z a-z 0-9 _ -; <game>: the program id,
// 16 lowercase hex digits; <saved>, <uploaded>: unix seconds (the save's own time on the device that made it /
// when the server got it); <counter>: the uploading device's own count of its uploads; hashes: 64 lowercase hex digits.
// <token>: "<name>:<64 hex>" - the server keeps only its SHA-256.
#pragma once

#define ACCT_PROTO_VERSION 1
#define ACCT_DEFAULT_PORT 39200
#define ACCT_MAX_HEADER 2048
#define ACCT_NAME_MIN 3
#define ACCT_NAME_MAX 24
#define ACCT_PASS_MIN 8
#define ACCT_PASS_MAX 256
#define ACCT_DEVICE_MAX 24
#define ACCT_TOKEN_MAX (ACCT_NAME_MAX + 1 + 64)
#define ACCT_BLOB_MAGIC "LZSV"  // (a save blob: runtime/acct.c pack_save)
