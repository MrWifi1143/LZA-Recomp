// Online server accounts and save sync. Playing the game never needs an account: offline, with the save on this PC,
// nothing here does anything. An account lives on one online server (larecomp.ini online_server; contrib/lzaserver;
// the launcher's Options > Online tab) and is what PLAYING on that server needs (Link Play by Link Code, the shared
// world - an lzaserver lets only signed-in players in unless it runs with --no-accounts / --open-play). Nearby play
// (ldn.c) and Steam friends (no server set) need none.
// Signing in links the save on this PC to the account (Steam Cloud style): the save here stays the truth while playing;
// it's synced at start (if the server can be reached), after each save and at quit - offline in between is fine, what
// couldn't go up goes up once the server answers again (tried every 30 s). Another copy of the game signed in to the
// same account (another PC, the PS5 port: this file is in their shared runtime) carries on from it.
// Play tickets: while the game runs, a thread here gets a ticket for the signed-in account (JOIN, acct_proto.h) and
// hands it to the relay plugin (larecomp_server_ticket), whose hellos prove it to the server.
// Logs never hold a password, a token or the server's address (alog hides the addresses it has seen).
//
// Security: TLS 1.3 (Mbed TLS, third_party/mbedtls). The server's key is pinned the first time this copy talks to it
// (online/known_servers.txt: SHA-256 of the public key, per server address); later a different key is refused, unless
// a certificate authority vouches for it for that host name (an operator's real certificate, renewed). The password
// goes only inside TLS and is never written here; what is kept is the session token the server hands out
// (online/accounts.txt, readable by this user only).
//
// Save sync (per account, per game = program id, per save folder - the PS5 keeps one per console user): at boot,
// before the game reads its save, the server's latest is compared with the save here:
//   - same content: nothing;      - only the server's changed since the last sync: the save here is backed up, the
//   server's comes down;          - only ours changed (or the server has none): ours goes up;
//   - both changed (a conflict): nothing is overwritten - the newer is used, the other kept as a backup
//     (<save folder>/sync-backups/<time>-<why>/; the server's older versions stay on the server too).
// Each time the game commits its save, the save goes up a moment later (in the background) and at exit (waited for).
// An upload is based on the version this copy last saw: if another copy uploaded since, the server refuses it and
// the conflict is settled at the next start (as above). The launcher shows the last result (acct_last_sync).
// Files (data root): online/accounts.txt, online/known_servers.txt, online/sync/<key>.txt (the last sync's server
// version, counter and content hash), online/last_sync.txt.
// Tests: lasrecomp --account ... (acct_cli); LARECOMP_ACCT_LOG=1 logs each request.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef __PROSPERO__
#include <utime.h>
#endif

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "miniz/miniz.h"

#include "acct.h"
#include "acct_proto.h"
#include "rt.h"
#include "settings.h"

#ifdef _WIN32
typedef SOCKET sock_t;
#define NO_SOCK INVALID_SOCKET
#define closesock closesocket
#define MKDIR(p) _mkdir(p)
#else
typedef int sock_t;
#define NO_SOCK (-1)
#define closesock close
#define MKDIR(p) mkdir(p, 0700)
#endif

#define CONNECT_TIMEOUT_MS 5000
#define IO_TIMEOUT_S 20
#define BLOB_MAX (64u << 20)
#define SAVE_FILES_MAX 64
#define BACKUPS_KEPT 20

#if defined(__PROSPERO__)
#define ACCT_DEVICE "PS5"
#elif defined(_WIN32)
#define ACCT_DEVICE "PC-Windows"
#elif defined(__aarch64__)
#define ACCT_DEVICE "Linux-ARM"
#else
#define ACCT_DEVICE "PC-Linux"
#endif

extern const char *g_save_dir;  // (services/fs.c)
void menu_notice_for(const char *text, unsigned ms);  // (ui/menu.mm)

static bool g_log;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // (the files under online/, the sync state)

// (the server addresses seen: never written to the log - alog puts "<server>" in their place)
static pthread_mutex_t g_hide_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_hide[8][256];
static int g_nhide;
static void hide_in_log(const char *s) {
    if (!s || strlen(s) < 2) return;
    pthread_mutex_lock(&g_hide_lock);
    bool have = false;
    for (int i = 0; i < g_nhide && !have; i++) have = !strcmp(g_hide[i], s);
    if (!have) snprintf(g_hide[g_nhide < 8 ? g_nhide++ : 7], sizeof g_hide[0], "%s", s);
    pthread_mutex_unlock(&g_hide_lock);
}
static void scrub(char *b, size_t bn) {  // (the server addresses seen -> "<server>")
    char o[1400];
    pthread_mutex_lock(&g_hide_lock);
    for (int i = 0; i < g_nhide; i++) {  // (the longest first would be nicer; "host:port" is added before "host")
        char *at;
        while ((at = strstr(b, g_hide[i]))) {
            snprintf(o, sizeof o, "%.*s<server>%s", (int)(at - b), b, at + strlen(g_hide[i]));
            snprintf(b, bn, "%s", o);
        }
    }
    pthread_mutex_unlock(&g_hide_lock);
}
static void alog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void alog(const char *fmt, ...) {
    char b[1300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    scrub(b, sizeof b);
    LOG("[account] %s\n", b);
}
static void say(const char *m) {  // (lasrecomp --account's answer: its stdout is the log - the address left out too)
    char b[1300];
    snprintf(b, sizeof b, "%s", m);
    scrub(b, sizeof b);
    printf("%s\n", b);
}

// ------------------------------------------------------------------ small helpers
static void hex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) out[2 * i] = d[b[i] >> 4], out[2 * i + 1] = d[b[i] & 15];
    out[2 * n] = 0;
}
static void mkdirs(const char *path) {  // (each folder of path; path itself too)
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 1; *q; q++)
        if (*q == '/') *q = 0, MKDIR(p), *q = '/';
    MKDIR(p);
}
static bool write_atomic(const char *path, const void *data, size_t n, bool secret) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
#ifndef _WIN32
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, secret ? 0600 : 0644);
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
#else
    (void)secret;
    FILE *f = fopen(tmp, "wb");
