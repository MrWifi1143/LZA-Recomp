// lzaserver accounts and save sync (protocol: runtime/acct_proto.h). Players make an account on this server - only on
// this server - to play here (Link Play by Link Code, the shared world: a play ticket, JOIN, proves it in the UDP hello -
// lzaserver.c; --open-play / --no-accounts: anyone) and to have it keep their save, so another copy of the game
// (another PC, the PS5 port) signed in to the same account picks it up. The game itself needs no account.
//
// TCP + TLS 1.3 (Mbed TLS, third_party/mbedtls) on the same port number as the UDP Link Play port. Passwords: only a
// salted Argon2id hash is kept (Monocypher, third_party/monocypher; 19 MiB, 2 passes - OWASP's minimum; the parameters
// are stored with each hash, so they can be raised later). Sessions: random tokens; the server keeps their SHA-256.
// Tries are limited per address and per account. Saves: the latest of each game plus the versions before it (10 by
// default), each at most --save-max-mb; an upload that isn't based on the latest version is refused (the game keeps
// both copies, never overwrites one).
//
// Data (--data, default ./lzaserver-data): tls/key.pem + tls/cert.pem (made at the first start unless --cert / --key),
// users/<name>/account, users/<name>/sessions, users/<name>/saves/<game>/head + v<N>.bin + v<N>.meta.
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <io.h>
typedef SOCKET sock_t;
#define NO_SOCK INVALID_SOCKET
#define closesock closesocket
#define mkdir_(p) _mkdir(p)
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define NO_SOCK (-1)
#define closesock close
#define mkdir_(p) mkdir(p, 0700)
#endif

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "monocypher/monocypher.h"

#include "acct_proto.h"
#include "lzaserver_accounts.h"

#define MAX_CONNS 32
#define MAX_CONNS_PER_IP 4
#define IO_TIMEOUT_S 20
#define MAX_REQUESTS_PER_CONN 64
#define MAX_SESSIONS 10
#define SESSION_DAYS 180
#define MAX_GAMES 8
#define ARGON_BLOCKS 19456  // (KiB: 19 MiB)
#define ARGON_PASSES 2
#define ARGON_CONCURRENT 2  // (hashes at once: memory bound)
// tries: login failures per address / per account in 10 minutes, new accounts per address per hour
#define FAIL_WINDOW_S 600
#define FAIL_PER_IP 8
#define FAIL_PER_USER 5
#define CREATE_WINDOW_S 3600
#define CREATE_PER_IP 3

static AccountsOptions g_o;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_ssl_config g_conf;
static mbedtls_x509_crt g_cert;
static mbedtls_pk_context g_key;
static pthread_mutex_t g_store = PTHREAD_MUTEX_INITIALIZER;  // (the account files)
static pthread_mutex_t g_lim = PTHREAD_MUTEX_INITIALIZER;    // (tries, connections)
static pthread_mutex_t g_argon_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_argon_c = PTHREAD_COND_INITIALIZER;
static int g_argon_busy;
static int g_conns;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char t[32];
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(t, sizeof t, "%H:%M:%S", &tmv);
    va_list ap;
    va_start(ap, fmt);
    printf("  [accounts %s] ", t);
    vprintf(fmt, ap);
    va_end(ap);
}

// ---- small helpers
static void hex(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = 0;
}
static int unhex(const char *s, uint8_t *out, size_t max) {  // (-1: not hex / too long)
    size_t n = strlen(s);
    if (n % 2 || n / 2 > max) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1 || !strchr("0123456789abcdefABCDEF", s[2 * i]) || !strchr("0123456789abcdefABCDEF", s[2 * i + 1])) return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2);
}
static bool is_hex(const char *s, size_t n) {
    if (strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (!strchr("0123456789abcdef", s[i])) return false;
    return true;
}
static bool valid_name(const char *s) {
    size_t n = strlen(s);
    if (n < ACCT_NAME_MIN || n > ACCT_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (!strchr("abcdefghijklmnopqrstuvwxyz0123456789_.-", s[i])) return false;
    return s[0] != '.';  // (".", "..": not a folder name)
}
static bool valid_device(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > ACCT_DEVICE_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-", s[i])) return false;
    return true;
}
static void sha256(const void *p, size_t n, uint8_t out[32]) { mbedtls_sha256((const unsigned char *)p, n, out, 0); }
static void rnd(void *p, size_t n) { mbedtls_ctr_drbg_random(&g_drbg, (unsigned char *)p, n); }
static int64_t now_s(void) { return (int64_t)time(NULL); }

static bool write_file_atomic(const char *path, const void *data, size_t n) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
#ifndef _WIN32
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
#endif
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    if (ok) remove(path);
#endif
    if (!ok || rename(tmp, path)) { remove(tmp); return false; }
    return true;
}
static char *read_file(const char *path, size_t *n, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz > max) { fclose(f); return NULL; }
    char *b = malloc((size_t)sz + 1);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    if (b) b[sz] = 0, *n = (size_t)sz;
    return b;
}
static void user_path(char *out, size_t sz, const char *name, const char *rest) {
    snprintf(out, sz, "%s/users/%s%s%s", g_o.data, name, rest[0] ? "/" : "", rest);
}