#endif
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    if (ok) remove(path);
#endif
    if (!ok || rename(tmp, path)) { remove(tmp); return false; }
    return true;
}
static char *read_all(const char *path, size_t *n, size_t max) {
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
// "host[:port]" -> host, port; the key the files use is "host:port". IPv6: "[v6]:port", "[v6]" or a bare v6 address
// (more than one ':' - no port then); its key is "[v6]:port".
static bool parse_server(const char *s, char *host, size_t hn, int *port, char *key, size_t kn) {
    while (*s == ' ') s++;
    if (!*s) return false;
    const char *b = s, *c;
    size_t l;
    if (*s == '[' && strchr(s, ']')) {
        const char *e = strchr(s, ']');
        b = s + 1, l = (size_t)(e - b), c = e[1] == ':' ? e + 1 : NULL;
        if (e[1] && e[1] != ':') return false;
    } else {
        c = strrchr(s, ':');
        if (c && strchr(s, ':') != c) c = NULL;  // (a bare IPv6 address)
        l = c ? (size_t)(c - s) : strlen(s);
    }
    if (!l || l >= hn) return false;
    memcpy(host, b, l), host[l] = 0;
    *port = c ? atoi(c + 1) : ACCT_DEFAULT_PORT;
    if (*port <= 0 || *port > 65535) return false;
    for (char *q = host; *q; q++)
        if (*q == ' ' || *q == '/' || *q == '[' || *q == ']') return false;
    snprintf(key, kn, strchr(host, ':') ? "[%s]:%d" : "%s:%d", host, *port);
    hide_in_log(key), hide_in_log(s), hide_in_log(host);
    return true;
}

// ------------------------------------------------------------------ the files under online/
// online/accounts.txt "<server> <token>"; online/known_servers.txt "<server> <pin>"
static bool kv_get(const char *file, const char *key, char *val, size_t vn) {
    size_t n;
    char *s = read_all(file, &n, 1 << 20);
    bool ok = false;
    for (char *l = s ? strtok(s, "\n") : NULL; l && !ok; l = strtok(NULL, "\n")) {
        char k[300], v[300];
        if (sscanf(l, "%299s %299s", k, v) == 2 && !strcmp(k, key)) snprintf(val, vn, "%s", v), ok = true;
    }
    free(s);
    return ok;
}
static bool kv_set(const char *file, const char *key, const char *val, bool secret) {  // (val NULL: removed)
    size_t n;
    char *s = read_all(file, &n, 1 << 20);
    size_t cap = (s ? n : 0) + 1024;
    char *out = malloc(cap);
    size_t k = 0;
    for (char *l = s ? strtok(s, "\n") : NULL; l; l = strtok(NULL, "\n")) {
        char kk[300];
        if (sscanf(l, "%299s", kk) == 1 && !strcmp(kk, key)) continue;
        k += (size_t)snprintf(out + k, cap - k, "%s\n", l);
    }
    if (val) k += (size_t)snprintf(out + k, cap - k, "%s %s\n", key, val);
    mkdirs("online");
    bool ok = write_atomic(file, out, k, secret);
    free(out);
    free(s);
    return ok;
}
#define ACCOUNTS_FILE "online/accounts.txt"
#define PINS_FILE "online/known_servers.txt"
#define LAST_FILE "online/last_sync.txt"

static void set_last(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_last(const char *fmt, ...) {  // (the launcher's line: what the last sync did)
    char b[600], t[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(t, sizeof t, "%Y-%m-%d %H:%M", &tmv);
    char line[700];
    int n = snprintf(line, sizeof line, "%s: %s\n", t, b);
    mkdirs("online");
    write_atomic(LAST_FILE, line, (size_t)n, false);
    alog("%s", b);
}

// ------------------------------------------------------------------ TLS
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;
static bool g_tls_ok;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt g_cas;  // (the system's certificate authorities, where there is a bundle: Linux)
static bool g_have_cas;

#ifdef MBEDTLS_NO_PLATFORM_ENTROPY  // (the PS5: the platform layer's arc4random_buf)
void arc4random_buf(void *buf, size_t n);
static int entropy_arc4(void *data, unsigned char *out, size_t len, size_t *olen) {
    (void)data;
    arc4random_buf(out, len);
    *olen = len;
    return 0;
}
#endif

static void tls_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    g_log = getenv("LARECOMP_ACCT_LOG") != NULL;
    if (psa_crypto_init() != PSA_SUCCESS) return;
    mbedtls_entropy_init(&g_entropy);
#ifdef MBEDTLS_NO_PLATFORM_ENTROPY
    mbedtls_entropy_add_source(&g_entropy, entropy_arc4, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
#endif
    mbedtls_ctr_drbg_init(&g_drbg);
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, (const unsigned char *)"lzarecomp-acct", 14)) return;
    mbedtls_x509_crt_init(&g_cas);
    static const char *bundles[] = {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                                    "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem", NULL};
    for (int i = 0; bundles[i] && !g_have_cas; i++) {
        struct stat sb;
        if (!stat(bundles[i], &sb)) g_have_cas = mbedtls_x509_crt_parse_file(&g_cas, bundles[i]) >= 0 && g_cas.version;
    }
    g_tls_ok = true;
}

typedef struct {
    sock_t s;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
} Tls;

static int net_send(void *ctx, const unsigned char *b, size_t n) {
    int r = (int)send(((Tls *)ctx)->s, (const char *)b, (int)n, 0);
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}
static int net_recv(void *ctx, unsigned char *b, size_t n) {
    int r = (int)recv(((Tls *)ctx)->s, (char *)b, (int)n, 0);
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r == 0 ? MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY : r;
}

static sock_t tcp_connect(const char *host, int port, char *err, size_t en) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC, hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res) {
        snprintf(err, en, "Can't find the server %s", host);
        return NO_SOCK;
    }
    sock_t s = NO_SOCK;
    for (struct addrinfo *a = res; a && s == NO_SOCK; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == NO_SOCK) continue;
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        int r = connect(s, a->ai_addr, (int)a->ai_addrlen);
        bool ok = !r;
        if (r && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set w, x;
            FD_ZERO(&w);
            FD_ZERO(&x);
            FD_SET(s, &w);
            FD_SET(s, &x);
            struct timeval tv = {CONNECT_TIMEOUT_MS / 1000, (CONNECT_TIMEOUT_MS % 1000) * 1000};
            ok = select(0, NULL, &w, &x, &tv) > 0 && FD_ISSET(s, &w);
        }
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        DWORD tv2 = IO_TIMEOUT_S * 1000;
#else
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        int r = connect(s, a->ai_addr, a->ai_addrlen);
        bool ok = !r;
        if (r && errno == EINPROGRESS) {
            struct pollfd p = {s, POLLOUT, 0};
            int e = 0;
            socklen_t el = sizeof e;
            ok = poll(&p, 1, CONNECT_TIMEOUT_MS) == 1 && !getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) && !e;
        }
        fcntl(s, F_SETFL, fl);
        struct timeval tv2 = {IO_TIMEOUT_S, 0};
#endif
        if (!ok) {
            closesock(s);
            s = NO_SOCK;
            continue;
        }
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv2, sizeof tv2);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv2, sizeof tv2);
    }
    freeaddrinfo(res);
    if (s == NO_SOCK) snprintf(err, en, "Can't reach the server %s:%d (is it running? TCP port %d open?)", host, port, port);
    return s;
}

static void pin_of(const mbedtls_x509_crt *c, char *out) {
    unsigned char der[4096];
    uint8_t h[32] = {0};
    int n = mbedtls_pk_write_pubkey_der(&c->pk, der, sizeof der);
    if (n > 0) mbedtls_sha256(der + sizeof der - n, (size_t)n, h, 0);
    hex(h, 32, out);
}
static void pin_pretty(const char *pin, char *out) {  // (as lzaserver prints it: ab:cd:...)
    int k = 0;
    for (int i = 0; i < 64; i += 2) k += sprintf(out + k, "%c%c%s", pin[i], pin[i + 1], i < 62 ? ":" : "");
}

enum { TLS_OK, TLS_FAIL, TLS_KEY_CHANGED };
static int g_last_tls;
// a connection to the server, its key checked (see the top); *first: this server's key was just remembered
static bool tls_open(Tls *t, const char *server, char *err, size_t en, bool *first, char *pin_out) {
    pthread_once(&g_tls_once, tls_init);
    g_last_tls = TLS_FAIL;
    if (!g_tls_ok) { snprintf(err, en, "No encryption available (random numbers)"); return false; }
    char host[256], key[300];
    int port;
    if (!parse_server(server, host, sizeof host, &port, key, sizeof key)) { snprintf(err, en, "Set the online server's address first"); return false; }
    t->s = tcp_connect(host, port, err, en);
    if (t->s == NO_SOCK) return false;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) goto fail;
    mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);  // (the pin decides when no authority vouches)
    if (g_have_cas) mbedtls_ssl_conf_ca_chain(&t->conf, &g_cas, NULL);
    if (mbedtls_ssl_setup(&t->ssl, &t->conf) || mbedtls_ssl_set_hostname(&t->ssl, host)) goto fail;
    mbedtls_ssl_set_bio(&t->ssl, t, net_send, net_recv, NULL);
    int r;
    while ((r = mbedtls_ssl_handshake(&t->ssl)) != 0)
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char e[120];
            mbedtls_strerror(r, e, sizeof e);
            alog("TLS handshake with %s: %s", key, e);
            snprintf(err, en, "Couldn't open a secure connection to %s (is it an lzaserver with accounts?)", key);
            goto fail_quiet;
        }
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&t->ssl);
    if (!peer) goto fail;
    char pin[65], known[100];
    pin_of(peer, pin);
    if (pin_out) snprintf(pin_out, 65, "%s", pin);
    bool vouched = g_have_cas && mbedtls_ssl_get_verify_result(&t->ssl) == 0;
    pthread_mutex_lock(&g_lock);
    bool have = kv_get(PINS_FILE, key, known, sizeof known);
    if (first) *first = !have;
    bool ok = true;
    if (!have || strcmp(known, pin)) {
        if (have && !vouched) ok = false;
        else kv_set(PINS_FILE, key, pin, false);  // (first use, or an authority vouches for the new key)
        if (have && ok) alog("%s has a new key, vouched for by a certificate authority: remembered", key);
    }
    pthread_mutex_unlock(&g_lock);
    if (!ok) {
        char pp[100];
        pin_pretty(pin, pp);
        alog("%s: the server's key changed (now %s) - refused", key, pp);
        snprintf(err, en, "The server's identity changed since this copy last talked to it. If its owner says they changed "
                          "it, use Forget the server's key, then sign in again.");
        g_last_tls = TLS_KEY_CHANGED;
        goto fail_quiet;
    }
    g_last_tls = TLS_OK;
    return true;
fail:
    snprintf(err, en, "Couldn't open a secure connection to %s", key);
fail_quiet:
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    closesock(t->s);
    return false;
}
static void tls_close(Tls *t) {
    mbedtls_ssl_close_notify(&t->ssl);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    closesock(t->s);
}
static bool tls_rw(Tls *t, void *p, size_t n, bool wr) {
    uint8_t *b = p;
    while (n) {
        int r = wr ? mbedtls_ssl_write(&t->ssl, b, n) : mbedtls_ssl_read(&t->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return false;
        b += r, n -= (size_t)r;
    }
    return true;
}
// one request: head (+ body) out, the reply's head (and body, malloc'd) back; false: the connection failed
static bool call(Tls *t, const char *head, const void *body, uint32_t bn, char *rh, size_t rhn, uint8_t **rb, uint32_t *rbn) {
    uint32_t h[2] = {(uint32_t)strlen(head), bn};
    if (g_log) {
        char shown[200];
        snprintf(shown, sizeof shown, "%s", head);
        char *sp = strchr(shown, ' ');
        if (sp && (!strncmp(head, "LOGIN", 5) || !strncmp(head, "CREATE", 6))) {  // (never the password)
            char *sp2 = strchr(sp + 1, ' ');
            if (sp2) snprintf(sp2, sizeof shown - (size_t)(sp2 - shown), " <password>");
        } else if (sp && strchr(sp, ':')) {  // (nor the token's secret)
            char *c = strchr(sp, ':');
            snprintf(c, sizeof shown - (size_t)(c - shown), ":<token>%s", strchr(c, ' ') ? strchr(c, ' ') : "");
        }
        alog("-> %s (%u bytes)", shown, bn);
    }
    if (!tls_rw(t, h, 8, true) || !tls_rw(t, (void *)head, h[0], true) || (bn && !tls_rw(t, (void *)body, bn, true))) return false;
    if (!tls_rw(t, h, 8, false) || h[0] >= rhn || h[0] == 0 || h[1] > BLOB_MAX) return false;
    if (!tls_rw(t, rh, h[0], false)) return false;
    rh[h[0]] = 0;
    uint8_t *b = h[1] ? malloc(h[1]) : NULL;
    if (h[1] && (!b || !tls_rw(t, b, h[1], false))) { free(b); return false; }
    if (g_log) alog("<- %.120s (%u bytes)", !strncmp(head, "JOIN", 4) && !strncmp(rh, "OK ", 3) ? "OK <play ticket>"  // (never its key)
                                          : strncmp(rh, "OK ", 3) || !strchr(rh, ':') ? rh : "OK <token>", h[1]);
    if (rb) *rb = b, *rbn = h[1];
    else free(b);
    return true;
}
static const char *err_text(const char *rh) {  // "ERR code text" -> text
    const char *s = strchr(rh, ' ');
    s = s ? strchr(s + 1, ' ') : NULL;
    if (s && !strncmp(rh, "ERR wait ", 9)) s = strchr(s + 1, ' ');  // (the seconds)
    return s ? s + 1 : rh;
}

// ------------------------------------------------------------------ the account
static bool token_of(const char *server, char *token, size_t tn, char *key, size_t kn) {
    char host[256];
    int port;
    if (!parse_server(server, host, sizeof host, &port, key, kn)) return false;
    pthread_mutex_lock(&g_lock);
    bool ok = kv_get(ACCOUNTS_FILE, key, token, tn);
    pthread_mutex_unlock(&g_lock);
    return ok && strchr(token, ':');
}
static volatile int g_ui_gen;  // (a sign-in / out finished: acct_signed_in reads the file again)
bool acct_signed_in(char *name, size_t n) {  // (the launcher asks every frame: the file read at most once a second)
    static char c_server[160], c_name[ACCT_NAME_MAX + 1];
    static bool c_in;
    static int c_gen = -1;
    static uint64_t c_at;
    uint64_t now = (uint64_t)time(NULL);
    if (c_gen != g_ui_gen || now != c_at || strcmp(c_server, g_settings.online_server)) {
        char token[ACCT_TOKEN_MAX + 8], key[300];
        c_in = token_of(g_settings.online_server, token, sizeof token, key, sizeof key);
        if (c_in) *strchr(token, ':') = 0, snprintf(c_name, sizeof c_name, "%s", token);
        snprintf(c_server, sizeof c_server, "%s", g_settings.online_server);
        c_gen = g_ui_gen, c_at = now;
    }
    if (c_in) snprintf(name, n, "%s", c_name);
    return c_in;
}

// The server says the session token is no good (signed out elsewhere, 180 days unused, the account's sessions reset):
// forgotten here too - the Online tab shows "signed out" with why (acct_session_ended), the save stays as it is
static volatile bool g_session_ended;
static void session_ended(const char *server) {
    char host[256], key[300];
    int port;
    if (!parse_server(server, host, sizeof host, &port, key, sizeof key)) return;
    pthread_mutex_lock(&g_lock);
    kv_set(ACCOUNTS_FILE, key, NULL, true);
    pthread_mutex_unlock(&g_lock);
    g_session_ended = true;
    g_ui_gen++;
    alog("the server ended this copy's sign-in: signed out here too");
}
bool acct_session_ended(void) { return g_session_ended; }

static int account_op(int op, const char *server, const char *name, const char *pass, char *msg, size_t mn, bool *key_changed) {
    char host[256], key[300];
    int port;
    *key_changed = false;
    if (!parse_server(server, host, sizeof host, &port, key, sizeof key)) { snprintf(msg, mn, "Set the online server's address first"); return 1; }
    if (op == ACCT_OP_FORGET_KEY) {
        pthread_mutex_lock(&g_lock);
        kv_set(PINS_FILE, key, NULL, false);
        pthread_mutex_unlock(&g_lock);
        snprintf(msg, mn, "Forgot %s's key: the next sign-in remembers its new one", key);
        alog("%s", msg);
        return 0;
    }
    char token[ACCT_TOKEN_MAX + 8], tkey[300];
    bool have_token = token_of(server, token, sizeof token, tkey, sizeof tkey);
    if (op == ACCT_OP_LOGOUT) {
        pthread_mutex_lock(&g_lock);
        kv_set(ACCOUNTS_FILE, key, NULL, true);  // (signed out here whatever the server says)
        pthread_mutex_unlock(&g_lock);
        if (!have_token) { snprintf(msg, mn, "Signed out"); return 0; }
        Tls t;
        char err[300], rh[ACCT_MAX_HEADER], head[200];
        if (tls_open(&t, server, err, sizeof err, NULL, NULL)) {
            snprintf(head, sizeof head, "LOGOUT %s", token);
            call(&t, head, NULL, 0, rh, sizeof rh, NULL, NULL);
            tls_close(&t);
        }
        snprintf(msg, mn, "Signed out");
        alog("signed out of %s", key);
        return 0;
    }
    // CREATE / LOGIN
    size_t pl = strlen(pass);
    if (pl < ACCT_PASS_MIN || pl > ACCT_PASS_MAX) { snprintf(msg, mn, "Passwords are %d-%d characters", ACCT_PASS_MIN, ACCT_PASS_MAX); return 1; }
    char lname[ACCT_NAME_MAX + 1];
    size_t nl = strlen(name);
    if (nl < ACCT_NAME_MIN || nl > ACCT_NAME_MAX) { snprintf(msg, mn, "Names are %d-%d letters, digits, _ . or -", ACCT_NAME_MIN, ACCT_NAME_MAX); return 1; }
    for (size_t i = 0; i <= nl; i++) {
        char ch = name[i];
        lname[i] = ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
        if (i < nl && !strchr("abcdefghijklmnopqrstuvwxyz0123456789_.-", lname[i])) {
            snprintf(msg, mn, "Names are %d-%d letters, digits, _ . or -", ACCT_NAME_MIN, ACCT_NAME_MAX);
            return 1;
        }
    }
    size_t hn = 64 + 2 * pl;
    char *head = malloc(hn);
    char ph[2 * ACCT_PASS_MAX + 1];
    hex((const uint8_t *)pass, pl, ph);
    snprintf(head, hn, "%s %s %s %s", op == ACCT_OP_CREATE ? "CREATE" : "LOGIN", lname, ph, ACCT_DEVICE);
    mbedtls_platform_zeroize(ph, sizeof ph);
    Tls t;
    char err[300], rh[ACCT_MAX_HEADER];
    bool first = false;
    char pin[65] = "";
    if (!tls_open(&t, server, err, sizeof err, &first, pin)) {
        mbedtls_platform_zeroize(head, hn);
        free(head);
        *key_changed = g_last_tls == TLS_KEY_CHANGED;
        snprintf(msg, mn, "%s", err);
        return 1;
    }
    bool ok = call(&t, head, NULL, 0, rh, sizeof rh, NULL, NULL);
    mbedtls_platform_zeroize(head, hn);
    free(head);
    tls_close(&t);
    if (!ok) { snprintf(msg, mn, "The server didn't answer"); return 1; }
    if (strncmp(rh, "OK ", 3) || !strchr(rh + 3, ':')) {
        snprintf(msg, mn, "%s", err_text(rh));
        alog("%s on %s: %s", op == ACCT_OP_CREATE ? "new account" : "sign-in", key, rh);
        return 1;
    }
    pthread_mutex_lock(&g_lock);
    bool saved = kv_set(ACCOUNTS_FILE, key, rh + 3, true);
    pthread_mutex_unlock(&g_lock);
    if (!saved) { snprintf(msg, mn, "Signed in, but couldn't write online/accounts.txt"); return 1; }
    g_session_ended = false;
    char pp[100] = "";
    if (first) pin_pretty(pin, pp);
    snprintf(msg, mn, "%s as %s on %s.%s%s", op == ACCT_OP_CREATE ? "Account made: signed in" : "Signed in", lname, key,
             first ? " First visit: remembered the server's key " : "", pp);
    alog("%s", msg);
    return 0;
}

// A play ticket (JOIN) for the account signed in on server: 1 got it, 0 not signed in there (or the server ended the
// sign-in), -1 couldn't ask (offline, ...)
static int ticket_get(const char *server, uint8_t id[8], uint8_t key[16]) {
    char token[ACCT_TOKEN_MAX + 8], tkey[300], err[300], rh[ACCT_MAX_HEADER], head[200];
    if (!token_of(server, token, sizeof token, tkey, sizeof tkey)) return 0;
    Tls t;
    if (!tls_open(&t, server, err, sizeof err, NULL, NULL)) return -1;
    snprintf(head, sizeof head, "JOIN %s", token);
    bool ok = call(&t, head, NULL, 0, rh, sizeof rh, NULL, NULL);
    tls_close(&t);
    mbedtls_platform_zeroize(head, sizeof head);
    if (!ok) return -1;
    if (!strncmp(rh, "ERR signin", 10)) { session_ended(server); return 0; }
    char ih[20], kh[40];
    int secs = 0;
    int r = sscanf(rh, "OK %19s %39s %d", ih, kh, &secs) == 3 && strlen(ih) == 16 && strlen(kh) == 32 ? 1 : -1;
    for (int i = 0; r == 1 && i < 8; i++) { unsigned v; sscanf(ih + 2 * i, "%2x", &v); id[i] = (uint8_t)v; }
    for (int i = 0; r == 1 && i < 16; i++) { unsigned v; sscanf(kh + 2 * i, "%2x", &v); key[i] = (uint8_t)v; }
    mbedtls_platform_zeroize(rh, sizeof rh);
    mbedtls_platform_zeroize(kh, sizeof kh);
    if (r != 1) alog("play ticket: an odd answer");
    return r;
}

static void link_save(char *msg, size_t mn);  // (below: the save sync after a sign-in)
// the launcher's buttons, on a thread of their own
static pthread_mutex_t g_ui_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_ui_busy;
static char g_ui_msg[1800];
static bool g_ui_err, g_ui_keychg;
typedef struct { int op; char server[160], name[64], pass[ACCT_PASS_MAX + 1]; } UiJob;
static void *ui_thread(void *arg) {
    UiJob *j = arg;
    char msg[1800];
    bool kc;
    int r = 0;
    kc = false;
    if (j->op == ACCT_OP_SYNC) link_save(msg, sizeof msg);
    else {
        r = account_op(j->op, j->server, j->name, j->pass, msg, sizeof msg, &kc);
        if (!r && (j->op == ACCT_OP_LOGIN || j->op == ACCT_OP_CREATE)) {  // (signed in: this PC's save linked to it now)
            char m2[1200];
            link_save(m2, sizeof m2);
            size_t l = strlen(msg);
            snprintf(msg + l, sizeof msg - l, "\n%s", m2);
        }
    }
    mbedtls_platform_zeroize(j->pass, sizeof j->pass);
    free(j);
    pthread_mutex_lock(&g_ui_lock);
    snprintf(g_ui_msg, sizeof g_ui_msg, "%s", msg);
    g_ui_err = r != 0, g_ui_keychg = kc;
    g_ui_gen++;
    g_ui_busy = false;
    pthread_mutex_unlock(&g_ui_lock);
    return NULL;
}
void acct_ui_start(int op, const char *name, const char *password) {
    if (g_ui_busy) return;
    UiJob *j = calloc(1, sizeof *j);
    j->op = op;
    snprintf(j->server, sizeof j->server, "%s", g_settings.online_server);
    snprintf(j->name, sizeof j->name, "%s", name ? name : "");
    snprintf(j->pass, sizeof j->pass, "%s", password ? password : "");
    g_ui_busy = true;
    pthread_t th;
    if (pthread_create(&th, NULL, ui_thread, j)) {
        g_ui_busy = false;
        free(j);
        return;
    }
    pthread_detach(th);
}
bool acct_ui_busy(void) { return g_ui_busy; }
const char *acct_ui_message(bool *error) {
    static char copy[1800];
    pthread_mutex_lock(&g_ui_lock);
    snprintf(copy, sizeof copy, "%s", g_ui_msg);
    if (error) *error = g_ui_err;
    pthread_mutex_unlock(&g_ui_lock);
    return copy;
}
bool acct_ui_key_changed(void) { return g_ui_keychg; }
const char *acct_last_sync(void) {
    static char line[700];
    static uint64_t when;
    uint64_t now = (uint64_t)time(NULL);
    if (now != when) {  // (read at most once a second: the launcher draws every frame)
        when = now;
        size_t n;
        char *s = read_all(LAST_FILE, &n, 4096);
        snprintf(line, sizeof line, "%s", s ? s : "");
        line[strcspn(line, "\n")] = 0;
        free(s);
    }
    return line;
}

// ------------------------------------------------------------------ the save: files <-> a blob
typedef struct { char path[128]; uint8_t *data; uint64_t size; int64_t mtime; } SFile;
typedef struct { SFile f[SAVE_FILES_MAX]; int n; int64_t newest; char chash[65]; } Snap;
static void snap_free(Snap *s) {
    for (int i = 0; i < s->n; i++) free(s->f[i].data);
    s->n = 0;
}
static int sfile_cmp(const void *a, const void *b) { return strcmp(((const SFile *)a)->path, ((const SFile *)b)->path); }
static bool snap_dir(Snap *s, const char *root, const char *rel, int depth) {
    char p[1024];
    snprintf(p, sizeof p, "%s%s%s", root, rel[0] ? "/" : "", rel);
    DIR *d = opendir(p);
    if (!d) return depth == 0;  // (no save yet: an empty snapshot)
    bool ok = true;
    for (struct dirent *e; ok && (e = readdir(d));) {
        if (e->d_name[0] == '.' || strstr(e->d_name, ".tmp")) continue;
        char r2[300], full[1400];
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", root, r2);
        struct stat sb;
        if (stat(full, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) {
            if (depth < 4) ok = snap_dir(s, root, r2, depth + 1);
            continue;
        }
        if (s->n >= SAVE_FILES_MAX || strlen(r2) >= sizeof s->f[0].path) { ok = false; break; }
        SFile *f = &s->f[s->n];
        size_t n = 0;
        f->data = (uint8_t *)read_all(full, &n, BLOB_MAX);
        if (!f->data) { ok = false; break; }
        snprintf(f->path, sizeof f->path, "%s", r2);
        f->size = n, f->mtime = (int64_t)sb.st_mtime;
        if (f->mtime > s->newest) s->newest = f->mtime;
        s->n++;
    }
    closedir(d);
    return ok;
}
static void snap_hash(Snap *s) {  // (content only: paths, sizes, bytes - not times)
    qsort(s->f, (size_t)s->n, sizeof s->f[0], sfile_cmp);
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    for (int i = 0; i < s->n; i++) {
        mbedtls_sha256_update(&c, (const unsigned char *)s->f[i].path, strlen(s->f[i].path) + 1);
        uint8_t sz[8];
        for (int k = 0; k < 8; k++) sz[k] = (uint8_t)(s->f[i].size >> (8 * k));
        mbedtls_sha256_update(&c, sz, 8);
        mbedtls_sha256_update(&c, s->f[i].data, s->f[i].size);
    }
    uint8_t h[32];
    mbedtls_sha256_finish(&c, h);
    mbedtls_sha256_free(&c);
    if (s->n) hex(h, 32, s->chash);
    else snprintf(s->chash, sizeof s->chash, "none");
}
static bool snap_take(Snap *s, const char *dir) {
    memset(s, 0, sizeof *s);
    bool ok = snap_dir(s, dir, "", 0);
    if (!ok) snap_free(s);
    else snap_hash(s);
    return ok;
}
// blob = "LZSV" u32 1, u32 raw size, zlib(raw); raw = u32 n, n x {u16 path len, path, u64 size, i64 mtime}, the bytes
static void put(uint8_t **p, const void *v, size_t n) { memcpy(*p, v, n), *p += n; }
static uint8_t *pack(const Snap *s, uint32_t *out_n) {
    size_t raw_n = 4;
    for (int i = 0; i < s->n; i++) raw_n += 2 + strlen(s->f[i].path) + 16 + s->f[i].size;
    if (raw_n > BLOB_MAX) return NULL;
    uint8_t *raw = malloc(raw_n), *p = raw;
    uint32_t n = (uint32_t)s->n;
    put(&p, &n, 4);
    for (int i = 0; i < s->n; i++) {
        uint16_t l = (uint16_t)strlen(s->f[i].path);
        put(&p, &l, 2), put(&p, s->f[i].path, l), put(&p, &s->f[i].size, 8), put(&p, &s->f[i].mtime, 8);
    }
    for (int i = 0; i < s->n; i++) put(&p, s->f[i].data, s->f[i].size);
    mz_ulong cn = mz_compressBound((mz_ulong)raw_n);
    uint8_t *blob = malloc(12 + cn);
    memcpy(blob, ACCT_BLOB_MAGIC, 4);
    uint32_t v = 1, rn = (uint32_t)raw_n;
    memcpy(blob + 4, &v, 4), memcpy(blob + 8, &rn, 4);
    int r = mz_compress2(blob + 12, &cn, raw, (mz_ulong)raw_n, 1);
    free(raw);
    if (r != MZ_OK) { free(blob); return NULL; }
    *out_n = (uint32_t)(12 + cn);
    return blob;
}
static bool safe_rel(const char *p) {  // (a path from a blob: relative, plain names, no "..")
    if (!*p || *p == '/' || strstr(p, "..") || strchr(p, '\\') || strchr(p, ':')) return false;
    for (; *p; p++)
        if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-/", *p)) return false;
    return true;
}
static bool unpack(const uint8_t *blob, uint32_t bn, Snap *s) {
    memset(s, 0, sizeof *s);
    uint32_t v, rn;
    if (bn < 12 || memcmp(blob, ACCT_BLOB_MAGIC, 4)) return false;
    memcpy(&v, blob + 4, 4), memcpy(&rn, blob + 8, 4);
    if (v != 1 || rn > BLOB_MAX || rn < 4) return false;
    uint8_t *raw = malloc(rn);
    mz_ulong got = rn;
    if (!raw || mz_uncompress(raw, &got, blob + 12, bn - 12) != MZ_OK || got != rn) { free(raw); return false; }
    const uint8_t *p = raw, *end = raw + rn;
    uint32_t n;
    memcpy(&n, p, 4), p += 4;
    bool ok = n <= SAVE_FILES_MAX;
    for (uint32_t i = 0; ok && i < n; i++) {
        uint16_t l;
        if (end - p < 2) { ok = false; break; }
        memcpy(&l, p, 2), p += 2;
        if (l >= sizeof s->f[0].path || end - p < l + 16) { ok = false; break; }
        SFile *f = &s->f[s->n++];
        memcpy(f->path, p, l), f->path[l] = 0, p += l;
        memcpy(&f->size, p, 8), memcpy(&f->mtime, p + 8, 8), p += 16;
        ok = safe_rel(f->path);
    }
    for (int i = 0; ok && i < s->n; i++) {
        SFile *f = &s->f[i];
        if ((uint64_t)(end - p) < f->size) { ok = false; break; }
        f->data = malloc(f->size ? f->size : 1);
        memcpy(f->data, p, f->size), p += f->size;
        if (f->mtime > s->newest) s->newest = f->mtime;
    }
    free(raw);
    if (!ok || p != end) { snap_free(s); return false; }
    snap_hash(s);
    return true;
}

// ------------------------------------------------------------------ sync state
typedef struct {
    char server[300], token[ACCT_TOKEN_MAX + 8], user[ACCT_NAME_MAX + 1], game[17];
    char dir[512];      // the save folder (<save dir>/save_0)
    char state[600];    // online/sync/<key>.txt
    long long version;  // the server's version this copy last matched
    long long counter;  // this copy's uploads
    char chash[65];     // the content then
} Sync;
typedef struct { long long version, counter, saved, size, uploaded; char device[32], chash[65], bsha[65]; } Head;
static bool head_parse(const char *s, Head *h) {
    memset(h, 0, sizeof *h);
    return sscanf(s, "%lld %lld %lld %31s %64s %64s %lld %lld", &h->version, &h->counter, &h->saved, h->device, h->chash, h->bsha,
                  &h->size, &h->uploaded) == 8;
}
static void state_load(Sync *y) {
    size_t n;
    char *s = read_all(y->state, &n, 4096);
    y->version = 0, y->counter = 0;
    snprintf(y->chash, sizeof y->chash, "-");
    if (s) sscanf(s, "version %lld counter %lld hash %64s", &y->version, &y->counter, y->chash);
    free(s);
}
static void state_save(const Sync *y) {
    char b[300];
    int n = snprintf(b, sizeof b, "version %lld counter %lld hash %s\n", y->version, y->counter, y->chash);
    mkdirs("online/sync");
    write_atomic(y->state, b, (size_t)n, false);
}
static bool sync_setup(Sync *y, uint64_t program_id, const char *save_root) {
    memset(y, 0, sizeof *y);
    if (!g_settings.sync_saves) return false;
    snprintf(y->server, sizeof y->server, "%s", g_settings.online_server);
    char key[300];
    if (!token_of(y->server, y->token, sizeof y->token, key, sizeof key)) return false;
    snprintf(y->server, sizeof y->server, "%s", key);
    snprintf(y->user, sizeof y->user, "%.*s", (int)(strchr(y->token, ':') - y->token), y->token);
    snprintf(y->game, sizeof y->game, "%016llx", (unsigned long long)program_id);
    snprintf(y->dir, sizeof y->dir, "%s/save_0", save_root);
    char id[900];
    snprintf(id, sizeof id, "%s|%s|%s|%s", y->server, y->user, y->game, save_root);
    uint8_t h[32];
    char hs[65];
    mbedtls_sha256((const unsigned char *)id, strlen(id), h, 0);
    hex(h, 8, hs);
    snprintf(y->state, sizeof y->state, "online/sync/%s.txt", hs);
    state_load(y);
    return true;
}
static void stamp(char *out, size_t n) {
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(out, n, "%Y%m%d-%H%M%S", &tmv);
}
static void prune_backups(const char *bdir) {  // (the oldest beyond BACKUPS_KEPT; names sort by time)
    DIR *d = opendir(bdir);
    if (!d) return;
    char names[256][64];
    int n = 0;
    for (struct dirent *e; (e = readdir(d)) && n < 256;)
        if (e->d_name[0] != '.' && strlen(e->d_name) < 64) snprintf(names[n++], 64, "%s", e->d_name);
    closedir(d);
    qsort(names, (size_t)n, 64, (int (*)(const void *, const void *))strcmp);
    for (int i = 0; i < n - BACKUPS_KEPT; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", bdir, names[i]);
        plat_remove_tree(p);
    }
}
// the save folder moved aside as a backup (why: before-download / conflict-...): its path in out
static bool backup_move(const Sync *y, const char *why, char *out, size_t on) {
    char root[512], st[32];
    snprintf(root, sizeof root, "%s", y->dir);
    char *slash = strrchr(root, '/');
    if (slash) *slash = 0;
    stamp(st, sizeof st);
    char bdir[700];
    snprintf(bdir, sizeof bdir, "%s/sync-backups", root);
    snprintf(out, on, "%s/%s-%s", bdir, st, why);
    mkdirs(out);
    char dst[1000];
    snprintf(dst, sizeof dst, "%s/save_0", out);
    bool ok = !rename(y->dir, dst);
    prune_backups(bdir);
    return ok;
}
static bool snap_write(const Snap *s, const char *dir) {
    mkdirs(dir);
    for (int i = 0; i < s->n; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", dir, s->f[i].path);
        char *sl = strrchr(p, '/');
        *sl = 0, mkdirs(p), *sl = '/';
        FILE *f = fopen(p, "wb");
        if (!f) return false;
        bool ok = fwrite(s->f[i].data, 1, s->f[i].size, f) == s->f[i].size;
        if (fclose(f) || !ok) return false;
#ifndef __PROSPERO__
        struct utimbuf ut = {(time_t)s->f[i].mtime, (time_t)s->f[i].mtime};  // (its own time: "the newer save" compares them)
        utime(p, &ut);
#endif
    }
    return true;
}
// a snapshot written as a backup folder, the save itself untouched
static bool backup_copy(const Sync *y, const Snap *s, const char *why, char *out, size_t on) {
    char root[512], st[32];
    snprintf(root, sizeof root, "%s", y->dir);
    char *slash = strrchr(root, '/');
    if (slash) *slash = 0;
    stamp(st, sizeof st);
    snprintf(out, on, "%s/sync-backups/%s-%s", root, st, why);
    char d[900];
    snprintf(d, sizeof d, "%s/save_0", out);
    return snap_write(s, d);
}
// the server's save in place of ours: written next to it, ours moved aside (a backup, unless there was none), swapped
static bool install(Sync *y, const Snap *s, const char *why, char *bak, size_t bn) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.sync-new", y->dir);
    plat_remove_tree(tmp);
    if (!snap_write(s, tmp)) { plat_remove_tree(tmp); return false; }
    struct stat sb;
    bak[0] = 0;
    if (!stat(y->dir, &sb) && !backup_move(y, why, bak, bn)) { plat_remove_tree(tmp); return false; }
    if (rename(tmp, y->dir)) return false;
    return true;
}

static bool open_with_token(Tls *t, Sync *y, char *err, size_t en) { return tls_open(t, y->server, err, en, NULL, NULL); }
static bool fetch(Tls *t, Sync *y, const Head *h, Snap *out, char *err, size_t en) {
    char head[300], rh[ACCT_MAX_HEADER];
    uint8_t *b = NULL;
    uint32_t bn = 0;
    snprintf(head, sizeof head, "GET %s %s %lld", y->token, y->game, h->version);
    if (!call(t, head, NULL, 0, rh, sizeof rh, &b, &bn)) { snprintf(err, en, "the server didn't answer"); return false; }
    uint8_t d[32];
    char ds[65];
    if (b) mbedtls_sha256(b, bn, d, 0);
    hex(d, 32, ds);
    bool ok = !strncmp(rh, "OK ", 3) && b && !strcmp(ds, h->bsha) && unpack(b, bn, out) && !strcmp(out->chash, h->chash);
    free(b);
    if (!ok) snprintf(err, en, "%s", strncmp(rh, "OK ", 3) ? err_text(rh) : "the save from the server is damaged");
    return ok;
}
// 1: uploaded, 0: refused as a conflict (*h: the server's latest), -1: failed
static int upload(Tls *t, Sync *y, const Snap *s, Head *h, char *err, size_t en) {
    uint32_t bn = 0;
    uint8_t *blob = pack(s, &bn);
    if (!blob) { snprintf(err, en, "the save is too big"); return -1; }
    uint8_t d[32];
    char ds[65], head[500], rh[ACCT_MAX_HEADER];
    mbedtls_sha256(blob, bn, d, 0);
    hex(d, 32, ds);
    snprintf(head, sizeof head, "PUT %s %s %lld %lld %lld %s %s %s", y->token, y->game, y->version, y->counter + 1,
             (long long)s->newest, ACCT_DEVICE, s->chash, ds);
    bool ok = call(t, head, blob, bn, rh, sizeof rh, NULL, NULL);
    free(blob);
    if (!ok) { snprintf(err, en, "the server didn't answer"); return -1; }
    if (!strncmp(rh, "CONFLICT ", 9)) {
        head_parse(rh + 9, h);
        if (strcmp(h->chash, s->chash)) return 0;
        // (the server's latest is this very save - another start of this copy sent it first): in step
        y->version = h->version;
        snprintf(y->chash, sizeof y->chash, "%s", s->chash);
        state_save(y);
        return 1;
    }
    if (!strncmp(rh, "ERR signin", 10)) { snprintf(err, en, "the server ended this sign-in"); return -2; }
    if (strncmp(rh, "OK ", 3) || !head_parse(rh + 3, h)) { snprintf(err, en, "%s", err_text(rh)); return -1; }
    y->version = h->version, y->counter++;
    snprintf(y->chash, sizeof y->chash, "%s", s->chash);
    state_save(y);
    return 1;
}
static const char *when(long long t, char *b, size_t n) {
    time_t tt = (time_t)t;
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    strftime(b, n, "%Y-%m-%d %H:%M:%S", &tmv);
    return b;
}

// The sync at boot / after a sign-in (see the top). Returns 0 in step / done, 1 failed, 2 the server couldn't be
// reached (offline: the game plays with the save it has, synced when the server answers again)
static int sync_now(Sync *y, char *msg, size_t mn) {
    Snap local;
    if (!snap_take(&local, y->dir)) { snprintf(msg, mn, "Save sync: couldn't read the save in %s", y->dir); return 1; }
    Tls t;
    char err[400], rh[ACCT_MAX_HEADER], head[300], w1[32], w2[32], bak[900];
    if (!open_with_token(&t, y, err, sizeof err)) {
        snap_free(&local);
        bool keychg = g_last_tls == TLS_KEY_CHANGED;
        snprintf(msg, mn, keychg ? "Save sync skipped: %s" : "Save sync: offline (%s) - the save here is used and synced when the server answers", err);
        return keychg ? 1 : 2;
    }
    snprintf(head, sizeof head, "INFO %s %s", y->token, y->game);
    int ret = 1;
    Head h;
    if (!call(&t, head, NULL, 0, rh, sizeof rh, NULL, NULL)) { snprintf(msg, mn, "Save sync skipped: the server didn't answer"); goto out; }
    if (!strncmp(rh, "ERR signin", 10)) {
        session_ended(y->server);
        snprintf(msg, mn, "Save sync: the server ended this sign-in - sign in again (launcher > Options > Online). The save here is untouched.");
        goto out;
    }
    if (strncmp(rh, "OK ", 3)) { snprintf(msg, mn, "Save sync: %s", err_text(rh)); goto out; }
    bool have = strcmp(rh, "OK 0") && head_parse(rh + 3, &h);
    bool local_changed = strcmp(local.chash, y->chash) != 0;
    if (!have) {  // (nothing on the server yet)
        if (!local.n) { snprintf(msg, mn, "Save sync: no save here or on the server yet"); ret = 0; goto out; }
        int r = upload(&t, y, &local, &h, err, sizeof err);
        if (r == 1) snprintf(msg, mn, "Save sync: linked - your save went up to the server (the first one there)"), ret = 0;
        else snprintf(msg, mn, "Save sync: couldn't upload: %s", r ? err : "the server changed meanwhile");
        goto out;
    }
    if (!strcmp(h.chash, local.chash)) {  // (the same save on both sides)
        y->version = h.version;
        snprintf(y->chash, sizeof y->chash, "%s", local.chash);
        state_save(y);
        snprintf(msg, mn, "Save sync: in step with the server (save %lld)", h.version);
        ret = 0;
        goto out;
    }
    bool server_changed = h.version != y->version;
    if (!server_changed) {  // (only ours changed: up)
        int r = upload(&t, y, &local, &h, err, sizeof err);
        if (r == 1) snprintf(msg, mn, "Save sync: your save went up to the server (save %lld)", h.version), ret = 0;
        else snprintf(msg, mn, "Save sync: couldn't upload: %s", r ? err : "the server changed meanwhile - next start");
        goto out;
    }
    if (!local.n || !local_changed) {  // (only the server's changed: down, ours kept as a backup)
        Snap srv;
        if (!fetch(&t, y, &h, &srv, err, sizeof err)) { snprintf(msg, mn, "Save sync: couldn't download: %s", err); goto out; }
        if (install(y, &srv, "before-download", bak, sizeof bak)) {
            y->version = h.version;
            snprintf(y->chash, sizeof y->chash, "%s", srv.chash);
            state_save(y);
            snprintf(msg, mn, "Save sync: loaded the save from %s (%s, save %lld)%s%s", h.device, when(h.saved, w1, sizeof w1), h.version,
                     bak[0] ? "; the one here was kept in " : "", bak);
            ret = 0;
        } else snprintf(msg, mn, "Save sync: couldn't put the server's save in place (%s)", y->dir);
        snap_free(&srv);
        goto out;
    }
    // both changed since the last sync (or this copy never synced and both have a save): a conflict
    {
        Snap srv;
        if (!fetch(&t, y, &h, &srv, err, sizeof err)) { snprintf(msg, mn, "Save sync: conflict, and couldn't download: %s", err); goto out; }
        bool server_newer = h.saved > local.newest;
        // (never synced here: not a real conflict, but the same care - both saves kept)
        const char *what = y->version ? "Save sync CONFLICT: the save here and the server's both changed."
                                      : "Save sync, first time on this copy: it and the server both have a save.";
        if (server_newer) {
            if (install(y, &srv, "conflict-this-copy", bak, sizeof bak)) {
                y->version = h.version;
                snprintf(y->chash, sizeof y->chash, "%s", srv.chash);
                state_save(y);
                snprintf(msg, mn, "%s Using the newer one, from %s (%s); this copy's (%s) is kept in %s", what, h.device,
                         when(h.saved, w1, sizeof w1),
                         when(local.newest, w2, sizeof w2), bak);
                ret = 0;
            } else snprintf(msg, mn, "Save sync CONFLICT, and couldn't put the server's save in place - nothing changed");
        } else {
            if (!backup_copy(y, &srv, "conflict-server", bak, sizeof bak)) {
                snprintf(msg, mn, "Save sync CONFLICT: couldn't keep the server's save as a backup - nothing changed");
            } else {
                y->version = h.version;  // (ours replaces the server's latest; the server keeps that one too)
                Head was = h, now_h;
                int r = upload(&t, y, &local, &now_h, err, sizeof err);
                h = was;
                snprintf(msg, mn, "%s Using the newer one, this copy's (%s)%s; the server's, from %s (%s), is kept in %s", what,
                         when(local.newest, w1, sizeof w1),
                         r == 1 ? " - uploaded" : " - not uploaded yet", h.device, when(h.saved, w2, sizeof w2), bak);
                ret = 0;
            }
        }
        snap_free(&srv);
    }
out:
    tls_close(&t);
    snap_free(&local);
    return ret;
}

// ------------------------------------------------------------------ the game: boot, commits, exit
static Sync g_sync;
static bool g_sync_on;
static pthread_mutex_t g_up_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_up_cond = PTHREAD_COND_INITIALIZER;
static Snap *g_pending;      // (the latest committed save not uploaded yet)
static uint64_t g_pending_at;  // (ms: when it was committed; 0 = at once)
static Snap *g_retry;        // (offline: the latest save that couldn't go up - again when the server answers)
static uint64_t g_retry_at;  // (ms: the last try)
static bool g_uploading, g_exiting;
static uint64_t g_upload_delay = 2000;  // (ms after the last commit; LARECOMP_ACCT_UPLOAD_DELAY_MS: tests of the upload at exit)
#define RETRY_MS 30000

static uint64_t ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void snap_drop(Snap *s) {
    if (s) snap_free(s), free(s);
}
static void retry_keep(Snap *s) {  // (under g_up_lock: s is the newest that couldn't go up - an older one is dropped)
    snap_drop(g_retry);
    g_retry = s, g_retry_at = ms_now();
}
static void *uploader(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_up_lock);
    for (;;) {
        while (!g_pending) pthread_cond_wait(&g_up_cond, &g_up_lock);
        // (a moment after the last commit: the game commits each file of a save)
        while (!g_exiting && ms_now() - g_pending_at < g_upload_delay) {
            pthread_mutex_unlock(&g_up_lock);
            struct timespec d = {0, 200 * 1000000};
            nanosleep(&d, NULL);
            pthread_mutex_lock(&g_up_lock);
        }
        Snap *s = g_pending;
        g_pending = NULL;
        g_uploading = true;
        bool was_retry = g_pending_at == 0;
        pthread_mutex_unlock(&g_up_lock);
        snap_hash(s);
        pthread_mutex_lock(&g_lock);
        bool changed = strcmp(s->chash, g_sync.chash) != 0;
        pthread_mutex_unlock(&g_lock);
        Snap *keep = NULL;  // (couldn't go up: tried again later)
        if (changed) {
            Tls t;
            char err[400];
            Head h;
            if (!open_with_token(&t, &g_sync, err, sizeof err)) {
                // (offline: no error shown - the save here is the truth; it goes up when the server answers)
                set_last("Save sync: offline - this save stays here and goes up when the server answers (%s)", err);
                keep = s, s = NULL;
            } else {
                int r = upload(&t, &g_sync, s, &h, err, sizeof err);
                tls_close(&t);
                if (r == 1) set_last("Save sync: your save went up to the server (save %lld)%s", h.version, was_retry ? " - caught up after being offline" : "");
                else if (r == 0) {
                    set_last("Save sync: the server has a newer save from %s - this one stays here, nothing is "
                             "overwritten; sorted out at the next start", h.device);
                    menu_notice_for("Save sync: the server has a newer save from another device - "
                                    "sorted out at the next start (nothing overwritten)", 8000);
                } else if (r == -2) {
                    session_ended(g_sync.server);
                    set_last("Save sync: the server ended this sign-in - the save stays here; sign in again (launcher > Options > Online)");
                } else set_last("Save sync: the save couldn't go up (%s) - trying again", err), keep = s, s = NULL;
            }
        }
        snap_drop(s);
        pthread_mutex_lock(&g_up_lock);
        if (keep) {
            if (!g_pending) retry_keep(keep);  // (a newer commit meanwhile replaces it)
            else snap_drop(keep);
        } else if (changed && !g_pending) snap_drop(g_retry), g_retry = NULL;
        g_uploading = false;
        pthread_cond_broadcast(&g_up_cond);
    }
    return NULL;
}
static bool retry_now(bool force) {  // (the save that couldn't go up, queued again: when RETRY_MS passed, or force)
    pthread_mutex_lock(&g_up_lock);
    bool go = g_retry && !g_pending && !g_uploading && (force || ms_now() - g_retry_at >= RETRY_MS);
    if (go) {
        g_pending = g_retry, g_retry = NULL, g_pending_at = 0;
        pthread_cond_broadcast(&g_up_cond);
    }
    pthread_mutex_unlock(&g_up_lock);
    return go;
}
void acct_exit(void) {  // (the last save's upload, waited for: 20 s at most)
    static bool done;
    if (!g_sync_on || done) return;
    done = true;
    pthread_mutex_lock(&g_up_lock);
    uint64_t last_try = g_retry_at;
    pthread_mutex_unlock(&g_up_lock);
    if (ms_now() - last_try >= 10000) retry_now(true);  // (offline until now: once more - unless it just failed)
    pthread_mutex_lock(&g_up_lock);
    g_exiting = true;
    pthread_cond_broadcast(&g_up_cond);
    uint64_t until = ms_now() + 20000;
    bool had = g_pending || g_uploading;
    if (had) alog("waiting for the save's upload before quitting");
    while ((g_pending || g_uploading) && ms_now() < until) {
        pthread_mutex_unlock(&g_up_lock);
        struct timespec d = {0, 50 * 1000000};
        nanosleep(&d, NULL);
        pthread_mutex_lock(&g_up_lock);
    }
    bool left = g_pending || g_uploading || g_retry;
    pthread_mutex_unlock(&g_up_lock);
    if (had || left) alog("quitting: %s", left ? "the save isn't up yet (offline) - it goes up at the next start" : "the save is up");
}