// ---- tries (per address, per account)
typedef struct { char key[64]; int64_t t[16]; int n; int64_t last; } Tries;
#define NTRIES 2048
static Tries g_tries[NTRIES];
static Tries *tries_of(const char *key) {  // (under g_lim; the oldest entry recycled)
    Tries *old = &g_tries[0];
    for (int i = 0; i < NTRIES; i++) {
        if (!strcmp(g_tries[i].key, key)) return &g_tries[i];
        if (g_tries[i].last < old->last) old = &g_tries[i];
    }
    memset(old, 0, sizeof *old);
    snprintf(old->key, sizeof old->key, "%s", key);
    return old;
}
static int tries_count(Tries *t, int64_t window) {  // (drops the ones outside the window)
    int64_t now = now_s();
    int k = 0;
    for (int i = 0; i < t->n; i++)
        if (now - t->t[i] < window) t->t[k++] = t->t[i];
    t->n = k;
    return k;
}
static void tries_add(Tries *t) {
    if (t->n == 16) memmove(t->t, t->t + 1, 15 * sizeof t->t[0]), t->n = 15;
    t->t[t->n++] = t->last = now_s();
}
static int wait_left(Tries *t, int64_t window) { return t->n ? (int)(t->t[0] + window - now_s()) + 1 : 0; }  // (seconds)
// 0 or the seconds to wait before another try
static int login_blocked(const char *ip, const char *name) {
    char k[64];
    pthread_mutex_lock(&g_lim);
    snprintf(k, sizeof k, "f:%s", ip);
    Tries *a = tries_of(k);
    int w = tries_count(a, FAIL_WINDOW_S) >= FAIL_PER_IP ? wait_left(a, FAIL_WINDOW_S) : 0;
    snprintf(k, sizeof k, "u:%s", name);
    Tries *u = tries_of(k);
    if (!w && tries_count(u, FAIL_WINDOW_S) >= FAIL_PER_USER) w = wait_left(u, FAIL_WINDOW_S);
    pthread_mutex_unlock(&g_lim);
    return w;
}
static void login_failed(const char *ip, const char *name) {
    char k[64];
    pthread_mutex_lock(&g_lim);
    snprintf(k, sizeof k, "f:%s", ip);
    tries_add(tries_of(k));
    snprintf(k, sizeof k, "u:%s", name);
    tries_add(tries_of(k));
    pthread_mutex_unlock(&g_lim);
}
static int create_blocked(const char *ip, bool count) {
    char k[64];
    snprintf(k, sizeof k, "c:%s", ip);
    pthread_mutex_lock(&g_lim);
    Tries *c = tries_of(k);
    int w = tries_count(c, CREATE_WINDOW_S) >= CREATE_PER_IP ? wait_left(c, CREATE_WINDOW_S) : 0;
    if (!w && count) tries_add(c);
    pthread_mutex_unlock(&g_lim);
    return w;
}

// ---- passwords
static void argon2id(const uint8_t *pass, size_t n, const uint8_t salt[16], uint32_t blocks, uint32_t passes, uint8_t out[32]) {
    pthread_mutex_lock(&g_argon_m);
    while (g_argon_busy >= ARGON_CONCURRENT) pthread_cond_wait(&g_argon_c, &g_argon_m);
    g_argon_busy++;
    pthread_mutex_unlock(&g_argon_m);
    void *work = malloc((size_t)blocks * 1024);
    if (work) {
        crypto_argon2_config c = {CRYPTO_ARGON2_ID, blocks, passes, 1};
        crypto_argon2_inputs in = {pass, salt, (uint32_t)n, 16};
        crypto_argon2(out, 32, work, c, in, crypto_argon2_no_extras);
        crypto_wipe(work, (size_t)blocks * 1024);
        free(work);
    } else rnd(out, 32);  // (no memory: a hash nothing matches)
    pthread_mutex_lock(&g_argon_m);
    g_argon_busy--;
    pthread_cond_signal(&g_argon_c);
    pthread_mutex_unlock(&g_argon_m);
}

typedef struct { uint32_t blocks, passes; uint8_t salt[16], hash[32]; int64_t created; } Account;
static bool account_load(const char *name, Account *a) {  // (under g_store)
    char p[512], salt[64], h[96];
    user_path(p, sizeof p, name, "account");
    size_t n;
    char *s = read_file(p, &n, 4096);
    if (!s) return false;
    long long created = 0;
    bool ok = sscanf(s, "v1 argon2id m=%u t=%u p=1 %40s %70s %lld", &a->blocks, &a->passes, salt, h, &created) == 5 &&
              unhex(salt, a->salt, 16) == 16 && unhex(h, a->hash, 32) == 32 && a->blocks >= 8 && a->blocks <= (1u << 22) &&
              a->passes >= 1 && a->passes <= 16;
    a->created = created;
    free(s);
    return ok;
}
static bool account_save(const char *name, const Account *a) {
    char p[512], salt[40], h[70], line[256];
    hex(a->salt, 16, salt), hex(a->hash, 32, h);
    int n = snprintf(line, sizeof line, "v1 argon2id m=%u t=%u p=1 %s %s %lld\n", a->blocks, a->passes, salt, h, (long long)a->created);
    user_path(p, sizeof p, name, "account");
    return write_file_atomic(p, line, (size_t)n);
}
static int count_accounts(void) {
    char p[512];
    snprintf(p, sizeof p, "%s/users", g_o.data);
    int n = 0;
#ifdef _WIN32
    char pat[520];
    snprintf(pat, sizeof pat, "%s/*", p);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h != -1) {
        do n += fd.name[0] != '.';
        while (!_findnext(h, &fd));
        _findclose(h);
    }
#else
    DIR *d = opendir(p);
    if (d) {
        for (struct dirent *e; (e = readdir(d));) n += e->d_name[0] != '.';
        closedir(d);
    }
#endif
    return n;
}