// While the game runs (thread): the play ticket for the relay plugin, and the save that couldn't go up tried again
static void *online_thread(void *arg) {
    (void)arg;
    void *plugins_symbol(const char *name);
    void (*set_ticket)(const uint8_t *, const uint8_t *) = NULL;
    int (*auth)(void) = NULL, (*state)(char *, size_t) = NULL;
    bool gave = false;
    uint64_t next = 0, backoff = 5000, started = ms_now();
    int was_state = 0;
    for (;;) {
        struct timespec d = {1, 0};
        nanosleep(&d, NULL);
        uint64_t now = ms_now();
        if (!set_ticket && now - started < 60000) {  // (the relay plugin: loaded before the game starts)
            set_ticket = (void (*)(const uint8_t *, const uint8_t *))plugins_symbol("larecomp_server_ticket");
            auth = (int (*)(void))plugins_symbol("larecomp_server_auth");
            state = (int (*)(char *, size_t))plugins_symbol("larecomp_server_state");
            if (!auth || !state) set_ticket = NULL;
        }
        if (set_ticket && state(NULL, 0)) {
            int st = state(NULL, 0);
            // (the server answers again after it couldn't be reached: a ticket at once - the backoff that built up while
            // it was away (up to 2 min) kept a signed-in player out after it came back, and the server's "sign in"
            // answer meanwhile had the game tell them to sign in)
            if (st == 2 && was_state && was_state != 2) next = 0, backoff = 5000;
            char tok[ACCT_TOKEN_MAX + 8], key[300];
            bool in = token_of(g_settings.online_server, tok, sizeof tok, key, sizeof key);
            mbedtls_platform_zeroize(tok, sizeof tok);
            if (!in && gave) set_ticket(NULL, NULL), gave = false, alog("play ticket dropped (signed out)");
            if (in && (!gave || auth() == 3) && now >= next) {
                uint8_t id[8], k[16];
                int r = ticket_get(g_settings.online_server, id, k);
                if (r == 1) {
                    set_ticket(id, k), gave = true, backoff = 5000, next = now + 10000;
                    alog("play ticket: got one for the server (online play as the signed-in account)");
                } else if (r == 0) {
                    if (gave) set_ticket(NULL, NULL);
                    gave = false, next = now + 60000;
                } else next = now + backoff, backoff = backoff * 2 > 120000 ? 120000 : backoff * 2;
                mbedtls_platform_zeroize(k, sizeof k);
            }
            if (g_sync_on && st == 2 && was_state != 2 && was_state) retry_now(true);  // (the server answers again: catch up now)
            was_state = st;
        }
        if (g_sync_on) retry_now(false);
    }
    return NULL;
}