// ---- sessions: "<sha256(secret)> <created> <last used> <device>" per line
static bool session_new(const char *name, const char *device, char *token) {  // (under g_store)
    uint8_t sec[32], h[32];
    char sh[70], p[512];
    rnd(sec, sizeof sec);
    hex(sec, 32, sh);
    snprintf(token, ACCT_TOKEN_MAX + 1, "%s:%s", name, sh);
    sha256(sh, 64, h);
    hex(h, 32, sh);
    crypto_wipe(sec, sizeof sec);
    user_path(p, sizeof p, name, "sessions");
    size_t n = 0;
    char *old = read_file(p, &n, 64 * 1024);
    // keep the newest MAX_SESSIONS - 1 others, drop the expired
    char *lines[64];
    int nl = 0;
    for (char *l = old ? strtok(old, "\n") : NULL; l && nl < 64; l = strtok(NULL, "\n")) {
        long long c, u;
        if (sscanf(l, "%*64s %lld %lld", &c, &u) == 2 && now_s() - u < (int64_t)SESSION_DAYS * 86400) lines[nl++] = l;
    }
    char out[16 * 1024];
    int k = 0;
    for (int i = nl > MAX_SESSIONS - 1 ? nl - (MAX_SESSIONS - 1) : 0; i < nl; i++) k += snprintf(out + k, sizeof out - (size_t)k, "%s\n", lines[i]);
    k += snprintf(out + k, sizeof out - (size_t)k, "%s %lld %lld %s\n", sh, (long long)now_s(), (long long)now_s(), device);
    free(old);
    return write_file_atomic(p, out, (size_t)k);
}
// the token's account name (out) if it is a live session; touch: its last use now (at most once an hour)
static bool session_check(const char *token, char *name, bool drop) {
    const char *c = strchr(token, ':');
    if (!c || c - token > ACCT_NAME_MAX) return false;
    memcpy(name, token, (size_t)(c - token)), name[c - token] = 0;
    if (!valid_name(name) || !is_hex(c + 1, 64)) return false;
    uint8_t h[32];
    char sh[70], p[512];
    sha256(c + 1, 64, h);
    hex(h, 32, sh);
    pthread_mutex_lock(&g_store);
    user_path(p, sizeof p, name, "sessions");
    size_t n = 0;
    char *s = read_file(p, &n, 64 * 1024);
    bool ok = false, changed = false;
    char out[16 * 1024];
    int k = 0;
    for (char *l = s ? strtok(s, "\n") : NULL; l; l = strtok(NULL, "\n")) {
        char lh[70], dev[32] = "?";
        long long cr, u;
        if (sscanf(l, "%68s %lld %lld %30s", lh, &cr, &u, dev) < 3) continue;
        if (now_s() - u >= (int64_t)SESSION_DAYS * 86400) { changed = true; continue; }
        // (constant time: the stored hashes are the server's own, but the compare costs nothing)
        uint8_t a[32], b[32];
        if (unhex(lh, a, 32) == 32 && (memcpy(b, h, 32), !crypto_verify32(a, b))) {
            ok = true;
            if (drop) { changed = true; continue; }
            if (now_s() - u > 3600) u = now_s(), changed = true;
        }
        if (k < (int)sizeof out - 200) k += snprintf(out + k, sizeof out - (size_t)k, "%s %lld %lld %s\n", lh, cr, u, dev);
    }
    if (changed) write_file_atomic(p, out, (size_t)k);
    pthread_mutex_unlock(&g_store);
    free(s);
    return ok;
}

// ---- play tickets (acct_proto.h): Link Play / the shared world through this server need one; made by JOIN, kept in
// memory only (a restart: the games ask again), dropped with their session (LOGOUT) or ACCT_TICKET_S after made
#define MAX_TICKETS 4096
#define TICKETS_PER_ACCOUNT 16
typedef struct {
    bool used;
    uint8_t id[8], key[16], sess[32];  // (sess: the session token's hash - its sign-out drops the ticket)
    char name[ACCT_NAME_MAX + 1];
    int64_t made;
} Ticket;
static Ticket g_tickets[MAX_TICKETS];
static pthread_mutex_t g_tick = PTHREAD_MUTEX_INITIALIZER;
static void token_hash(const char *token, uint8_t out[32]) {  // (as session_check: SHA-256 of the secret's hex)
    const char *c = strchr(token, ':');
    sha256(c ? c + 1 : token, c ? strlen(c + 1) : strlen(token), out);
}
static bool ticket_new(const char *name, const char *token, char *out, size_t on) {
    pthread_mutex_lock(&g_tick);
    Ticket *t = NULL, *oldest = NULL, *oldest_mine = NULL;
    int mine = 0;
    for (int i = 0; i < MAX_TICKETS; i++) {
        Ticket *k = &g_tickets[i];
        if (k->used && now_s() - k->made >= ACCT_TICKET_S) k->used = false;
        if (!k->used) { if (!t) t = k; continue; }
        if (!oldest || k->made < oldest->made) oldest = k;
        if (!strcmp(k->name, name)) {
            mine++;
            if (!oldest_mine || k->made < oldest_mine->made) oldest_mine = k;
        }
    }
    if (mine >= TICKETS_PER_ACCOUNT) t = oldest_mine;  // (one account's games can't fill the table)
    else if (!t) t = oldest;
    memset(t, 0, sizeof *t);
    t->used = true, t->made = now_s();
    rnd(t->id, sizeof t->id);
    rnd(t->key, sizeof t->key);
    token_hash(token, t->sess);
    snprintf(t->name, sizeof t->name, "%s", name);
    char ih[17], kh[33];
    hex(t->id, 8, ih);
    hex(t->key, 16, kh);
    snprintf(out, on, "OK %s %s %d", ih, kh, ACCT_TICKET_S);
    crypto_wipe(kh, sizeof kh);
    pthread_mutex_unlock(&g_tick);
    return true;
}
static void tickets_drop(const char *token) {  // (its session signed out)
    uint8_t h[32];
    token_hash(token, h);
    pthread_mutex_lock(&g_tick);
    for (int i = 0; i < MAX_TICKETS; i++)
        if (g_tickets[i].used && !memcmp(g_tickets[i].sess, h, 32)) crypto_wipe(&g_tickets[i], sizeof g_tickets[i]);
    pthread_mutex_unlock(&g_tick);
}
bool accounts_ticket(const uint8_t id[8], uint8_t key[16], char *name, size_t n) {
    bool ok = false;
    pthread_mutex_lock(&g_tick);
    for (int i = 0; i < MAX_TICKETS && !ok; i++) {
        Ticket *t = &g_tickets[i];
        if (!t->used || memcmp(t->id, id, 8)) continue;
        if (now_s() - t->made >= ACCT_TICKET_S) { t->used = false; break; }
        memcpy(key, t->key, 16);
        snprintf(name, n, "%s", t->name);
        ok = true;
    }
    pthread_mutex_unlock(&g_tick);
    return ok;
}

// ---- saves
typedef struct {
    long long version, counter, saved, uploaded, size;
    char device[ACCT_DEVICE_MAX + 1], chash[65], bsha[65];
} SaveMeta;
static bool meta_parse(const char *s, SaveMeta *m) {
    memset(m, 0, sizeof *m);
    return sscanf(s, "%lld %lld %lld %24s %64s %64s %lld %lld", &m->version, &m->counter, &m->saved, m->device, m->chash, m->bsha,
                  &m->size, &m->uploaded) == 8;
}
static int meta_fmt(const SaveMeta *m, char *out, size_t n) {
    return snprintf(out, n, "%lld %lld %lld %s %s %s %lld %lld", m->version, m->counter, m->saved, m->device, m->chash, m->bsha,
                    m->size, m->uploaded);
}
static bool head_load(const char *name, const char *game, SaveMeta *m) {  // (under g_store; false: no save yet)
    char p[600], rest[64];
    snprintf(rest, sizeof rest, "saves/%s/head", game);
    user_path(p, sizeof p, name, rest);
    size_t n;
    char *s = read_file(p, &n, 1024);
    bool ok = s && meta_parse(s, m);
    free(s);
    return ok;
}
static long long user_bytes(const char *name, int *ngames, bool *has_game, const char *game) {  // (under g_store)
    char p[600];
    long long total = 0;
    *ngames = 0, *has_game = false;
    user_path(p, sizeof p, name, "saves");
#ifndef _WIN32
    DIR *d = opendir(p);
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        (*ngames)++;
        if (!strcmp(e->d_name, game)) *has_game = true;
        char gp[900];
        snprintf(gp, sizeof gp, "%s/%s", p, e->d_name);
        DIR *g = opendir(gp);
        if (!g) continue;
        for (struct dirent *f; (f = readdir(g));) {
            char fp[1200];
            struct stat sb;
            snprintf(fp, sizeof fp, "%s/%s", gp, f->d_name);
            if (strstr(f->d_name, ".bin") && !stat(fp, &sb)) total += sb.st_size;
        }
        closedir(g);
    }
    closedir(d);
#else
    char pat[700];
    snprintf(pat, sizeof pat, "%s/*", p);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return 0;
    do {
        if (fd.name[0] == '.') continue;
        (*ngames)++;
        if (!strcmp(fd.name, game)) *has_game = true;
        char gp[900];
        snprintf(gp, sizeof gp, "%s/%s/*.bin", p, fd.name);
        struct _finddata_t f;
        intptr_t g = _findfirst(gp, &f);
        if (g != -1) {
            do total += f.size;
            while (!_findnext(g, &f));
            _findclose(g);
        }
    } while (!_findnext(h, &fd));
    _findclose(h);
#endif
    return total;
}