static const char *save_root(void) {
    const char *e = getenv("LARECOMP_SAVE_DIR");  // (as services/fs.c)
    return e ? e : g_save_dir;
}
void acct_boot_sync(uint64_t program_id) {
    static bool online_started;
    if (!online_started) {  // (tickets: whether or not saves sync)
        online_started = true;
        pthread_t th;
        if (!pthread_create(&th, NULL, online_thread, NULL)) pthread_detach(th);
    }
    if (g_sync_on) return;  // (a reboot to the title: once per run)
    if (!sync_setup(&g_sync, program_id, save_root())) return;
    char msg[1200];
    uint64_t t0 = ms_now();
    int r = sync_now(&g_sync, msg, sizeof msg);
    set_last("%s", msg);
    alog("(%llu ms)", (unsigned long long)(ms_now() - t0));
    if (strstr(msg, "CONFLICT") || strstr(msg, "first time") || strstr(msg, "loaded the save"))
        menu_notice_for(msg, 30000);
    if (r == 2) {  // (offline: a save changed since the last sync goes up once the server answers - the game hasn't run yet)
        Snap *s = malloc(sizeof *s);
        if (snap_take(s, g_sync.dir) && s->n && strcmp(s->chash, g_sync.chash)) {
            pthread_mutex_lock(&g_up_lock);
            retry_keep(s);
            pthread_mutex_unlock(&g_up_lock);
            alog("offline at start: this save goes up when the server answers");
        } else snap_free(s), free(s);
    }
    g_sync_on = true;
    if (getenv("LARECOMP_ACCT_UPLOAD_DELAY_MS")) g_upload_delay = strtoull(getenv("LARECOMP_ACCT_UPLOAD_DELAY_MS"), NULL, 10);
    pthread_t th;
    if (!pthread_create(&th, NULL, uploader, NULL)) pthread_detach(th);
    atexit(acct_exit);
}
void acct_save_committed(const char *fs_root) {
    if (!g_sync_on || strcmp(fs_root, g_sync.dir)) return;
    Snap *s = malloc(sizeof *s);
    memset(s, 0, sizeof *s);
    if (!snap_dir(s, g_sync.dir, "", 0)) {  // (read now: the files as committed)
        snap_free(s);
        free(s);
        return;
    }
    pthread_mutex_lock(&g_up_lock);
    if (g_pending) snap_free(g_pending), free(g_pending);
    g_pending = s;
    g_pending_at = ms_now();
    if (g_retry) snap_drop(g_retry), g_retry = NULL;  // (this newer one replaces what couldn't go up)
    pthread_cond_broadcast(&g_up_cond);
    pthread_mutex_unlock(&g_up_lock);
    if (g_log) alog("save committed (%d files)", s->n);
}