// ---- the connection: framing over TLS
typedef struct { sock_t s; char ip[64]; mbedtls_ssl_context ssl; } Conn;
static int net_send(void *ctx, const unsigned char *b, size_t n) {
    int r = (int)send(((Conn *)ctx)->s, (const char *)b, (int)n, 0);
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}
static int net_recv(void *ctx, unsigned char *b, size_t n) {
    int r = (int)recv(((Conn *)ctx)->s, (char *)b, (int)n, 0);
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r == 0 ? MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY : r;
}
static bool tls_read(Conn *c, void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        int r = mbedtls_ssl_read(&c->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return false;
        b += r, n -= (size_t)r;
    }
    return true;
}
static bool tls_write(Conn *c, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        int r = mbedtls_ssl_write(&c->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return false;
        b += r, n -= (size_t)r;
    }
    return true;
}
static bool reply(Conn *c, const char *head, const void *body, size_t bn) {
    uint32_t h[2] = {(uint32_t)strlen(head), (uint32_t)bn};  // (little-endian hosts only: x86, ARM)
    return tls_write(c, h, 8) && tls_write(c, head, h[0]) && (!bn || tls_write(c, body, bn));
}
static bool replyf(Conn *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool replyf(Conn *c, const char *fmt, ...) {
    char b[ACCT_MAX_HEADER];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return reply(c, b, NULL, 0);
}

// ---- requests
static bool do_create_login(Conn *c, char **f, int nf, bool create) {
    if (nf != 4 || !valid_device(f[3])) return replyf(c, "ERR bad request");
    for (char *q = f[1]; *q; q++)
        if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');  // (names aren't case sensitive)
    uint8_t pass[ACCT_PASS_MAX];
    int pn = unhex(f[2], pass, sizeof pass);
    if (!valid_name(f[1])) {
        if (create) return replyf(c, "ERR bad Names are 3-24 letters, digits, _ . or -");
        pn = -1;  // (a login with a name nobody can have: wrong, below)
    }
    if (create) {
        if (!g_o.signup) return replyf(c, "ERR closed This server doesn't take new accounts");
        if (pn < ACCT_PASS_MIN) return replyf(c, "ERR bad Passwords are at least %d characters", ACCT_PASS_MIN);
        int w = create_blocked(c->ip, false);
        if (w) return replyf(c, "ERR wait %d Too many new accounts from your address: try again later", w);
        pthread_mutex_lock(&g_store);
        Account a;
        bool taken = account_load(f[1], &a);
        int n = count_accounts();
        pthread_mutex_unlock(&g_store);
        if (taken) return replyf(c, "ERR taken That name is taken");
        if (n >= g_o.max_accounts) return replyf(c, "ERR full This server has no room for more accounts");
        memset(&a, 0, sizeof a);
        a.blocks = ARGON_BLOCKS, a.passes = ARGON_PASSES, a.created = now_s();
        rnd(a.salt, 16);
        argon2id(pass, (size_t)pn, a.salt, a.blocks, a.passes, a.hash);
        crypto_wipe(pass, sizeof pass);
        char p[512], token[ACCT_TOKEN_MAX + 1];
        pthread_mutex_lock(&g_store);
        Account other;
        bool ok = false;
        if (account_load(f[1], &other)) taken = true;  // (made meanwhile)
        else {
            user_path(p, sizeof p, f[1], "");
            mkdir_(p);
            user_path(p, sizeof p, f[1], "saves");
            mkdir_(p);
            ok = account_save(f[1], &a) && session_new(f[1], f[3], token);
        }
        pthread_mutex_unlock(&g_store);
        if (taken) return replyf(c, "ERR taken That name is taken");
        if (!ok) return replyf(c, "ERR server The server couldn't save the account");
        create_blocked(c->ip, true);
        logf_("new account %s (%s, %s)\n", f[1], f[3], c->ip);
        return replyf(c, "OK %s", token);
    }
    int w = login_blocked(c->ip, f[1]);
    if (w) {
        crypto_wipe(pass, sizeof pass);
        return replyf(c, "ERR wait %d Too many wrong tries: wait a few minutes", w);
    }
    Account a;
    pthread_mutex_lock(&g_store);
    bool have = pn >= 0 && account_load(f[1], &a);
    pthread_mutex_unlock(&g_store);
    if (!have) {  // (the same work as a real check: no telling which names exist from the time taken)
        memset(&a, 0, sizeof a);
        a.blocks = ARGON_BLOCKS, a.passes = ARGON_PASSES;
        rnd(a.hash, 32);
    }
    uint8_t h[32];
    argon2id(pass, pn > 0 ? (size_t)pn : 0, a.salt, a.blocks, a.passes, h);
    crypto_wipe(pass, sizeof pass);
    if (!have || crypto_verify32(h, a.hash)) {
        login_failed(c->ip, f[1]);
        logf_("wrong name or password for %.24s (%s)\n", f[1], c->ip);
        return replyf(c, "ERR wrong Wrong name or password");
    }
    char token[ACCT_TOKEN_MAX + 1];
    pthread_mutex_lock(&g_store);
    bool ok = session_new(f[1], f[3], token);
    pthread_mutex_unlock(&g_store);
    if (!ok) return replyf(c, "ERR server The server couldn't save the session");
    logf_("%s signed in (%s, %s)\n", f[1], f[3], c->ip);
    return replyf(c, "OK %s", token);
}