// ------------------------------------------------------------------ lasrecomp --account ...
static uint64_t program_id_of(const char *root) {  // (main.npdm's ACI0, as main.c boot)
    char p[1100];
    snprintf(p, sizeof p, "%s/exefs/main.npdm", root);
    FILE *f = fopen(p, "rb");
    uint64_t id = 0;
    uint8_t meta[0x80], aci[0x18];
    if (f && fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4) &&
        !fseek(f, *(uint32_t *)(meta + 0x70), SEEK_SET) && fread(aci, 1, sizeof aci, f) == sizeof aci && !memcmp(aci, "ACI0", 4))
        id = *(uint64_t *)(aci + 0x10);
    if (f) fclose(f);
    return id;
}
static char g_game_root[512] = "extracted";
static uint64_t g_cli_pid;  // (lasrecomp --account ... --program-id: the game without its files here, tests)
void acct_set_game_root(const char *root) {
    if (root && root[0]) snprintf(g_game_root, sizeof g_game_root, "%s", root);
}
// Signed in (the launcher, lasrecomp --account login): the save on this PC linked to the account - the first sign-in
// with nothing on the server sends it up; both having one: the newer is used, the other kept (sync_now)
static void link_save(char *msg, size_t mn) {
    if (!g_settings.sync_saves) {
        snprintf(msg, mn, "Save sync is off: the save on this PC isn't linked to the account (Keep my save synced)");
        return;
    }
    uint64_t id = g_cli_pid ? g_cli_pid : program_id_of(g_game_root);
    if (!id) { snprintf(msg, mn, "The save links to the account once the game files are set up (at the game's next start)"); return; }
    Sync y;
    if (!sync_setup(&y, id, save_root())) { snprintf(msg, mn, "Not signed in on this server"); return; }
    sync_now(&y, msg, mn);
    set_last("%s", msg);
}