static bool do_save(Conn *c, char **f, int nf, const char *body, uint32_t bn) {
    const char *cmd = f[0];
    char name[ACCT_NAME_MAX + 1];
    if (nf < 3 || !is_hex(f[2], 16)) return replyf(c, "ERR bad request");
    if (!session_check(f[1], name, false)) return replyf(c, "ERR signin Signed out: sign in again");
    const char *game = f[2];
    char gdir[600], rest[64];
    snprintf(rest, sizeof rest, "saves/%s", game);
    user_path(gdir, sizeof gdir, name, rest);
    SaveMeta m;
    char line[512];
    if (!strcmp(cmd, "INFO")) {
        if (nf != 3) return replyf(c, "ERR bad request");
        pthread_mutex_lock(&g_store);
        bool have = head_load(name, game, &m);
        pthread_mutex_unlock(&g_store);
        if (!have) return replyf(c, "OK 0");
        meta_fmt(&m, line, sizeof line);
        return replyf(c, "OK %s", line);
    }
    if (!strcmp(cmd, "LIST")) {
        if (nf != 3) return replyf(c, "ERR bad request");
        pthread_mutex_lock(&g_store);
        char out[16 * 1024];
        int k = 0, n = 0;
        if (head_load(name, game, &m))
            for (long long v = m.version; v > 0 && v > m.version - g_o.versions; v--) {
                char p[700], *s;
                size_t sn;
                snprintf(p, sizeof p, "%s/v%lld.meta", gdir, v);
                if (!(s = read_file(p, &sn, 1024))) continue;
                SaveMeta vm;
                if (meta_parse(s, &vm)) {
                    meta_fmt(&vm, line, sizeof line);
                    k += snprintf(out + k, sizeof out - (size_t)k, "%s\n", line);
                    n++;
                }
                free(s);
            }
        pthread_mutex_unlock(&g_store);
        char h[32];
        snprintf(h, sizeof h, "OK %d", n);
        return reply(c, h, out, (size_t)k);
    }
    if (!strcmp(cmd, "GET")) {
        if (nf != 4) return replyf(c, "ERR bad request");
        long long v = atoll(f[3]);
        char p[700], *s;
        size_t sn = 0, bnn = 0;
        pthread_mutex_lock(&g_store);
        snprintf(p, sizeof p, "%s/v%lld.meta", gdir, v);
        s = read_file(p, &sn, 1024);
        snprintf(p, sizeof p, "%s/v%lld.bin", gdir, v);
        char *blob = s ? read_file(p, &bnn, (size_t)g_o.save_max_mb << 20) : NULL;
        pthread_mutex_unlock(&g_store);
        bool ok = s && blob && meta_parse(s, &m) && (long long)bnn == m.size;
        free(s);
        if (!ok) {
            free(blob);
            return replyf(c, "ERR bad No such save version");
        }
        meta_fmt(&m, line, sizeof line);
        char h[600];
        snprintf(h, sizeof h, "OK %s", line);
        bool r = reply(c, h, blob, bnn);
        free(blob);
        return r;
    }
    // PUT <token> <game> <base> <counter> <saved> <device> <content hash> <blob sha256>
    if (nf != 9 || !valid_device(f[6]) || !is_hex(f[7], 64) || !is_hex(f[8], 64)) return replyf(c, "ERR bad request");
    if (!bn) return replyf(c, "ERR bad An empty save");
    uint8_t h[32];
    char hs[70];
    sha256(body, bn, h);
    hex(h, 32, hs);
    if (strcmp(hs, f[8])) return replyf(c, "ERR corrupt The save arrived damaged: try again");
    long long base = atoll(f[3]);
    pthread_mutex_lock(&g_store);
    bool have = head_load(name, game, &m);
    if ((have ? m.version : 0) != base) {
        pthread_mutex_unlock(&g_store);
        meta_fmt(&m, line, sizeof line);
        return replyf(c, "CONFLICT %s", line);
    }
    int ngames;
    bool has_game;
    long long used = user_bytes(name, &ngames, &has_game, game);
    if (!has_game && ngames >= MAX_GAMES) {
        pthread_mutex_unlock(&g_store);
        return replyf(c, "ERR quota Too many games on this account");
    }
    // (the oldest version kept goes when this one comes: not counted)
    long long drop = 0;
    if (have && m.version - g_o.versions + 1 >= 1) {
        char p[700];
        struct stat sb;
        snprintf(p, sizeof p, "%s/v%lld.bin", gdir, m.version - g_o.versions + 1);
        if (!stat(p, &sb)) drop = sb.st_size;
    }
    if (used - drop + bn > ((long long)g_o.quota_mb << 20)) {
        pthread_mutex_unlock(&g_store);
        return replyf(c, "ERR quota This account's space on the server is full");
    }
    mkdir_(gdir);
    SaveMeta nm = {0};
    nm.version = (have ? m.version : 0) + 1, nm.counter = atoll(f[4]), nm.saved = atoll(f[5]), nm.uploaded = now_s(), nm.size = bn;
    snprintf(nm.device, sizeof nm.device, "%s", f[6]);
    memcpy(nm.chash, f[7], 65), memcpy(nm.bsha, f[8], 65);
    char p[700];
    snprintf(p, sizeof p, "%s/v%lld.bin", gdir, nm.version);
    int ln = meta_fmt(&nm, line, sizeof line);
    bool ok = write_file_atomic(p, body, bn);
    snprintf(p, sizeof p, "%s/v%lld.meta", gdir, nm.version);
    ok = ok && write_file_atomic(p, line, (size_t)ln);
    snprintf(p, sizeof p, "%s/head", gdir);
    ok = ok && write_file_atomic(p, line, (size_t)ln);  // (the head last: a failure before leaves the old one)
    if (ok)
        for (long long v = nm.version - g_o.versions; v > 0; v--) {  // (versions beyond the kept ones)
            snprintf(p, sizeof p, "%s/v%lld.bin", gdir, v);
            if (remove(p)) break;
            snprintf(p, sizeof p, "%s/v%lld.meta", gdir, v);
            remove(p);
        }
    pthread_mutex_unlock(&g_store);
    if (!ok) return replyf(c, "ERR server The server couldn't store the save");
    logf_("%s: save %lld of %s (%u bytes, %s)\n", name, nm.version, game, bn, nm.device);
    return replyf(c, "OK %s", line);
}

static void *conn_thread(void *arg) {
    Conn *c = arg;
#ifdef _WIN32
    DWORD tv = IO_TIMEOUT_S * 1000;
#else
    struct timeval tv = {IO_TIMEOUT_S, 0};
#endif
    setsockopt(c->s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    setsockopt(c->s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
    mbedtls_ssl_init(&c->ssl);
    char *body = NULL;
    if (mbedtls_ssl_setup(&c->ssl, &g_conf)) goto done;
    mbedtls_ssl_set_bio(&c->ssl, c, net_send, net_recv, NULL);
    int r;
    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0)
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) goto done;
    for (int req = 0; req < MAX_REQUESTS_PER_CONN; req++) {
        uint32_t h[2];
        char head[ACCT_MAX_HEADER + 1];
        if (!tls_read(c, h, 8) || h[0] == 0 || h[0] > ACCT_MAX_HEADER) break;
        if (h[1] > ((uint32_t)g_o.save_max_mb << 20)) {
            replyf(c, "ERR big The save is bigger than this server takes (%d MB)", g_o.save_max_mb);
            break;
        }
        if (!tls_read(c, head, h[0])) break;
        head[h[0]] = 0;
        char *f[12];
        int nf = 0;
        for (char *t = strtok(head, " "); t && nf < 12; t = strtok(NULL, " ")) f[nf++] = t;
        if (!nf) break;
        free(body);
        body = NULL;
        if (h[1]) {  // (a body only with an upload from a signed-in player: nobody else makes the server hold megabytes)
            char who[ACCT_NAME_MAX + 1];
            if (strcmp(f[0], "PUT") || nf < 2 || !session_check(f[1], who, false)) {
                replyf(c, "ERR signin Signed out: sign in again");
                break;
            }
            body = malloc(h[1]);
            if (!body || !tls_read(c, body, h[1])) break;
        }
        bool ok;
        if (!strcmp(f[0], "CREATE") || !strcmp(f[0], "LOGIN")) ok = do_create_login(c, f, nf, f[0][0] == 'C');
        else if (!strcmp(f[0], "LOGOUT") || !strcmp(f[0], "WHOAMI")) {
            char name[ACCT_NAME_MAX + 1];
            bool out = f[0][0] == 'L';
            if (nf != 2 || !session_check(f[1], name, out)) ok = replyf(c, "ERR signin Signed out: sign in again");
            else {
                if (out) logf_("%s signed out a device (%s)\n", name, c->ip), tickets_drop(f[1]);
                ok = out ? replyf(c, "OK") : replyf(c, "OK %s", name);
            }
        } else if (!strcmp(f[0], "JOIN")) {  // (a play ticket: acct_proto.h)
            char name[ACCT_NAME_MAX + 1], out[120];
            if (nf != 2 || !session_check(f[1], name, false)) ok = replyf(c, "ERR signin Signed out: sign in again");
            else {
                ticket_new(name, f[1], out, sizeof out);
                ok = reply(c, out, NULL, 0);
                crypto_wipe(out, sizeof out);
                logf_("%s: a play ticket (%s)\n", name, c->ip);
            }
        } else if (!strcmp(f[0], "INFO") || !strcmp(f[0], "GET") || !strcmp(f[0], "PUT") || !strcmp(f[0], "LIST"))
            ok = do_save(c, f, nf, body, h[1]);
        else ok = replyf(c, "ERR bad Unknown request");
        if (!ok) break;
    }
    mbedtls_ssl_close_notify(&c->ssl);
done:
    free(body);
    mbedtls_ssl_free(&c->ssl);
    closesock(c->s);
    pthread_mutex_lock(&g_lim);
    g_conns--;
    char k[80];
    snprintf(k, sizeof k, "n:%s", c->ip);
    Tries *t = tries_of(k);  // (n: connections open from that address)
    if (t->n > 0) t->n--;
    pthread_mutex_unlock(&g_lim);
    free(c);
    return NULL;
}

static sock_t g_ls = NO_SOCK;
static void *accept_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct sockaddr_storage a;
        socklen_t al = sizeof a;
        sock_t s = accept(g_ls, (struct sockaddr *)&a, &al);
        if (s == NO_SOCK) continue;
        Conn *c = calloc(1, sizeof *c);
        c->s = s;
        if (a.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&a)->sin_addr, c->ip, sizeof c->ip);
        else inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&a)->sin6_addr, c->ip, sizeof c->ip);
        char k[80];
        snprintf(k, sizeof k, "n:%s", c->ip);
        pthread_mutex_lock(&g_lim);
        Tries *t = tries_of(k);
        bool ok = g_conns < MAX_CONNS && t->n < MAX_CONNS_PER_IP;
        if (ok) g_conns++, t->n++, t->last = now_s();
        pthread_mutex_unlock(&g_lim);
        pthread_t th;
        if (!ok || pthread_create(&th, NULL, conn_thread, c)) {
            if (ok) {
                pthread_mutex_lock(&g_lim);
                g_conns--, t->n--;
                pthread_mutex_unlock(&g_lim);
            }
            closesock(s);
            free(c);
            continue;
        }
        pthread_detach(th);
    }
    return NULL;
}

// ---- the certificate: the operator's, or our own (made once, kept in <data>/tls)
static bool make_cert(const char *key_path, const char *cert_path) {
    mbedtls_pk_context k;
    mbedtls_pk_init(&k);
    bool ok = !mbedtls_pk_setup(&k, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) &&
              !mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(k), mbedtls_ctr_drbg_random, &g_drbg);
    unsigned char *pem = calloc(1, 16384);
    if (ok) ok = !mbedtls_pk_write_key_pem(&k, pem, 16384) && write_file_atomic(key_path, pem, strlen((char *)pem));
    if (ok) {
        mbedtls_x509write_cert w;
        mbedtls_x509write_crt_init(&w);
        unsigned char serial[16];
        rnd(serial, sizeof serial);
        serial[0] &= 0x7f;
        mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial);
        mbedtls_x509write_crt_set_validity(&w, "20250101000000", "20491231235959");
        mbedtls_x509write_crt_set_subject_name(&w, "CN=lzaserver");
        mbedtls_x509write_crt_set_issuer_name(&w, "CN=lzaserver");
        mbedtls_x509write_crt_set_subject_key(&w, &k);
        mbedtls_x509write_crt_set_issuer_key(&w, &k);
        mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
        ok = !mbedtls_x509write_crt_pem(&w, pem, 16384, mbedtls_ctr_drbg_random, &g_drbg) &&
             write_file_atomic(cert_path, pem, strlen((char *)pem));
        mbedtls_x509write_crt_free(&w);
    }
    free(pem);
    mbedtls_pk_free(&k);
    return ok;
}