int acct_cli(int argc, char **argv) {
    const char *cmd = NULL, *user = "", *pwfile = getenv("LARECOMP_ACCT_PASSWORD_FILE"), *server = NULL, *pid = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--account") && i + 1 < argc) cmd = argv[++i];
        else if (!strcmp(argv[i], "--server") && i + 1 < argc) server = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) user = argv[++i];
        else if (!strcmp(argv[i], "--password-file") && i + 1 < argc) pwfile = argv[++i];
        else if (!strcmp(argv[i], "--program-id") && i + 1 < argc) pid = argv[++i];
    }
    settings_load();
    if (pid) g_cli_pid = strtoull(pid, NULL, 16);
    if (server) snprintf(g_settings.online_server, sizeof g_settings.online_server, "%s", server);
    if (!cmd) return 2;
    char msg[1200];
    bool kc;
    if (!strcmp(cmd, "status")) {
        char name[64];
        if (acct_signed_in(name, sizeof name)) printf("signed in as %s on the server set\n", name);
        else printf("not signed in on %s\n", g_settings.online_server[0] ? "the server set" : "(no server set)");
        char ls[800];
        snprintf(ls, sizeof ls, "last sync: %s", acct_last_sync()[0] ? acct_last_sync() : "never");
        say(ls);
        return 0;
    }
    if (!strcmp(cmd, "logout") || !strcmp(cmd, "forget-key")) {
        int r = account_op(cmd[0] == 'l' ? ACCT_OP_LOGOUT : ACCT_OP_FORGET_KEY, g_settings.online_server, "", "", msg, sizeof msg, &kc);
        say(msg);
        return r;
    }
    if (!strcmp(cmd, "login") || !strcmp(cmd, "create")) {
        char pw[ACCT_PASS_MAX + 4] = "";
        FILE *f = pwfile ? fopen(pwfile, "r") : stdin;
        if (!f || !fgets(pw, sizeof pw, f)) { fprintf(stderr, "no password (--password-file or stdin)\n"); return 2; }
        if (f != stdin) fclose(f);
        pw[strcspn(pw, "\r\n")] = 0;
        int r = account_op(cmd[0] == 'c' ? ACCT_OP_CREATE : ACCT_OP_LOGIN, g_settings.online_server, user, pw, msg, sizeof msg, &kc);
        mbedtls_platform_zeroize(pw, sizeof pw);
        say(msg);
        if (kc) say("[key changed]");
        if (!r && !getenv("LARECOMP_ACCT_NO_LINK")) {  // (as the launcher: the save here linked to the account)
            link_save(msg, sizeof msg);
            say(msg);
        }
        return r;
    }
    if (!strcmp(cmd, "sync")) {
        uint64_t id = pid ? strtoull(pid, NULL, 16) : program_id_of("extracted");
        if (!id) { fprintf(stderr, "no program id (--program-id or extracted/exefs/main.npdm)\n"); return 2; }
        Sync y;
        if (!sync_setup(&y, id, save_root())) { printf("not signed in (or sync_saves = 0)\n"); return 1; }
        int r = sync_now(&y, msg, sizeof msg);
        set_last("%s", msg);
        say(msg);
        return r;
    }
    fprintf(stderr, "--account create | login | logout | status | sync | forget-key\n");
    return 2;
}