static void fingerprint(char *out) {  // (what the game pins: SHA-256 of the public key, runtime/acct.c)
    unsigned char der[2048];
    int n = mbedtls_pk_write_pubkey_der(&g_cert.pk, der, sizeof der);
    uint8_t h[32];
    if (n > 0) sha256(der + sizeof der - n, (size_t)n, h);
    else memset(h, 0, 32);
    for (int i = 0; i < 32; i++) sprintf(out + 3 * i, "%02x%s", h[i], i < 31 ? ":" : "");
}

static bool crypto_setup(void) {
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    return !mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, (const unsigned char *)"lzaserver", 9);
}

int accounts_start(const AccountsOptions *o) {
    g_o = *o;
    if (!crypto_setup()) {
        fprintf(stderr, "lzaserver: accounts: no random numbers\n");
        return 1;
    }
    char p[600], kp[600], cp[600];
    mkdir_(g_o.data);
    snprintf(p, sizeof p, "%s/users", g_o.data);
    mkdir_(p);
    snprintf(p, sizeof p, "%s/tls", g_o.data);
    mkdir_(p);
    snprintf(kp, sizeof kp, "%s", g_o.key[0] ? g_o.key : "");
    snprintf(cp, sizeof cp, "%s", g_o.cert[0] ? g_o.cert : "");
    if (!kp[0]) {
        snprintf(kp, sizeof kp, "%s/tls/key.pem", g_o.data);
        snprintf(cp, sizeof cp, "%s/tls/cert.pem", g_o.data);
        struct stat sb;
        if (stat(kp, &sb) || stat(cp, &sb)) {
            if (!make_cert(kp, cp)) {
                fprintf(stderr, "lzaserver: accounts: can't make a certificate in %s/tls\n", g_o.data);
                return 1;
            }
            printf("lzaserver: made this server's own certificate (%s)\n", cp);
        }
    }
    mbedtls_x509_crt_init(&g_cert);
    mbedtls_pk_init(&g_key);
    if (mbedtls_x509_crt_parse_file(&g_cert, cp) || mbedtls_pk_parse_keyfile(&g_key, kp, NULL, mbedtls_ctr_drbg_random, &g_drbg)) {
        fprintf(stderr, "lzaserver: accounts: can't read the certificate %s / key %s\n", cp, kp);
        return 1;
    }
    mbedtls_ssl_config_init(&g_conf);
    if (mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) ||
        mbedtls_ssl_conf_own_cert(&g_conf, &g_cert, &g_key)) {
        fprintf(stderr, "lzaserver: accounts: TLS setup failed\n");
        return 1;
    }
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_min_tls_version(&g_conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&g_conf, MBEDTLS_SSL_VERSION_TLS1_3);

    g_ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)g_o.port)};
    a.sin_addr.s_addr = g_o.bind[0] ? inet_addr(g_o.bind) : htonl(INADDR_ANY);
    if (g_ls == NO_SOCK || bind(g_ls, (struct sockaddr *)&a, sizeof a) || listen(g_ls, 32)) {
        fprintf(stderr, "lzaserver: accounts: can't use TCP port %d\n", g_o.port);
        return 1;
    }
    char fp[100];
    fingerprint(fp);
    pthread_mutex_lock(&g_store);
    int n = count_accounts();
    pthread_mutex_unlock(&g_store);
    printf("lzaserver: accounts + save sync on TCP port %d (TLS), %d accounts, new accounts %s; saves up to %d MB, %d versions, "
           "%d MB per account\nlzaserver: certificate key fingerprint (players see it at their first sign-in): %s\n",
           g_o.port, n, g_o.signup ? "open" : "closed", g_o.save_max_mb, g_o.versions, g_o.quota_mb, fp);
    pthread_t th;
    if (pthread_create(&th, NULL, accept_thread, NULL)) return 1;
    pthread_detach(th);
    return 0;
}

// lzaserver --add-user <name> (the operator, with new accounts closed): the password from the first line of stdin
int accounts_add_user(const AccountsOptions *o, const char *name_in) {
    g_o = *o;
    if (!crypto_setup()) return 1;
    char name[64], pw[ACCT_PASS_MAX + 2];
    snprintf(name, sizeof name, "%s", name_in);
    for (char *q = name; *q; q++)
        if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
    if (!valid_name(name)) {
        fprintf(stderr, "names are 3-24 of a-z 0-9 _ . -\n");
        return 2;
    }
    fprintf(stderr, "password for %s (one line): ", name);
    if (!fgets(pw, sizeof pw, stdin)) return 2;
    pw[strcspn(pw, "\r\n")] = 0;
    if (strlen(pw) < ACCT_PASS_MIN) {
        fprintf(stderr, "at least %d characters\n", ACCT_PASS_MIN);
        return 2;
    }
    char p[600];
    mkdir_(g_o.data);
    snprintf(p, sizeof p, "%s/users", g_o.data);
    mkdir_(p);
    Account a = {ARGON_BLOCKS, ARGON_PASSES, {0}, {0}, now_s()};
    if (account_load(name, &a)) {
        fprintf(stderr, "%s exists\n", name);
        return 1;
    }
    a.blocks = ARGON_BLOCKS, a.passes = ARGON_PASSES, a.created = now_s();
    rnd(a.salt, 16);
    argon2id((const uint8_t *)pw, strlen(pw), a.salt, a.blocks, a.passes, a.hash);
    crypto_wipe(pw, sizeof pw);
    user_path(p, sizeof p, name, "");
    mkdir_(p);
    user_path(p, sizeof p, name, "saves");
    mkdir_(p);
    if (!account_save(name, &a)) return 1;
    printf("account %s made\n", name);
    return 0;
}
