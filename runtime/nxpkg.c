// Packages -> game files (runtime/nxpkg.h). What is read, in order:
//   .nsp = a PFS0 of NCAs (+ tickets .tik); .xci = a gamecard image whose HFS0 "secure" partition holds the NCAs.
//   NCA3: 0xC00 header bytes in AES-XTS (header_key, sectors of 0x200, the sector number as a big-endian tweak); the
//   content key is the key area's AES-CTR key (key_area_key_<kind>_<generation>) or, with a rights id, the title key
//   from the ticket / title.keys (titlekek_<generation>). Sections are AES-CTR: the counter is the section's upper IV
//   (FS header +0x140, big-endian) and the byte offset in the NCA / 16.
//   An update's RomFS section (BKTR): an indirect table maps the patched section's offsets to the base game's RomFS
//   section (storage 0) or to the update's own data (storage 1), whose AES-CTR counter takes a generation from the
//   AesCtrEx table per range. Both tables are bucket trees (16 KiB nodes) in plain AES-CTR at the section's end.
//   The RomFS image is the IVFC data level (the last of the hash tree) of that section.
// Layouts this doesn't read (sparse and compressed sections, NCZ) are refused with a message.
// Checked while the image is written (nxpkg_check): each NCA used is hashed whole, as stored (SHA-256: its first 16
// bytes are the content id, the file's name - a damaged or altered file doesn't match), on a second thread, from the
// same reads as the extraction where it reads in order (the rest is read for the hash alone); and the image against
// the section's hash tree (IVFC: the master hash in the FS header, then each level's blocks against the level above,
// the data level - the image - block by block as it's written), on a third.
#include "nxpkg.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "platform.h"  // (Windows: POSIX mkdir)
#include "rt.h"

#ifdef _WIN32
#define xseek(f, o) _fseeki64(f, (long long)(o), SEEK_SET)
#define xsize(f) (_fseeki64(f, 0, SEEK_END), (uint64_t)_ftelli64(f))
#else
#define xseek(f, o) fseeko(f, (off_t)(o), SEEK_SET)
#define xsize(f) (fseeko(f, 0, SEEK_END), (uint64_t)ftello(f))
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <immintrin.h>
#define HAVE_AESNI 1
#endif

#define CHUNK (8u << 20)

// ------------------------------------------------------------------ AES-128 (ECB / CTR / XTS)
static uint8_t g_sbox[256], g_isbox[256];

static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)(x << s | x >> (8 - s)); }
static void aes_tables(void) {
    if (g_sbox[0]) return;
    uint8_t p = 1, q = 1;
    do {  // (p walks the field by multiplying by 3, q by dividing by 3: q = 1/p)
        p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
        q ^= (uint8_t)(q << 1), q ^= (uint8_t)(q << 2), q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        g_sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    g_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) g_isbox[g_sbox[i]] = (uint8_t)i;
}

typedef struct { uint8_t rk[176]; } Aes;

static void aes_key(Aes *a, const uint8_t key[16]) {
    aes_tables();
    memcpy(a->rk, key, 16);
    uint8_t rc = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, a->rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t t0 = t[0];
            t[0] = (uint8_t)(g_sbox[t[1]] ^ rc), t[1] = g_sbox[t[2]], t[2] = g_sbox[t[3]], t[3] = g_sbox[t0];
            rc = (uint8_t)(rc << 1 ^ (rc & 0x80 ? 0x1B : 0));
        }
        for (int j = 0; j < 4; j++) a->rk[i + j] = (uint8_t)(a->rk[i - 16 + j] ^ t[j]);
    }
}

static uint8_t xt(uint8_t x) { return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1B : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a)) if (b & 1) r ^= a;
    return r;
}

static void aes_enc(const Aes *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[i];
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) t[i] = g_sbox[s[(i + 4 * (i % 4)) % 16]];  // SubBytes + ShiftRows
        if (r < 10)
            for (int c = 0; c < 16; c += 4) {  // MixColumns
                uint8_t a0 = t[c], a1 = t[c + 1], a2 = t[c + 2], a3 = t[c + 3], x = a0 ^ a1 ^ a2 ^ a3;
                t[c] ^= x ^ xt(a0 ^ a1), t[c + 1] ^= x ^ xt(a1 ^ a2), t[c + 2] ^= x ^ xt(a2 ^ a3), t[c + 3] ^= x ^ xt(a3 ^ a0);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ a->rk[16 * r + i];
    }
    memcpy(out, s, 16);
}

static void aes_dec(const Aes *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = g_isbox[s[i]];  // InvShiftRows + InvSubBytes
        for (int i = 0; i < 16; i++) t[i] ^= a->rk[16 * r + i];
        if (r > 0)
            for (int c = 0; c < 16; c += 4) {  // InvMixColumns
                uint8_t a0 = t[c], a1 = t[c + 1], a2 = t[c + 2], a3 = t[c + 3];
                t[c] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                t[c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                t[c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                t[c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

static void ecb_dec(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n) {
    Aes a;
    aes_key(&a, key);
    for (size_t i = 0; i + 16 <= n; i += 16) aes_dec(&a, in + i, out + i);
}

#ifdef HAVE_AESNI
__attribute__((target("aes,sse2"))) static void ctr_ni(const Aes *a, uint64_t hi, uint64_t ctr, const uint8_t *src, uint8_t *buf, size_t nb) {
    __m128i k[11];
    for (int i = 0; i < 11; i++) k[i] = _mm_loadu_si128((const __m128i *)(a->rk + 16 * i));
    size_t b = 0;
    for (; b + 8 <= nb; b += 8) {
        __m128i x[8];
        for (int j = 0; j < 8; j++) x[j] = _mm_xor_si128(_mm_set_epi64x((long long)__builtin_bswap64(ctr + j), (long long)hi), k[0]);
        for (int r = 1; r < 10; r++)
            for (int j = 0; j < 8; j++) x[j] = _mm_aesenc_si128(x[j], k[r]);
        for (int j = 0; j < 8; j++)
            _mm_storeu_si128((__m128i *)(buf + 16 * (b + j)),
                             _mm_xor_si128(_mm_loadu_si128((const __m128i *)(src + 16 * (b + j))), _mm_aesenclast_si128(x[j], k[10])));
        ctr += 8;
    }
    for (; b < nb; b++, ctr++) {
        __m128i x = _mm_xor_si128(_mm_set_epi64x((long long)__builtin_bswap64(ctr), (long long)hi), k[0]);
        for (int r = 1; r < 10; r++) x = _mm_aesenc_si128(x, k[r]);
        _mm_storeu_si128((__m128i *)(buf + 16 * b), _mm_xor_si128(_mm_loadu_si128((const __m128i *)(src + 16 * b)), _mm_aesenclast_si128(x, k[10])));
    }
}
#endif

// AES-CTR over src into buf (whole blocks; the same buffer or apart), the counter: upper (big-endian) | block index
// (big-endian).
static void ctr_xor(const Aes *a, uint64_t upper, uint64_t block, const uint8_t *src, uint8_t *buf, size_t nb) {
    uint8_t iv[16];
    for (int i = 0; i < 8; i++) iv[i] = (uint8_t)(upper >> (56 - 8 * i));
#ifdef HAVE_AESNI
    static int ni = -1;
    if (ni < 0) {  // (CPUID 1, ECX bit 25: AES-NI - read directly: __builtin_cpu_supports needs a runtime symbol the
                   // Windows build (zig's lld-link) doesn't have)
        unsigned a, b, c, d;
        ni = __get_cpuid(1, &a, &b, &c, &d) && (c >> 25 & 1);
    }
    if (ni) {
        uint64_t hi;
        memcpy(&hi, iv, 8);
        ctr_ni(a, hi, block, src, buf, nb);
        return;
    }
#endif
    for (size_t b = 0; b < nb; b++, block++) {
        uint8_t ks[16];
        for (int i = 0; i < 8; i++) iv[8 + i] = (uint8_t)(block >> (56 - 8 * i));
        aes_enc(a, iv, ks);
        for (int i = 0; i < 16; i++) buf[16 * b + i] = src[16 * b + i] ^ ks[i];
    }
}

// The NCA header: AES-XTS, sectors of 0x200 from sector 0, the tweak = the sector number, big-endian.
static void xts_dec(const uint8_t key[32], uint8_t *buf, size_t n) {
    Aes k1, k2;
    aes_key(&k1, key), aes_key(&k2, key + 16);
    for (size_t sec = 0; sec * 0x200 < n; sec++) {
        uint8_t tw[16] = {0}, t[16];
        for (int i = 0; i < 8; i++) tw[15 - i] = (uint8_t)(sec >> (8 * i));
        aes_enc(&k2, tw, t);
        for (size_t o = sec * 0x200; o < (sec + 1) * 0x200 && o + 16 <= n; o += 16) {
            uint8_t x[16];
            for (int i = 0; i < 16; i++) x[i] = buf[o + i] ^ t[i];
            aes_dec(&k1, x, x);
            for (int i = 0; i < 16; i++) buf[o + i] = x[i] ^ t[i];
            int carry = t[15] >> 7;  // (the tweak times x in GF(2^128), little-endian)
            for (int i = 15; i > 0; i--) t[i] = (uint8_t)(t[i] << 1 | t[i - 1] >> 7);
            t[0] = (uint8_t)(t[0] << 1 ^ (carry ? 0x87 : 0));
        }
    }
}

// ------------------------------------------------------------------ SHA-256
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define SROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_blocks_c(uint32_t s[8], const uint8_t *p, size_t nb) {
    for (; nb; nb--, p += 64) {
        uint32_t w[64], a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++)
            w[i] = w[i - 16] + (SROR(w[i - 15], 7) ^ SROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] +
                   (SROR(w[i - 2], 17) ^ SROR(w[i - 2], 19) ^ w[i - 2] >> 10);
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + (SROR(e, 6) ^ SROR(e, 11) ^ SROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
            uint32_t t2 = (SROR(a, 2) ^ SROR(a, 13) ^ SROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        s[0] += a, s[1] += b, s[2] += c, s[3] += d, s[4] += e, s[5] += f, s[6] += g, s[7] += h;
    }
}

#ifdef HAVE_AESNI
// The SHA extensions (Intel since Goldmont / Ice Lake, AMD since Zen): ~2 GB/s against ~0.4 in C.
__attribute__((target("sha,sse4.1"))) static void sha_blocks_ni(uint32_t s[8], const uint8_t *p, size_t nb) {
    const __m128i bswap = _mm_set_epi64x(0x0c0d0e0f08090a0bll, 0x0405060700010203ll);
    __m128i t = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)s), 0xB1);         // CDAB
    __m128i s1 = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)(s + 4)), 0x1B);  // EFGH
    __m128i s0 = _mm_alignr_epi8(t, s1, 8);                                           // ABEF
    s1 = _mm_blend_epi16(s1, t, 0xF0);                                                // CDGH
    for (; nb; nb--, p += 64) {
        __m128i a0 = s0, c0 = s1, w[4];
        for (int g = 0; g < 16; g++) {  // (4 rounds each; w: the last 16 schedule words)
            __m128i m;
            if (g < 4) m = w[g] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * g)), bswap);
            else {
                m = _mm_add_epi32(_mm_sha256msg1_epu32(w[g & 3], w[(g + 1) & 3]), _mm_alignr_epi8(w[(g + 3) & 3], w[(g + 2) & 3], 4));
                m = w[g & 3] = _mm_sha256msg2_epu32(m, w[(g + 3) & 3]);
            }
            m = _mm_add_epi32(m, _mm_loadu_si128((const __m128i *)(K256 + 4 * g)));
            s1 = _mm_sha256rnds2_epu32(s1, s0, m);
            s0 = _mm_sha256rnds2_epu32(s0, s1, _mm_shuffle_epi32(m, 0x0E));
        }
        s0 = _mm_add_epi32(s0, a0), s1 = _mm_add_epi32(s1, c0);
    }
    t = _mm_shuffle_epi32(s0, 0x1B);                                   // FEBA
    s1 = _mm_shuffle_epi32(s1, 0xB1);                                  // DCHG
    _mm_storeu_si128((__m128i *)s, _mm_blend_epi16(t, s1, 0xF0));     // DCBA
    _mm_storeu_si128((__m128i *)(s + 4), _mm_alignr_epi8(s1, t, 8));  // HGFE
}
#endif

typedef struct { uint32_t s[8]; uint64_t len; uint8_t buf[64]; size_t nb; } Sha;
static void (*g_sha_blocks)(uint32_t *, const uint8_t *, size_t);

static void sha_init(Sha *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    if (!g_sha_blocks) {
        void (*f)(uint32_t *, const uint8_t *, size_t) = sha_blocks_c;
#ifdef HAVE_AESNI
        unsigned a, b, c1, d;  // (CPUID 7: EBX bit 29 SHA; 1: ECX bits 9 SSSE3, 19 SSE4.1)
        if (__get_cpuid_count(7, 0, &a, &b, &c1, &d) && (b >> 29 & 1) && __get_cpuid(1, &a, &b, &c1, &d) && (c1 >> 9 & 1) && (c1 >> 19 & 1))
            f = sha_blocks_ni;
#endif
        g_sha_blocks = f;
    }
    memcpy(c->s, iv, sizeof iv), c->len = 0, c->nb = 0;
}
static void sha_update(Sha *c, const uint8_t *p, size_t n) {
    c->len += n;
    if (c->nb) {
        size_t k = 64 - c->nb < n ? 64 - c->nb : n;
        memcpy(c->buf + c->nb, p, k), c->nb += k, p += k, n -= k;
        if (c->nb < 64) return;
        g_sha_blocks(c->s, c->buf, 1), c->nb = 0;
    }
    if (n >= 64) g_sha_blocks(c->s, p, n / 64), p += n & ~(size_t)63, n &= 63;
    memcpy(c->buf, p, n), c->nb = n;
}
static void sha_final(Sha *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad[128] = {0x80};
    sha_update(c, pad, (c->nb < 56 ? 56 : 120) - c->nb);
    for (int i = 0; i < 8; i++) pad[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(c, pad, 8);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(c->s[i / 4] >> (24 - 8 * (i % 4)));
}

static void hex(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) out[2 * i] = "0123456789abcdef"[b[i] >> 4], out[2 * i + 1] = "0123456789abcdef"[b[i] & 15];
    out[2 * n] = 0;
}

// ------------------------------------------------------------------ hashing threads
// A worker with a few buffers: the reading thread fills one (pipe_slot), hands it over (pipe_push) and goes on while
// the worker runs fn over it. No thread (pthread_create failed): fn runs at once, on the reading thread.
#define NSLOT 3
typedef void (*PipeFn)(void *ctx, const uint8_t *b, size_t n);
typedef struct {
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint8_t *slot[NSLOT];
    size_t len[NSLOT];
    PipeFn fn[NSLOT];
    void *ctx[NSLOT];
    int head, count;
    bool on, quit;
} Pipe;

static void *pipe_main(void *arg) {
    Pipe *q = arg;
    pthread_mutex_lock(&q->mu);
    for (;;) {
        while (!q->count && !q->quit) pthread_cond_wait(&q->cv, &q->mu);
        if (!q->count) break;
        int h = q->head;
        pthread_mutex_unlock(&q->mu);
        q->fn[h](q->ctx[h], q->slot[h], q->len[h]);
        pthread_mutex_lock(&q->mu);
        q->head = (h + 1) % NSLOT, q->count--;
        pthread_cond_broadcast(&q->cv);
    }
    pthread_mutex_unlock(&q->mu);
    return NULL;
}
static bool pipe_start(Pipe *q) {
    if (q->slot[0]) return true;
    for (int i = 0; i < NSLOT; i++)
        if (!(q->slot[i] = malloc(CHUNK + 32))) {
            for (int j = 0; j <= i; j++) free(q->slot[j]), q->slot[j] = NULL;
            return false;
        }
    pthread_mutex_init(&q->mu, NULL), pthread_cond_init(&q->cv, NULL);
    q->on = pthread_create(&q->th, NULL, pipe_main, q) == 0;
    return true;
}
static uint8_t *pipe_slot(Pipe *q) {  // (a free buffer, CHUNK + 32 bytes)
    if (!q->on) return q->slot[0];
    pthread_mutex_lock(&q->mu);
    while (q->count == NSLOT) pthread_cond_wait(&q->cv, &q->mu);
    uint8_t *s = q->slot[(q->head + q->count) % NSLOT];
    pthread_mutex_unlock(&q->mu);
    return s;
}
static void pipe_push(Pipe *q, size_t n, PipeFn fn, void *ctx) {  // (the buffer pipe_slot gave)
    if (!q->on) { fn(ctx, q->slot[0], n); return; }
    pthread_mutex_lock(&q->mu);
    int t = (q->head + q->count) % NSLOT;
    q->len[t] = n, q->fn[t] = fn, q->ctx[t] = ctx, q->count++;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}
static void pipe_wait(Pipe *q) {
    if (!q->on) return;
    pthread_mutex_lock(&q->mu);
    while (q->count) pthread_cond_wait(&q->cv, &q->mu);
    pthread_mutex_unlock(&q->mu);
}
static void pipe_stop(Pipe *q) {
    if (q->on) {
        pthread_mutex_lock(&q->mu);
        q->quit = true;
        pthread_cond_broadcast(&q->cv);
        pthread_mutex_unlock(&q->mu);
        pthread_join(q->th, NULL);
    }
    if (q->slot[0]) pthread_mutex_destroy(&q->mu), pthread_cond_destroy(&q->cv);
    for (int i = 0; i < NSLOT; i++) free(q->slot[i]), q->slot[i] = NULL;
    q->on = false;
}

// ------------------------------------------------------------------ keys
#define NGEN 0x20
typedef struct {
    uint8_t header_key[32];
    uint8_t kak[3][NGEN][16];  // key_area_key_{application,ocean,system}_XX
    uint8_t titlekek[NGEN][16], master[NGEN][16];
    uint32_t have_kak[3], have_titlekek, have_master;  // (bit per generation)
    bool have_header;
    // sources, to derive what a keys file leaves out
    uint8_t kak_src[3][16], titlekek_src[16], aes_kek_gen_src[16], aes_key_gen_src[16], header_kek_src[16], header_key_src[32];
    uint32_t have_src;  // bits: 0-2 kak, 3 titlekek, 4 aes_kek_gen, 5 aes_key_gen, 6 header_kek, 7 header_key
    char path[1024];
} Keys;

typedef struct { uint8_t rights[16], key[16]; } TitleKey;

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static size_t parse_hex(const char *s, uint8_t *out, size_t max) {
    size_t n = 0;
    while (*s == ' ' || *s == '\t') s++;
    for (; n < max && hexval(s[0]) >= 0 && hexval(s[1]) >= 0; s += 2) out[n++] = (uint8_t)(hexval(s[0]) << 4 | hexval(s[1]));
    return hexval(*s) >= 0 ? 0 : n;  // (longer than expected: not this key)
}

static bool keys_file(Keys *k, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    static const char *const kind[3] = {"application", "ocean", "system"};
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char name[128];
        size_t n = 0;
        for (char *c = line; *c && n + 1 < sizeof name; c++)
            if (!isspace((unsigned char)*c)) name[n++] = (char)tolower((unsigned char)*c);
        name[n] = 0;
        const char *v = eq + 1;
        uint8_t b[32];
        unsigned gen = 0;
        char tail[8];
        if (!strcmp(name, "header_key") && parse_hex(v, b, 32) == 32) memcpy(k->header_key, b, 32), k->have_header = true;
        else if (sscanf(name, "titlekek_%2x%1s", &gen, tail) == 1 && gen < NGEN && parse_hex(v, b, 16) == 16)
            memcpy(k->titlekek[gen], b, 16), k->have_titlekek |= 1u << gen;
        else if (sscanf(name, "master_key_%2x%1s", &gen, tail) == 1 && gen < NGEN && parse_hex(v, b, 16) == 16)
            memcpy(k->master[gen], b, 16), k->have_master |= 1u << gen;
        else if (!strcmp(name, "titlekek_source") && parse_hex(v, b, 16) == 16) memcpy(k->titlekek_src, b, 16), k->have_src |= 8;
        else if (!strcmp(name, "aes_kek_generation_source") && parse_hex(v, b, 16) == 16) memcpy(k->aes_kek_gen_src, b, 16), k->have_src |= 16;
        else if (!strcmp(name, "aes_key_generation_source") && parse_hex(v, b, 16) == 16) memcpy(k->aes_key_gen_src, b, 16), k->have_src |= 32;
        else if (!strcmp(name, "header_kek_source") && parse_hex(v, b, 16) == 16) memcpy(k->header_kek_src, b, 16), k->have_src |= 64;
        else if (!strcmp(name, "header_key_source") && parse_hex(v, b, 32) == 32) memcpy(k->header_key_src, b, 32), k->have_src |= 128;
        else
            for (int i = 0; i < 3; i++) {
                char pre[64], src[64];
                snprintf(pre, sizeof pre, "key_area_key_%s_%%2x%%1s", kind[i]);
                snprintf(src, sizeof src, "key_area_key_%s_source", kind[i]);
                if (sscanf(name, pre, &gen, tail) == 1 && gen < NGEN && parse_hex(v, b, 16) == 16)
                    memcpy(k->kak[i][gen], b, 16), k->have_kak[i] |= 1u << gen;
                else if (!strcmp(name, src) && parse_hex(v, b, 16) == 16) memcpy(k->kak_src[i], b, 16), k->have_src |= 1u << i;
            }
    }
    fclose(f);
    return true;
}

// The keys a file leaves out, from the master keys and the sources (as the console derives them).
static void keys_derive(Keys *k) {
    for (int g = 0; g < NGEN; g++) {
        if (!(k->have_master >> g & 1)) continue;
        if (!(k->have_titlekek >> g & 1) && (k->have_src & 8))
            ecb_dec(k->master[g], k->titlekek_src, k->titlekek[g], 16), k->have_titlekek |= 1u << g;
        if ((k->have_src & 48) != 48) continue;
        uint8_t kek[16];
        ecb_dec(k->master[g], k->aes_kek_gen_src, kek, 16);
        for (int i = 0; i < 3; i++)
            if (!(k->have_kak[i] >> g & 1) && (k->have_src >> i & 1)) {
                uint8_t sk[16];
                ecb_dec(kek, k->kak_src[i], sk, 16);
                ecb_dec(sk, k->aes_key_gen_src, k->kak[i][g], 16);
                k->have_kak[i] |= 1u << g;
            }
        if (g == 0 && !k->have_header && (k->have_src & 0xF0) == 0xF0) {
            uint8_t sk[16], hk[16];
            ecb_dec(kek, k->header_kek_src, sk, 16);
            ecb_dec(sk, k->aes_key_gen_src, hk, 16);
            ecb_dec(hk, k->header_key_src, k->header_key, 32);
            k->have_header = true;
        }
    }
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static char g_keys_file[1024], g_more[4][1024];  // (nxpkg_set_keys, nxpkg_add_path)
static int g_nmore;
static void slashes(char *p) { for (; *p; p++) if (*p == '\\') *p = '/'; }
void nxpkg_set_keys(const char *file) { snprintf(g_keys_file, sizeof g_keys_file, "%s", file ? file : ""), slashes(g_keys_file); }
void nxpkg_add_path(const char *path) {
    if (path && *path && g_nmore < 4) snprintf(g_more[g_nmore], sizeof g_more[0], "%s", path), slashes(g_more[g_nmore++]);
}

static bool keys_try(Keys *k, const char *p) {
    if (!exists(p)) return false;
    Keys t;
    memset(&t, 0, sizeof t);
    keys_file(&t, p);
    keys_derive(&t);
    if (!t.have_header) return false;
    *k = t;
    snprintf(k->path, sizeof k->path, "%s", p);
    return true;
}

// prod.keys: the one given (--keys), next to the packages, then the usual places (~/.switch, the emulators' key folders).
static bool keys_find(Keys *k, const char *folder) {
    if (g_keys_file[0] && keys_try(k, g_keys_file)) return true;
    char dirs[24][1024];
    int n = 0;
    snprintf(dirs[n++], sizeof dirs[0], "%s", folder);
    snprintf(dirs[n++], sizeof dirs[0], ".");  // (the data root)
    snprintf(dirs[n++], sizeof dirs[0], "keys");
    const char *home = getenv("HOME");
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (!home) home = getenv("USERPROFILE");
#endif
    if (home) snprintf(dirs[n++], sizeof dirs[0], "%s/.switch", home);
    static const char *const emus[] = {"eden", "yuzu", "suyu", "sudachi", "citron", "torzu"};
    for (size_t i = 0; i < sizeof emus / sizeof *emus && home; i++) {
#ifdef _WIN32
        if (appdata) snprintf(dirs[n++], sizeof dirs[0], "%s/%s/keys", appdata, emus[i]);
#elif defined(__APPLE__)
        snprintf(dirs[n++], sizeof dirs[0], "%s/Library/Application Support/%s/keys", home, emus[i]);
#else
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg && *xdg) snprintf(dirs[n++], sizeof dirs[0], "%s/%s/keys", xdg, emus[i]);
        else snprintf(dirs[n++], sizeof dirs[0], "%s/.local/share/%s/keys", home, emus[i]);
#endif
    }
#ifdef _WIN32
    if (appdata) snprintf(dirs[n++], sizeof dirs[0], "%s/Ryujinx/system", appdata);
#elif defined(__APPLE__)
    if (home) snprintf(dirs[n++], sizeof dirs[0], "%s/Library/Application Support/Ryujinx/system", home);
#else
    if (home) snprintf(dirs[n++], sizeof dirs[0], "%s/.config/Ryujinx/system", home);
#endif
    for (int i = 0; i < n; i++) {
        static const char *const names[] = {"prod.keys", "keys.txt", "keys.prod"};
        for (size_t j = 0; j < sizeof names / sizeof *names; j++) {
            char p[1100];
            snprintf(p, sizeof p, "%s/%s", dirs[i], names[j]);
            if (keys_try(k, p)) return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ packages
typedef struct { char name[128]; uint64_t off, size; int nca; } Ent;  // a file in a package (nca: its Nca, -1 other)
typedef struct {
    char path[1024];
    FILE *f;
    uint64_t size, hdr;  // (the file's size; its PFS0 / secure HFS0 header's)
    const char *kind;    // "PFS0" (.nsp), "HFS0" (.xci's secure partition)
    Ent *ents;
    int nent;
} Pkg;

typedef struct {
    int pkg;
    uint64_t off, size;  // in the package file
    uint8_t h[0xC00];    // the decrypted header
    uint8_t type;        // 0 program, 1 meta, 2 control, 3 manual, 4 data, 5 public data
    uint64_t id;         // program id
    uint32_t version;    // (from the package's meta, when it has one)
    bool patch;          // an update's program (its meta says so; the program id is the base game's)
    uint8_t cid[16];     // content id (the file name: <32 hex digits>.nca), what a meta's content records name
    bool has_cid;
    Aes key;
    int key_state;  // 0 not looked up, 1 ok, -1 missing
    int ent;        // its Ent in the package
    int meta;       // the meta NCA whose content records name it (-1 none)
    int ctype;      // its content type in that record (-1 not named): 0 meta, 1 program, 2 data, 3 control, 4 html document...
    uint8_t rec_hash[32];  // that record's SHA-256 of it
    // the SHA-256 of the whole NCA as stored, while it's read: hon = being hashed, hpos = bytes handed to the hasher
    // (in order; reads behind it were hashed already, reads ahead of it first hash what's between), hres NXPKG_CHECK_*
    Sha sha;
    bool hon;
    int hres;
    uint64_t hpos;
    uint8_t digest[32];
} Nca;

struct NxPkg {
    Keys keys;
    Pkg *pkgs;
    int npkg;
    Nca *ncas;
    int nnca;
    TitleKey *tks;
    int ntk;
    uint64_t title;
    int base;       // nca index of the base program
    int upd[16];    // nca indexes of the updates, newest first
    int nupd;
    int dlc;        // nca index of the add-on content (-1 none)
    bool compressed;
    uint8_t *scratch, *buf;  // (CHUNK + 32 each: raw blocks being decrypted, data being written)
    Pipe hp, vp;             // hashing threads: the NCAs' SHA-256 (stored bytes), the image's hash tree (written bytes)
    bool checking;           // (the pipes started: checks on)
    uint64_t hash_extra;     // bytes read for the NCA hashes alone (what the extraction didn't read in order)
    NxCheck chk;
};

static bool rd(NxPkg *P, int pkg, uint64_t off, void *buf, size_t n) {
    FILE *f = P->pkgs[pkg].f;
    return !xseek(f, off) && fread(buf, 1, n, f) == n;
}

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }

static bool ext_is(const char *name, const char *ext) {
    size_t a = strlen(name), b = strlen(ext);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)name[a - b + i]) != ext[i]) return false;
    return true;
}

static void add_ticket(NxPkg *P, const uint8_t *t, size_t n) {
    if (n < 0x2C0) return;
    uint32_t sig = (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3];
    size_t data = sig == 0x10000 || sig == 0x10003 ? 0x240 : sig == 0x10002 || sig == 0x10005 ? 0x80 : 0x140;
    if (data + 0x170 > n || t[data + 0x141] != 0) return;  // (personalized tickets: the title key is RSA-wrapped)
    P->tks = realloc(P->tks, (size_t)(P->ntk + 1) * sizeof *P->tks);
    memcpy(P->tks[P->ntk].rights, t + data + 0x160, 16);
    memcpy(P->tks[P->ntk].key, t + data + 0x40, 16);
    P->ntk++;
}

static void title_keys_file(NxPkg *P, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        TitleKey tk;
        if (parse_hex(s, tk.rights, 16) != 16 || parse_hex(eq + 1, tk.key, 16) != 16) continue;
        P->tks = realloc(P->tks, (size_t)(P->ntk + 1) * sizeof *P->tks);
        P->tks[P->ntk++] = tk;
    }
    fclose(f);
}

// A file of a package, listed (the log's list of what each package holds)
static void add_ent(NxPkg *P, int pkg, const char *name, uint64_t off, uint64_t size, int nca) {
    Pkg *k = &P->pkgs[pkg];
    if (k->nent >= 256) return;
    k->ents = realloc(k->ents, (size_t)(k->nent + 1) * sizeof *k->ents);
    Ent *e = &k->ents[k->nent++];
    snprintf(e->name, sizeof e->name, "%s", name), e->off = off, e->size = size, e->nca = nca;
    if (nca >= 0) P->ncas[nca].ent = k->nent - 1;
}

static int add_nca(NxPkg *P, int pkg, const char *name, uint64_t off, uint64_t size) {
    if (size < 0xC00) return -1;
    Nca n;
    memset(&n, 0, sizeof n);
    n.pkg = pkg, n.off = off, n.size = size, n.meta = n.ctype = -1;
    n.has_cid = strlen(name) >= 36 && name[32] == '.' && parse_hex(name, n.cid, 16) == 16;
    if (!rd(P, pkg, off, n.h, sizeof n.h)) return -1;
    xts_dec(P->keys.header_key, n.h, sizeof n.h);
    if (memcmp(n.h + 0x200, "NCA3", 4)) return -1;  // (NCA2 / NCA0: titles from before 1.0, not these games)
    n.type = n.h[0x205];
    n.id = u64(n.h + 0x210);
    P->ncas = realloc(P->ncas, (size_t)(P->nnca + 1) * sizeof *P->ncas);
    P->ncas[P->nnca++] = n;
    return P->nnca - 1;
}

// PFS0 (.nsp) at `off` of the package
static void read_pfs0(NxPkg *P, int pkg, uint64_t off) {
    uint8_t h[16];
    if (!rd(P, pkg, off, h, 16) || memcmp(h, "PFS0", 4)) return;
    uint32_t cnt = u32(h + 4), strsz = u32(h + 8);
    if (cnt > 4096 || strsz > (1u << 20)) return;
    size_t tsz = 16 + (size_t)cnt * 0x18 + strsz;
    uint8_t *t = malloc(tsz);
    if (!t || !rd(P, pkg, off, t, tsz)) { free(t); return; }
    const char *str = (const char *)t + 16 + cnt * 0x18;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *e = t + 16 + i * 0x18;
        uint64_t fo = off + tsz + u64(e), fs = u64(e + 8);
        uint32_t no = u32(e + 16);
        if (no >= strsz) continue;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)(strsz - no), str + no);
        int nca = -1;
        if (ext_is(name, ".nca")) nca = add_nca(P, pkg, name, fo, fs);
        else if (ext_is(name, ".ncz")) P->compressed = true;
        else if (ext_is(name, ".tik") && fs <= 0x1000) {
            uint8_t tik[0x1000];
            if (rd(P, pkg, fo, tik, (size_t)fs)) add_ticket(P, tik, (size_t)fs);
        }
        add_ent(P, pkg, name, fo, fs, nca);
    }
    P->pkgs[pkg].kind = "PFS0", P->pkgs[pkg].hdr = tsz;
    free(t);
}

// HFS0 (gamecard partitions): the root's "secure" partition holds the NCAs
static void read_hfs0(NxPkg *P, int pkg, uint64_t off, bool root) {
    uint8_t h[16];
    if (!rd(P, pkg, off, h, 16) || memcmp(h, "HFS0", 4)) return;
    uint32_t cnt = u32(h + 4), strsz = u32(h + 8);
    if (cnt > 4096 || strsz > (1u << 20)) return;
    size_t tsz = 16 + (size_t)cnt * 0x40 + strsz;
    uint8_t *t = malloc(tsz);
    if (!t || !rd(P, pkg, off, t, tsz)) { free(t); return; }
    const char *str = (const char *)t + 16 + cnt * 0x40;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *e = t + 16 + i * 0x40;
        uint64_t fo = off + tsz + u64(e), fs = u64(e + 8);
        uint32_t no = u32(e + 16);
        if (no >= strsz) continue;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)(strsz - no), str + no);
        if (root) { if (!strcmp(name, "secure")) read_hfs0(P, pkg, fo, false); continue; }
        int nca = -1;
        if (ext_is(name, ".nca")) nca = add_nca(P, pkg, name, fo, fs);
        else if (ext_is(name, ".ncz")) P->compressed = true;
        add_ent(P, pkg, name, fo, fs, nca);
    }
    if (!root) P->pkgs[pkg].kind = "HFS0", P->pkgs[pkg].hdr = tsz;
    free(t);
}

static void add_package(NxPkg *P, const char *path) {
    bool xci = ext_is(path, ".xci") || ext_is(path, ".xcz"), nsp = ext_is(path, ".nsp") || ext_is(path, ".nsz");
    if (!xci && !nsp) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    P->pkgs = realloc(P->pkgs, (size_t)(P->npkg + 1) * sizeof *P->pkgs);
    Pkg *k = &P->pkgs[P->npkg];
    memset(k, 0, sizeof *k);
    snprintf(k->path, sizeof k->path, "%s", path);
    k->f = f, k->kind = "?";
    k->size = xsize(f);
    int i = P->npkg++;
    if (nsp) read_pfs0(P, i, 0);
    else {
        uint8_t h[0x200];
        for (uint64_t base = 0; base <= 0x1000; base += 0x1000)  // (dumps with the key area in front)
            if (rd(P, i, base, h, sizeof h) && !memcmp(h + 0x100, "HEAD", 4)) {
                read_hfs0(P, i, base + u64(h + 0x130), true);
                break;
            }
    }
}

static void scan(NxPkg *P, const char *path, int depth, bool count_only, bool *any) {
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISREG(st.st_mode)) {
        if (count_only) *any |= ext_is(path, ".nsp") || ext_is(path, ".xci") || ext_is(path, ".nsz") || ext_is(path, ".xcz");
        else add_package(P, path);
        return;
    }
    if (!S_ISDIR(st.st_mode) || depth > 4) return;
    DIR *d = opendir(path);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        scan(P, p, depth + 1, count_only, any);
    }
    closedir(d);
}

bool nxpkg_any(const char *path) {
    bool any = false;
    scan(NULL, path, 0, true, &any);
    return any;
}

// ------------------------------------------------------------------ NCA sections
static const uint8_t *fs_header(const Nca *n, int s) { return n->h + 0x400 + 0x200 * s; }
static bool sec_range(const Nca *n, int s, uint64_t *start, uint64_t *size) {
    const uint8_t *e = n->h + 0x240 + 0x10 * s;
    uint64_t a = (uint64_t)u32(e) * 0x200, b = (uint64_t)u32(e + 4) * 0x200;
    if (b <= a || b > n->size) return false;
    *start = a, *size = b - a;
    return true;
}
static uint64_t upper_iv(const Nca *n, int s) {
    const uint8_t *f = fs_header(n, s);
    return (uint64_t)u32(f + 0x144) << 32 | u32(f + 0x140);  // (secure value, generation)
}

static bool nca_key(NxPkg *P, Nca *n, char *why, size_t wn) {
    if (n->key_state) {
        if (n->key_state < 0) snprintf(why, wn, "no key for %016llx", (unsigned long long)n->id);
        return n->key_state > 0;
    }
    n->key_state = -1;
    int gen = n->h[0x206] > n->h[0x220] ? n->h[0x206] : n->h[0x220];
    int rev = gen ? gen - 1 : 0;
    static const uint8_t zero[16];
    uint8_t k[16];
    if (memcmp(n->h + 0x230, zero, 16)) {  // a rights id: the title key from a ticket
        int t = -1;
        for (int i = 0; i < P->ntk && t < 0; i++)
            if (!memcmp(P->tks[i].rights, n->h + 0x230, 16)) t = i;
        if (t < 0) { snprintf(why, wn, "no ticket / title key for %016llx", (unsigned long long)n->id); return false; }
        if (rev >= NGEN || !(P->keys.have_titlekek >> rev & 1)) {
            snprintf(why, wn, "keys too old: titlekek_%02x missing (%s)", rev, P->keys.path);
            return false;
        }
        ecb_dec(P->keys.titlekek[rev], P->tks[t].key, k, 16);
    } else {
        int kind = n->h[0x207];
        if (kind > 2 || rev >= NGEN || !(P->keys.have_kak[kind] >> rev & 1)) {
            snprintf(why, wn, "keys too old: key_area_key_%s_%02x missing (%s)", kind == 1 ? "ocean" : kind == 2 ? "system" : "application", rev,
                     P->keys.path);
            return false;
        }
        uint8_t area[64];
        ecb_dec(P->keys.kak[kind][rev], n->h + 0x300, area, 64);
        memcpy(k, area + 0x20, 16);  // (the third key: AES-CTR)
    }
    aes_key(&n->key, k);
    n->key_state = 1;
    return true;
}


// Bytes [abs, abs+len) of the NCA (abs: the offset in the NCA), AES-CTR with that upper IV (enc false: as stored).
static void sha_job(void *ctx, const uint8_t *b, size_t n) { sha_update(&((Nca *)ctx)->sha, b, n); }
#define GAP_MAX (64ull << 20)

// The NCA's bytes up to `end` (as stored) into its hash: read for it alone (hash_extra) - the extraction reads ahead.
static void hash_upto(NxPkg *P, Nca *n, uint64_t end) {
    while (n->hon && n->hpos < end) {
        size_t k = end - n->hpos < CHUNK ? (size_t)(end - n->hpos) : CHUNK;
        uint8_t *s = pipe_slot(&P->hp);
        if (!rd(P, n->pkg, n->off + n->hpos, s, k)) { n->hon = false, n->hres = NXPKG_CHECK_UNREAD; return; }
        pipe_push(&P->hp, k, sha_job, n);
        n->hpos += k, P->hash_extra += k;
    }
}

static bool nca_read(NxPkg *P, Nca *n, uint64_t abs, uint8_t *buf, size_t len, bool enc, uint64_t upper, char *why,
                     size_t wn) {
    while (len) {
        uint64_t a0 = abs & ~15ull;
        size_t lead = (size_t)(abs - a0), take = len < CHUNK ? len : CHUNK, raw = (lead + take + 15) & ~(size_t)15;
        // (a small gap: hashed first, in order; a read far ahead isn't hashed now - the hash gets there in order later,
        // or reads what's left at the end - so nothing is read twice when the extraction jumps ahead and comes back)
        if (n->hon && a0 > n->hpos && a0 - n->hpos <= GAP_MAX && a0 + raw <= n->size) hash_upto(P, n, a0);
        if (n->hon && a0 == n->hpos && a0 + raw <= n->size) {  // (in order: read into the hasher's buffer, no copy)
            uint8_t *s = pipe_slot(&P->hp);
            if (!rd(P, n->pkg, n->off + a0, s, raw)) {
                snprintf(why, wn, "read error: %s", P->pkgs[n->pkg].path);
                return false;
            }
            if (!lead && take == raw) enc ? ctr_xor(&n->key, upper, a0 >> 4, s, buf, raw / 16) : (void)memcpy(buf, s, take);
            else {
                if (enc) ctr_xor(&n->key, upper, a0 >> 4, s, P->scratch, raw / 16);
                memcpy(buf, (enc ? P->scratch : s) + lead, take);
            }
            pipe_push(&P->hp, raw, sha_job, n);
            n->hpos = a0 + raw;
            buf += take, abs += take, len -= take;
            continue;
        }
        if (a0 + raw > n->size || !rd(P, n->pkg, n->off + a0, P->scratch, raw)) {
            snprintf(why, wn, "read error: %s", P->pkgs[n->pkg].path);
            return false;
        }
        if (n->hon && a0 < n->hpos && a0 + raw > n->hpos) {  // (the part not hashed yet, as stored: before decrypting)
            size_t skip = (size_t)(n->hpos - a0);
            memcpy(pipe_slot(&P->hp), P->scratch + skip, raw - skip);
            pipe_push(&P->hp, raw - skip, sha_job, n);
            n->hpos = a0 + raw;
        }
        if (enc) ctr_xor(&n->key, upper, a0 >> 4, P->scratch, P->scratch, raw / 16);
        memcpy(buf, P->scratch + lead, take);
        buf += take, abs += take, len -= take;
    }
    return true;
}

static bool sec_ok(const Nca *n, int s, char *why, size_t wn) {
    const uint8_t *f = fs_header(n, s);
    if (u64(f + 0x150) || u64(f + 0x180)) {  // (sparse / compressed storage tables)
        snprintf(why, wn, "a sparse or compressed section in %016llx: not supported", (unsigned long long)n->id);
        return false;
    }
    int enc = f[4];
    if (enc != 1 && enc != 3 && enc != 4) {
        snprintf(why, wn, "section encryption %d in %016llx: not supported", enc, (unsigned long long)n->id);
        return false;
    }
    return true;
}

// Section bytes [off, off+len), plain AES-CTR with the section's IV (or unencrypted).
static bool sec_read(NxPkg *P, Nca *n, int s, uint64_t off, uint8_t *buf, size_t len, char *why, size_t wn) {
    uint64_t start, size;
    if (!sec_range(n, s, &start, &size) || off + len > size) {
        snprintf(why, wn, "read past a section's end (%016llx)", (unsigned long long)n->id);
        return false;
    }
    bool enc = fs_header(n, s)[4] != 1;
    if (enc && !nca_key(P, n, why, wn)) return false;
    return nca_read(P, n, start + off, buf, len, enc, upper_iv(n, s), why, wn);
}

// The section holding the program's code (a PFS0 with main.npdm) / the RomFS (IVFC): its index, -1 none.
static int find_pfs0(NxPkg *P, Nca *n, uint64_t *pfs, uint8_t **tab, uint32_t *cnt, uint32_t *strsz, char *why, size_t wn) {
    for (int s = 0; s < 4; s++) {
        uint64_t start, size;
        const uint8_t *f = fs_header(n, s);
        if (!sec_range(n, s, &start, &size) || f[2] != 1 || f[3] != 2 || !sec_ok(n, s, why, wn)) continue;
        uint32_t layers = u32(f + 8 + 0x24);
        if (layers < 1 || layers > 5) continue;
        uint64_t off = u64(f + 8 + 0x28 + 16 * (layers - 1));
        uint8_t h[16];
        if (!sec_read(P, n, s, off, h, 16, why, wn) || memcmp(h, "PFS0", 4)) continue;
        uint32_t c = u32(h + 4), ss = u32(h + 8);
        if (c > 1024 || ss > (1u << 16)) continue;
        size_t tsz = 16 + (size_t)c * 0x18 + ss;
        uint8_t *t = malloc(tsz);
        if (!t || !sec_read(P, n, s, off, t, tsz, why, wn)) { free(t); continue; }
        *pfs = off, *tab = t, *cnt = c, *strsz = ss;
        return s;
    }
    return -1;
}
static int find_romfs(Nca *n, uint64_t *l6off, uint64_t *l6size) {
    for (int s = 0; s < 4; s++) {
        uint64_t start, size;
        const uint8_t *f = fs_header(n, s);
        if (!sec_range(n, s, &start, &size) || f[2] != 0 || f[3] != 3 || memcmp(f + 8, "IVFC", 4)) continue;
        uint32_t layers = u32(f + 8 + 0xC);
        if (layers < 2 || layers > 7) continue;
        const uint8_t *lv = f + 8 + 0x10 + 0x18 * (layers - 2);  // (the data level: the last)
        *l6off = u64(lv), *l6size = u64(lv + 8);
        return s;
    }
    return -1;
}

static bool pfs0_has(const uint8_t *t, uint32_t cnt, uint32_t strsz, const char *name) {
    const char *str = (const char *)t + 16 + cnt * 0x18;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t no = u32(t + 16 + i * 0x18 + 16);
        if (no < strsz && !strncmp(str + no, name, strsz - no)) return true;
    }
    return false;
}

static Nca *program(NxPkg *P, int upd) { return &P->ncas[upd > 0 && upd <= P->nupd ? P->upd[upd - 1] : P->base]; }

bool nxpkg_write_exefs(NxPkg *P, int upd, const char *dir, char *why, size_t wn) {
    Nca *n = program(P, upd);
    uint64_t pfs;
    uint8_t *t = NULL;
    uint32_t cnt, strsz;
    int s = find_pfs0(P, n, &pfs, &t, &cnt, &strsz, why, wn);
    if (s < 0 || !pfs0_has(t, cnt, strsz, "main.npdm")) {
        if (!why[0]) snprintf(why, wn, "no code (exefs) in %016llx", (unsigned long long)n->id);
        free(t);
        return false;
    }
    mkdir(dir, 0755);
    const char *str = (const char *)t + 16 + cnt * 0x18;
    uint64_t data = pfs + 16 + (uint64_t)cnt * 0x18 + strsz;
    bool ok = true;
    for (uint32_t i = 0; ok && i < cnt; i++) {
        const uint8_t *e = t + 16 + i * 0x18;
        uint32_t no = u32(e + 16);
        if (no >= strsz) continue;
        char name[256], path[1400];
        snprintf(name, sizeof name, "%.*s", (int)(strsz - no), str + no);
        if (!name[0] || strchr(name, '/') || strchr(name, '\\') || !strcmp(name, "..") || !strcmp(name, ".")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, name);
        FILE *o = fopen(path, "wb");
        if (!o) { snprintf(why, wn, "can't write %s", path); ok = false; break; }
        for (uint64_t at = 0, sz = u64(e + 8); ok && at < sz;) {
            size_t k = sz - at < CHUNK ? (size_t)(sz - at) : CHUNK;
            ok = sec_read(P, n, s, data + u64(e) + at, P->buf, k, why, wn);
            if (ok && fwrite(P->buf, 1, k, o) != k) snprintf(why, wn, "can't write %s (disk full?)", path), ok = false;
            at += k;
        }
        if (fclose(o) && ok) snprintf(why, wn, "can't write %s", path), ok = false;
    }
    free(t);
    return ok;
}

// ------------------------------------------------------------------ RomFS (+ the update's patch)
typedef struct { uint64_t virt, phys; int idx; } Ind;           // indirect: patched offset -> base (0) / patch (1)
typedef struct { uint64_t off; uint32_t gen; bool plain; } Ctx;  // AesCtrEx: the patch data's counter generation

// A bucket tree (16 KiB nodes: the index nodes, then the entry sets) in plain AES-CTR at [toff, toff+tsize) of the
// section; the entries copied into *out (esz bytes each), *end = the offset where the last entry ends.
static bool bucket_tree(NxPkg *P, Nca *n, int s, uint64_t toff, uint64_t tsize, const uint8_t *hdr, size_t esz,
                        uint8_t **out, int *count, uint64_t *end, char *why, size_t wn) {
    *out = NULL, *count = 0, *end = 0;
    int32_t cnt = (int32_t)u32(hdr + 8);
    if (memcmp(hdr, "BKTR", 4) || cnt < 0) { snprintf(why, wn, "bad patch table"); return false; }
    if (!cnt) return true;
    const int64_t node = 0x4000, per = (node - 16) / (int64_t)esz, offs = (node - 16) / 8;
    int64_t sets = (cnt + per - 1) / per, l2 = 0;
    if (sets > offs) {
        int64_t nl2 = (sets + offs - 1) / offs;
        l2 = (sets - (offs - (nl2 - 1)) + offs - 1) / offs;
    }
    uint64_t es = (uint64_t)(1 + l2) * node;
    if (es + (uint64_t)sets * node > tsize) { snprintf(why, wn, "bad patch table size"); return false; }
    uint8_t *t = malloc((size_t)sets * node), *o = malloc((size_t)cnt * esz);
    bool ok = t && o && sec_read(P, n, s, toff + es, t, (size_t)sets * node, why, wn);
    int got = 0;
    for (int64_t i = 0; ok && i < sets; i++) {
        const uint8_t *h = t + i * node;
        int32_t c = (int32_t)u32(h + 4);
        if (c < 0 || c > per || got + c > cnt) { snprintf(why, wn, "bad patch table entry set"); ok = false; break; }
        memcpy(o + (size_t)got * esz, h + 16, (size_t)c * esz);
        got += c;
        *end = u64(h + 8);
    }
    free(t);
    if (!ok || got != cnt) { free(o); if (ok) snprintf(why, wn, "patch table short"); return false; }
    *out = o, *count = cnt;
    return true;
}

// Where a RomFS section's bytes come from: a plain section (b, bs), or an update's patched one (u, us): its indirect
// table over the base game's section (b, bs) and the update's own data, whose counters are in the AesCtrEx table.
typedef struct {
    Nca *b, *u;
    int bs, us;
    Ind *ind;
    Ctx *cx;
    int ni, nc;
    uint64_t iend, cend, ustart, usz, sv;
} Src;
// Where src_copy's pieces go: buf gives the buffer for the next (CHUNK + 32 bytes), put takes it filled.
typedef struct Sink {
    uint8_t *(*buf)(struct Sink *);
    bool (*put)(struct Sink *, const uint8_t *b, size_t n, char *why, size_t wn);
} Sink;

static bool src_open(NxPkg *P, Src *S, Nca *b, int bs, Nca *u, int us, char *why, size_t wn) {
    memset(S, 0, sizeof *S);
    S->b = b, S->bs = bs;
    if (!u) return true;
    S->u = u, S->us = us;
    const uint8_t *uf = fs_header(u, us);
    uint8_t *ie = NULL, *ce = NULL;
    bool ok = sec_range(u, us, &S->ustart, &S->usz) && nca_key(P, u, why, wn) &&
              bucket_tree(P, u, us, u64(uf + 0x100), u64(uf + 0x108), uf + 0x110, 0x14, &ie, &S->ni, &S->iend, why, wn) &&
              bucket_tree(P, u, us, u64(uf + 0x120), u64(uf + 0x128), uf + 0x130, 0x10, &ce, &S->nc, &S->cend, why, wn);
    S->ind = ok ? calloc((size_t)S->ni + 1, sizeof *S->ind) : NULL;
    S->cx = ok ? calloc((size_t)S->nc + 1, sizeof *S->cx) : NULL;
    if (ok && (!S->ind || !S->cx)) snprintf(why, wn, "out of memory"), ok = false;
    for (int i = 0; ok && i < S->ni; i++) {
        const uint8_t *e = ie + 0x14 * i;
        S->ind[i] = (Ind){u64(e), u64(e + 8), (int)u32(e + 16)};
        if (S->ind[i].idx > 1 || (i && S->ind[i].virt <= S->ind[i - 1].virt)) snprintf(why, wn, "bad patch table"), ok = false;
    }
    for (int i = 0; ok && i < S->nc; i++) {
        const uint8_t *e = ce + 0x10 * i;
        S->cx[i] = (Ctx){u64(e), u32(e + 12), e[8] == 1};
    }
    S->sv = upper_iv(u, us) & ~0xFFFFFFFFull;
    free(ie), free(ce);
    return ok;
}
static void src_close(Src *S) { free(S->ind), free(S->cx), S->ind = NULL, S->cx = NULL; }

// The section's bytes [a, z) into sk, in order, CHUNK at most at a time.
static bool src_copy(NxPkg *P, Src *S, uint64_t a, uint64_t z, Sink *sk, char *why, size_t wn) {
    if (!S->u) {
        while (a < z) {
            size_t k = z - a < CHUNK ? (size_t)(z - a) : CHUNK;
            uint8_t *b = sk->buf(sk);
            if (!sec_read(P, S->b, S->bs, a, b, k, why, wn) || !sk->put(sk, b, k, why, wn)) return false;
            a += k;
        }
        return true;
    }
    if (!S->ni || S->ind[0].virt > a || S->iend < z) { snprintf(why, wn, "the patch doesn't cover the RomFS"); return false; }
    int lo = 0, hi = S->ni - 1, ci = 0;
    while (lo < hi) {  // (the entry holding a)
        int mid = (lo + hi + 1) / 2;
        if (S->ind[mid].virt <= a) lo = mid; else hi = mid - 1;
    }
    for (int i = lo; i < S->ni && S->ind[i].virt < z; i++) {
        uint64_t v0 = S->ind[i].virt, v1 = i + 1 < S->ni ? S->ind[i + 1].virt : S->iend;
        uint64_t x = v0 > a ? v0 : a, y = v1 < z ? v1 : z;
        if (x >= y) continue;
        uint64_t p = S->ind[i].phys + (x - v0), len = y - x;
        while (len) {
            size_t k;
            uint8_t *b = sk->buf(sk);
            if (S->ind[i].idx == 0) {  // the base game's section
                k = len < CHUNK ? (size_t)len : CHUNK;
                if (!sec_read(P, S->b, S->bs, p, b, k, why, wn)) return false;
            } else {  // patch data: split where the counter generation changes
                if (ci >= S->nc || S->cx[ci].off > p) ci = 0;
                while (ci + 1 < S->nc && S->cx[ci + 1].off <= p) ci++;
                if (!S->nc || S->cx[ci].off > p || p >= S->cend) { snprintf(why, wn, "patch data outside its counter table"); return false; }
                uint64_t lim = ci + 1 < S->nc ? S->cx[ci + 1].off : S->cend;
                k = (size_t)(len < lim - p ? len : lim - p);
                if (k > CHUNK) k = CHUNK;
                if (p + k > S->usz) { snprintf(why, wn, "patch data past its section's end"); return false; }
                if (!nca_read(P, S->u, S->ustart + p, b, k, !S->cx[ci].plain, S->sv | S->cx[ci].gen, why, wn)) return false;
            }
            if (!sk->put(sk, b, k, why, wn)) return false;
            p += k, len -= k;
        }
    }
    return true;
}

// The hash tree (IVFC, in the section's FS header): the master hash, then levels whose 32-byte entries are the
// SHA-256 of the next level's blocks (2^bs bytes, the last one zero-padded); the last level is the data, the image.
typedef struct {
    int n;  // levels, the data's included
    uint64_t off[6], size[6];
    uint32_t bs[6];
    uint8_t master[32];
} Ivfc;
static bool ivfc_info(const uint8_t *fsh, Ivfc *v) {
    const uint8_t *h = fsh + 8;
    uint32_t cnt = u32(h + 0xC);
    if (memcmp(h, "IVFC", 4) || u32(h + 8) != 0x20 || cnt < 3 || cnt > 7) return false;
    v->n = (int)cnt - 1;
    for (int i = 0; i < v->n; i++) {
        const uint8_t *l = h + 0x10 + 0x18 * i;
        v->off[i] = u64(l), v->size[i] = u64(l + 8), v->bs[i] = u32(l + 0x10);
        if (v->bs[i] < 5 || v->bs[i] > 24) return false;
    }
    memcpy(v->master, h + 0xC0, 32);
    return true;
}
static void sha_zeros(Sha *c, uint64_t n) {
    static const uint8_t z[4096];
    for (size_t k; n; n -= k) k = n < sizeof z ? (size_t)n : sizeof z, sha_update(c, z, k);
}
// A level in memory against the hashes above it: false and *bad = the first block that doesn't match.
static bool level_ok(const uint8_t *d, uint64_t size, uint32_t bs, const uint8_t *h, uint64_t hsize, uint64_t *bad) {
    uint64_t bsz = 1ull << bs, nb = (size + bsz - 1) >> bs;
    *bad = 0;
    if (nb * 32 > hsize) return false;
    for (uint64_t j = 0; j < nb; j++) {
        Sha c;
        uint8_t dg[32];
        uint64_t k = size - j * bsz < bsz ? size - j * bsz : bsz;
        sha_init(&c), sha_update(&c, d + j * bsz, (size_t)k), sha_zeros(&c, bsz - k), sha_final(&c, dg);
        if (memcmp(dg, h + 32 * j, 32)) { *bad = j; return false; }
    }
    return true;
}
// The data level as it's written (on the hash tree's thread): each block against its hash
typedef struct {
    const uint8_t *h;
    uint64_t nh, blk, fill, nbad, first_bad;
    uint32_t bs;
    Sha c;
} Iv;
static void iv_block(Iv *v) {
    uint8_t dg[32];
    sha_zeros(&v->c, (1ull << v->bs) - v->fill), sha_final(&v->c, dg);
    if (v->blk >= v->nh || memcmp(dg, v->h + 32 * v->blk, 32))
        if (!v->nbad++) v->first_bad = v->blk;
    v->blk++, v->fill = 0;
}
static void iv_job(void *ctx, const uint8_t *b, size_t n) {
    Iv *v = ctx;
    uint64_t bsz = 1ull << v->bs;
    while (n) {
        if (!v->fill) sha_init(&v->c);
        size_t k = bsz - v->fill < n ? (size_t)(bsz - v->fill) : n;
        sha_update(&v->c, b, k), v->fill += k, b += k, n -= k;
        if (v->fill == bsz) iv_block(v);
    }
}

typedef struct {  // the image file (and the data blocks' check: the pieces are read into its thread's buffers)
    Sink sk;
    NxPkg *P;
    FILE *o;
    uint64_t done, total;
    void (*progress)(uint64_t, uint64_t);
    Iv *iv;
} Out;
static uint8_t *out_buf(Sink *s) {
    Out *w = (Out *)s;
    return w->iv ? pipe_slot(&w->P->vp) : w->P->buf;
}
static bool out_put(Sink *s, const uint8_t *b, size_t n, char *why, size_t wn) {
    Out *w = (Out *)s;
    if (fwrite(b, 1, n, w->o) != n) { snprintf(why, wn, "can't write the RomFS image (disk full?)"); return false; }
    if (w->iv) pipe_push(&w->P->vp, n, iv_job, w->iv);
    w->done += n;
    if (w->progress) w->progress(w->done, w->total);
    return true;
}
typedef struct { Sink sk; NxPkg *P; uint8_t *p; size_t at; } Mem;  // (into memory)
static uint8_t *mem_buf(Sink *s) { return ((Mem *)s)->P->buf; }
static bool mem_put(Sink *s, const uint8_t *b, size_t n, char *why, size_t wn) {
    Mem *m = (Mem *)s;
    (void)why, (void)wn;
    memcpy(m->p + m->at, b, n), m->at += n;
    return true;
}

static FILE *open_out(const char *path, char *why, size_t wn) {
    FILE *o = fopen(path, "wb");
    if (!o) snprintf(why, wn, "can't create %s", path);
    else setvbuf(o, NULL, _IOFBF, 1 << 20);
    return o;
}
static bool close_out(FILE *o, bool ok, const char *path, char *why, size_t wn) {
    if (fclose(o) && ok) snprintf(why, wn, "can't write %s (disk full?)", path), ok = false;
    if (!ok) remove(path);
    return ok;
}

// The image (the data level of S's section, fsh its FS header) into path. With the checks on, the hash tree's levels
// are read and checked first (small: 32 bytes per 16 KiB block), the data blocks as they're written: *image
// NXPKG_CHECK_*, detail what didn't match.
static bool write_image(NxPkg *P, Src *S, const uint8_t *fsh, uint64_t doff, uint64_t dsize, const char *path,
                        void (*progress)(uint64_t, uint64_t), int *image, char *detail, size_t dn, char *why, size_t wn) {
    Ivfc v;
    *image = NXPKG_CHECK_NONE;
    bool check = P->checking && ivfc_info(fsh, &v) && v.off[v.n - 1] == doff && v.size[v.n - 1] == dsize, ok = true;
    uint64_t hsz = 0;
    for (int i = 0; check && i < v.n - 1; i++) hsz += v.size[i];
    if (check && hsz >= (256u << 20)) check = false;
    if (!check && P->checking) snprintf(detail, dn, "a hash tree this doesn't read");
    uint8_t *lv[6] = {0};
    for (int i = 0; check && ok && i < v.n - 1; i++) {
        Mem m = {{mem_buf, mem_put}, P, lv[i] = malloc(v.size[i] + 1), 0};
        if (!lv[i]) check = false;
        else ok = src_copy(P, S, v.off[i], v.off[i] + v.size[i], &m.sk, why, wn);
    }
    Iv iv;
    memset(&iv, 0, sizeof iv);
    if (check && ok) {
        uint64_t bad;
        *image = NXPKG_CHECK_OK;
        for (int i = 0; i < v.n - 1 && *image == NXPKG_CHECK_OK; i++)
            if (!level_ok(lv[i], v.size[i], v.bs[i], i ? lv[i - 1] : v.master, i ? v.size[i - 1] : 32, &bad))
                *image = NXPKG_CHECK_BAD, snprintf(detail, dn, "hash level %d (block %llu) doesn't match the %s", i + 1, (unsigned long long)bad,
                                                   i ? "level above" : "master hash");
        iv.h = lv[v.n - 2], iv.nh = v.size[v.n - 2] / 32, iv.bs = v.bs[v.n - 1];
    }
    FILE *o = ok ? open_out(path, why, wn) : NULL;
    if (o) {
        Out w = {{out_buf, out_put}, P, o, 0, dsize, progress, check ? &iv : NULL};
        ok = src_copy(P, S, doff, doff + dsize, &w.sk, why, wn);
        if (ok && w.done != dsize) snprintf(why, wn, "RomFS size mismatch"), ok = false;
        pipe_wait(&P->vp);  // (iv is here)
        ok = close_out(o, ok, path, why, wn);
    } else ok = false;
    if (check && ok) {
        if (iv.fill) iv_block(&iv);
        if (iv.nbad && *image == NXPKG_CHECK_OK)
            *image = NXPKG_CHECK_BAD, snprintf(detail, dn, "%llu of its %llu blocks (%u KiB) don't match the hash tree, the first at image offset 0x%llx",
                                               (unsigned long long)iv.nbad, (unsigned long long)iv.blk, 1u << iv.bs >> 10,
                                               (unsigned long long)(iv.first_bad << iv.bs));
        else if (*image == NXPKG_CHECK_OK) snprintf(detail, dn, "%llu blocks of %u KiB match the hash tree", (unsigned long long)iv.blk, 1u << iv.bs >> 10);
    }
    if (!ok) *image = NXPKG_CHECK_NONE;
    for (int i = 0; i < 6; i++) free(lv[i]);
    return ok;
}

// ------------------------------------------------------------------ checks
static const char *nca_kind(const Nca *n) {
    static const char *const ct[] = {"meta", "program", "data", "control", "html document", "legal information", "delta fragment"};
    static const char *const nt[] = {"program", "meta", "control", "manual", "data", "public data"};
    return n->ctype >= 0 && n->ctype < 7 ? ct[n->ctype] : n->type < 6 ? nt[n->type] : "?";
}
static int nca_keygen(const Nca *n) { return n->h[0x206] > n->h[0x220] ? n->h[0x206] : n->h[0x220]; }
static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > s) s = b;
    return s ? s + 1 : p;
}

static void check_start(NxPkg *P) {
    if (!P->checking) P->checking = pipe_start(&P->hp) && pipe_start(&P->vp);
}
static void hash_start(NxPkg *P, Nca *n) {
    if (!P->checking || n->hon || n->hres != NXPKG_CHECK_NONE) return;
    sha_init(&n->sha), n->hpos = 0, n->hon = true;
}
// The rest of the NCA, then its SHA-256 against its name (the content id: the first 16 bytes) and its meta's record
static void hash_end(NxPkg *P, Nca *n) {
    if (!n->hon) return;
    hash_upto(P, n, n->size);
    pipe_wait(&P->hp);
    if (!n->hon) return;  // (unreadable: hres says so)
    n->hon = false;
    sha_final(&n->sha, n->digest);
    bool named = n->has_cid || n->ctype >= 0;
    bool bad = (n->has_cid && memcmp(n->digest, n->cid, 16)) || (n->ctype >= 0 && memcmp(n->digest, n->rec_hash, 32));
    n->hres = bad ? NXPKG_CHECK_BAD : named ? NXPKG_CHECK_OK : NXPKG_CHECK_UNNAMED;
}
// A side (the base game, the update): its program NCA and the meta naming it
static int side_result(const NxPkg *P, const Nca *n) {
    if (!n) return NXPKG_CHECK_NONE;
    int r = n->hres, m = n->meta >= 0 ? P->ncas[n->meta].hres : NXPKG_CHECK_NONE;
    if (r == NXPKG_CHECK_BAD || m == NXPKG_CHECK_BAD) return NXPKG_CHECK_BAD;
    if (r == NXPKG_CHECK_UNREAD || m == NXPKG_CHECK_UNREAD) return NXPKG_CHECK_UNREAD;
    return r;
}
static const char *check_text(int r) {
    return r == NXPKG_CHECK_OK ? "ok" : r == NXPKG_CHECK_BAD ? "DAMAGED OR ALTERED" : r == NXPKG_CHECK_UNNAMED ? "not checked (not named by its content id)"
         : r == NXPKG_CHECK_UNREAD ? "unreadable" : "not checked";
}

// Every package and what it holds (name, size, kind; for the NCAs used, the hash result)
static void log_packages(NxPkg *P) {
    for (int i = 0; i < P->npkg; i++) {
        Pkg *k = &P->pkgs[i];
        LOG("[nxpkg] %s: %llu bytes, %s with %d files (header 0x%llx)\n", k->path, (unsigned long long)k->size, k->kind, k->nent,
            (unsigned long long)k->hdr);
        for (int j = 0; j < k->nent; j++) {
            Ent *e = &k->ents[j];
            if (e->nca < 0) {
                LOG("[nxpkg]   %s %llu bytes%s\n", e->name, (unsigned long long)e->size,
                    ext_is(e->name, ".ncz") ? ": compressed (NSZ), not read" : ext_is(e->name, ".nca") ? ": not an NCA3 these keys open" : "");
                continue;
            }
            Nca *n = &P->ncas[e->nca];
            char dg[65] = "";
            if (n->hres != NXPKG_CHECK_NONE && n->hres != NXPKG_CHECK_UNREAD) hex(n->digest, 32, dg);
            LOG("[nxpkg]   %s %llu bytes: %s %016llx v%u, key generation %d - %s%s%s%s\n", e->name, (unsigned long long)e->size, nca_kind(n),
                (unsigned long long)n->id, n->version, nca_keygen(n), n->hres == NXPKG_CHECK_NONE ? "not used" : "used, SHA-256 ",
                n->hres == NXPKG_CHECK_NONE ? "" : dg, n->hres == NXPKG_CHECK_NONE ? "" : ": ", n->hres == NXPKG_CHECK_NONE ? "" : check_text(n->hres));
        }
    }
}

// What a check found, for the log: a side's program NCA
static void log_side(NxPkg *P, const char *what, const Nca *n, int r) {
    if (!n) return;
    const Pkg *k = &P->pkgs[n->pkg];
    const char *name = n->ent < k->nent ? k->ents[n->ent].name : "?";
    if (r == NXPKG_CHECK_BAD)
        LOG("[nxpkg] the %s (%s in %s) is DAMAGED OR ALTERED: its SHA-256 doesn't match its content id%s\n", what, name, k->path,
            n->meta >= 0 && P->ncas[n->meta].hres == NXPKG_CHECK_BAD ? " (or its meta's doesn't)" : "");
    else LOG("[nxpkg] the %s (%s in %s): %s\n", what, name, k->path, check_text(r));
}

const NxCheck *nxpkg_check(const NxPkg *P) { return &P->chk; }

// ------------------------------------------------------------------ writing
bool nxpkg_write_romfs(NxPkg *P, int upd, const char *path, void (*progress)(uint64_t, uint64_t), char *why, size_t wn) {
    Nca *b = &P->ncas[P->base], *u = upd > 0 && upd <= P->nupd ? &P->ncas[P->upd[upd - 1]] : NULL;
    uint64_t boff, bsize, uoff = 0, usize = 0;
    int bs = find_romfs(b, &boff, &bsize), us = u ? find_romfs(u, &uoff, &usize) : -1;
    if (bs < 0 || !sec_ok(b, bs, why, wn)) { if (!why[0]) snprintf(why, wn, "no RomFS in the base game"); return false; }
    const uint8_t *uf = us >= 0 ? fs_header(u, us) : NULL;
    bool whole = uf && uf[4] != 4 && !u64(uf + 0x108);  // (the update's RomFS whole, not a patch: the base game's not used)
    Nca *nb = whole ? NULL : b, *nu = uf ? u : NULL;     // (the base game's / the update's NCA the image comes from)
    if (whole) b = u, bs = us, uf = NULL;
    memset(&P->chk, 0, sizeof P->chk);
    check_start(P);
    Nca *used[2] = {nb, nu};
    for (int i = 0; i < 2; i++)
        if (used[i]) hash_start(P, used[i]);
    Src S;
    int image = NXPKG_CHECK_NONE;
    char detail[256] = "";
    bool ok = src_open(P, &S, b, bs, uf ? u : NULL, us, why, wn) &&
              write_image(P, &S, uf ? uf : fs_header(b, bs), uf || whole ? uoff : boff, uf || whole ? usize : bsize, path, progress, &image,
                          detail, sizeof detail, why, wn);
    src_close(&S);
    for (int i = 0; i < 2; i++)
        if (used[i]) {
            hash_end(P, used[i]);
            if (used[i]->meta >= 0) hash_start(P, &P->ncas[used[i]->meta]), hash_end(P, &P->ncas[used[i]->meta]);
        }
    NxCheck *c = &P->chk;
    c->base = side_result(P, nb), c->update = side_result(P, nu), c->image = image;
    if (nb) snprintf(c->base_file, sizeof c->base_file, "%s", base_name(P->pkgs[nb->pkg].path));
    if (nu) snprintf(c->update_file, sizeof c->update_file, "%s", base_name(P->pkgs[nu->pkg].path));
    snprintf(c->detail, sizeof c->detail, "%s", detail);
    if (P->checking) {
        log_packages(P);
        log_side(P, "base game", nb, c->base);
        log_side(P, "update", nu, c->update);
        if (ok) LOG("[nxpkg] the image against its hash tree: %s (%s)\n", image == NXPKG_CHECK_BAD ? "DOESN'T MATCH" : check_text(image), detail[0] ? detail : "-");
        LOG("[nxpkg] the hashes read %.1f MB the extraction didn't read in order\n", (double)P->hash_extra / 1e6);
    }
    return ok;
}

bool nxpkg_write_dlc(NxPkg *P, const char *path, char *why, size_t wn) {
    if (P->dlc < 0) { snprintf(why, wn, "no add-on content"); return false; }
    Nca *n = &P->ncas[P->dlc];
    uint64_t off, size;
    int s = find_romfs(n, &off, &size);
    if (s < 0 || !sec_ok(n, s, why, wn) || fs_header(n, s)[4] == 4) { if (!why[0]) snprintf(why, wn, "no RomFS in the add-on content"); return false; }
    check_start(P);
    hash_start(P, n);
    Src S;
    int image = NXPKG_CHECK_NONE;
    char detail[256] = "";
    bool ok = src_open(P, &S, n, s, NULL, -1, why, wn) &&
              write_image(P, &S, fs_header(n, s), off, size, path, NULL, &image, detail, sizeof detail, why, wn);
    hash_end(P, n);
    if (n->meta >= 0) hash_start(P, &P->ncas[n->meta]), hash_end(P, &P->ncas[n->meta]);
    int r = side_result(P, n);
    if (P->checking)
        log_side(P, "add-on content", n, r),
            LOG("[nxpkg] the add-on content's image: %s (%s)\n", image == NXPKG_CHECK_BAD ? "DOESN'T MATCH" : check_text(image), detail);
    if (ok && (r == NXPKG_CHECK_BAD || image == NXPKG_CHECK_BAD)) {
        snprintf(why, wn, "the add-on content's file (%s) is damaged or altered", base_name(P->pkgs[n->pkg].path));
        remove(path), ok = false;
    }
    return ok;
}

void nxpkg_log_details(NxPkg *P, int upd, const char *romfs) {
    Nca *list[2] = {&P->ncas[P->base], upd > 0 && upd <= P->nupd ? &P->ncas[P->upd[upd - 1]] : NULL};
    static const char *const what[2] = {"base game", "update"};
    for (int k = 0; k < 2; k++) {
        Nca *n = list[k];
        if (!n) continue;
        const Pkg *pk = &P->pkgs[n->pkg];
        const uint8_t *h = n->h;
        char rights[33], cid[33];
        hex(h + 0x230, 16, rights), hex(n->cid, 16, cid);
        LOG("[nxpkg] details, the %s: %s in %s at 0x%llx, %llu bytes (content id %s%s); header %.4s, distribution %d, content type %d, key "
            "generation %d / %d, key area %d, size 0x%llx, id %016llx, sdk %08x, rights id %s\n",
            what[k], n->ent < pk->nent ? pk->ents[n->ent].name : "?", pk->path, (unsigned long long)n->off, (unsigned long long)n->size,
            n->has_cid ? cid : "-", n->ctype >= 0 ? ", named by its meta" : "", (const char *)h + 0x200, h[0x204], h[0x205], h[0x206], h[0x220],
            h[0x207], (unsigned long long)u64(h + 0x208), (unsigned long long)n->id, u32(h + 0x21C), rights);
        for (int s = 0; s < 4; s++) {
            const uint8_t *e = h + 0x240 + 0x10 * s, *f = fs_header(n, s);
            if (!u32(e) && !u32(e + 4)) continue;
            Sha c;
            uint8_t dg[32];
            sha_init(&c), sha_update(&c, f, 0x200), sha_final(&c, dg);
            LOG("[nxpkg]   section %d: 0x%llx..0x%llx; FS header version %u, fs type %d, hash type %d, encryption %d, upper IV %016llx, its "
                "hash %s\n",
                s, (unsigned long long)u32(e) * 0x200, (unsigned long long)u32(e + 4) * 0x200, f[0] | f[1] << 8, f[2], f[3], f[4],
                (unsigned long long)upper_iv(n, s), memcmp(dg, h + 0x280 + 0x20 * s, 32) ? "DOESN'T MATCH the header's" : "matches the header's");
            Ivfc v;
            if (f[3] == 3 && ivfc_info(f, &v)) {
                char m[65];
                hex(v.master, 32, m);
                for (int i = 0; i < v.n; i++)
                    LOG("[nxpkg]     IVFC level %d: 0x%llx + 0x%llx, blocks of 2^%u\n", i + 1, (unsigned long long)v.off[i], (unsigned long long)v.size[i],
                        v.bs[i]);
                LOG("[nxpkg]     IVFC master hash %s\n", m);
            } else if (f[3] == 2)
                LOG("[nxpkg]     hash table: block 0x%x, layers %u, hashes 0x%llx + 0x%llx, data 0x%llx + 0x%llx\n", u32(f + 8 + 0x20),
                    u32(f + 8 + 0x24), (unsigned long long)u64(f + 8 + 0x28), (unsigned long long)u64(f + 8 + 0x30),
                    (unsigned long long)u64(f + 8 + 0x38), (unsigned long long)u64(f + 8 + 0x40));
            if (u64(f + 0x108) || u64(f + 0x128))
                LOG("[nxpkg]     patch: indirect table 0x%llx + 0x%llx (%.4s, %u entries), counter table 0x%llx + 0x%llx (%.4s, %u entries)\n",
                    (unsigned long long)u64(f + 0x100), (unsigned long long)u64(f + 0x108), (const char *)f + 0x110, u32(f + 0x118),
                    (unsigned long long)u64(f + 0x120), (unsigned long long)u64(f + 0x128), (const char *)f + 0x130, u32(f + 0x138));
            if (u64(f + 0x150) || u64(f + 0x180))
                LOG("[nxpkg]     sparse info at 0x%llx, compression info at 0x%llx\n", (unsigned long long)u64(f + 0x148),
                    (unsigned long long)u64(f + 0x178));
        }
    }
    // the image's RomFS header, and which bytes of the update's patched section its tables came from
    FILE *fi = romfs ? fopen(romfs, "rb") : NULL;
    uint8_t rh[0x50];
    bool got = fi && fread(rh, 1, sizeof rh, fi) == sizeof rh;
    if (fi) fclose(fi);
    if (!got) return;
    static const char *const tn[4] = {"directory hash", "directories", "file hash", "files"};
    LOG("[nxpkg] image %s: RomFS header 0x%llx bytes, directory hash 0x%llx + 0x%llx, directories 0x%llx + 0x%llx, file hash 0x%llx + "
        "0x%llx, files 0x%llx + 0x%llx, file data at 0x%llx\n",
        romfs, (unsigned long long)u64(rh), (unsigned long long)u64(rh + 8), (unsigned long long)u64(rh + 16), (unsigned long long)u64(rh + 24),
        (unsigned long long)u64(rh + 32), (unsigned long long)u64(rh + 40), (unsigned long long)u64(rh + 48), (unsigned long long)u64(rh + 56),
        (unsigned long long)u64(rh + 64), (unsigned long long)u64(rh + 72));
    Nca *u = list[1];
    uint64_t uoff, usize;
    int us = u ? find_romfs(u, &uoff, &usize) : -1;
    if (us < 0 || !u64(fs_header(u, us) + 0x108)) return;
    char why[256] = "";
    Src S;
    if (!src_open(P, &S, list[0], find_romfs(list[0], &(uint64_t){0}, &(uint64_t){0}), u, us, why, sizeof why)) {
        LOG("[nxpkg] patch tables: %s\n", why);
        src_close(&S);
        return;
    }
    uint64_t from[2] = {0, 0};
    for (int i = 0; i < S.ni; i++) {
        uint64_t v0 = S.ind[i].virt, v1 = i + 1 < S.ni ? S.ind[i + 1].virt : S.iend;
        uint64_t x = v0 > uoff ? v0 : uoff, y = v1 < uoff + usize ? v1 : uoff + usize;
        if (x < y) from[S.ind[i].idx & 1] += y - x;
    }
    LOG("[nxpkg] patch: %d indirect entries (to 0x%llx), %d counter entries (to 0x%llx); the image: 0x%llx bytes from the base game, 0x%llx from "
        "the update\n",
        S.ni, (unsigned long long)S.iend, S.nc, (unsigned long long)S.cend, (unsigned long long)from[0], (unsigned long long)from[1]);
    for (int t = -1; t < 4; t++) {  // (-1: the header)
        uint64_t a = uoff + (t < 0 ? 0 : u64(rh + 8 + 16 * t)), z = t < 0 ? uoff + 0x50 : a + u64(rh + 16 + 16 * t);
        int shown = 0;
        for (int i = 0; i < S.ni && shown < 12; i++) {
            uint64_t v0 = S.ind[i].virt, v1 = i + 1 < S.ni ? S.ind[i + 1].virt : S.iend;
            if (v1 <= a || v0 >= z) continue;
            LOG("[nxpkg]   %s: image 0x%llx..0x%llx <- %s 0x%llx\n", t < 0 ? "header" : tn[t], (unsigned long long)((v0 > a ? v0 : a) - uoff),
                (unsigned long long)((v1 < z ? v1 : z) - uoff), S.ind[i].idx ? "update" : "base game",
                (unsigned long long)(S.ind[i].phys + ((v0 > a ? v0 : a) - v0)));
            shown++;
        }
    }
    src_close(&S);
}

// ------------------------------------------------------------------ opening
uint64_t nxpkg_title(const NxPkg *P) { return P->title; }
int nxpkg_update_count(const NxPkg *P) { return P->nupd; }
uint32_t nxpkg_update_version(const NxPkg *P, int upd) { return upd > 0 && upd <= P->nupd ? P->ncas[P->upd[upd - 1]].version : 0; }

void nxpkg_close(NxPkg *P) {
    if (!P) return;
    pipe_stop(&P->hp), pipe_stop(&P->vp);
    for (int i = 0; i < P->npkg; i++) fclose(P->pkgs[i].f), free(P->pkgs[i].ents);
    free(P->pkgs), free(P->ncas), free(P->tks), free(P->scratch), free(P->buf), free(P);
}

// The packages' meta NCAs (a .cnmt in a PFS0: title id, version, type - 0x81 = an update, its title id = the base
// game's | 0x800 - and content records naming the NCAs by content id): version and kind onto the programs they name
// (or, with renamed files, the package's one program of that id). A package without one: an update's RomFS has the
// patch tables.
static void read_versions(NxPkg *P) {
    for (int i = 0; i < P->nnca; i++) {
        Nca *n = &P->ncas[i];
        uint64_t off, size;
        int s = n->type == 0 ? find_romfs(n, &off, &size) : -1;
        n->patch = s >= 0 && u64(fs_header(n, s) + 0x108);
    }
    for (int i = 0; i < P->nnca; i++) {
        Nca *m = &P->ncas[i];
        char why[256] = "";
        uint64_t pfs;
        uint8_t *t = NULL;
        uint32_t cnt, strsz;
        if (m->type != 1) continue;
        int s = find_pfs0(P, m, &pfs, &t, &cnt, &strsz, why, sizeof why);
        if (s >= 0 && cnt) {
            uint8_t h[0x1000];
            const uint8_t *e = t + 16;
            size_t sz = u64(e + 8) < sizeof h ? (size_t)u64(e + 8) : sizeof h;
            if (sz >= 0x20 && sec_read(P, m, s, pfs + 16 + (uint64_t)cnt * 0x18 + strsz + u64(e), h, sz, why, sizeof why) &&
                (h[12] == 0x80 || h[12] == 0x81)) {
                size_t rec = 0x20 + (h[0xE] | h[0xF] << 8);
                int named = 0, nrec = h[0x10] | h[0x11] << 8;
                for (int r = 0; r < nrec && rec + 0x38 * (size_t)(r + 1) <= sz; r++) {
                    const uint8_t *c = h + rec + 0x38 * r;
                    for (int j = 0; j < P->nnca; j++) {
                        Nca *n = &P->ncas[j];
                        if (!n->has_cid || memcmp(n->cid, c + 0x20, 16)) continue;
                        n->meta = i, n->ctype = c[0x36], memcpy(n->rec_hash, c, 32);  // (its record: kind, SHA-256)
                        if (n->type == 0 && c[0x36] == 1) n->version = u32(h + 8), n->patch = h[12] == 0x81, named++;  // (program content)
                    }
                }
                int same = 0, last = -1;
                for (int j = 0; !named && j < P->nnca; j++)
                    if (P->ncas[j].pkg == m->pkg && P->ncas[j].type == 0 && P->ncas[j].id == (u64(h) & ~0x800ull)) same++, last = j;
                if (same == 1) P->ncas[last].version = u32(h + 8), P->ncas[last].patch = h[12] == 0x81, P->ncas[last].meta = i;
            }
        }
        free(t);
    }
}

NxPkg *nxpkg_open(const char *path, const uint64_t *titles, int *err, char *why, size_t wn) {
    why[0] = 0;
    NxPkg *P = calloc(1, sizeof *P);
    P->base = P->dlc = -1;
    P->scratch = malloc(CHUNK + 32), P->buf = malloc(CHUNK + 32);
    char folder[1024];
    struct stat st;
    snprintf(folder, sizeof folder, "%s", path);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        char *sl = strrchr(folder, '/');
        if (sl) *sl = 0; else snprintf(folder, sizeof folder, ".");
    }
    if (!P->scratch || !P->buf) { *err = NXPKG_READ_ERROR, snprintf(why, wn, "out of memory"); nxpkg_close(P); return NULL; }
    if (!keys_find(&P->keys, folder)) {
        *err = NXPKG_NO_KEYS, snprintf(why, wn, "no prod.keys (with header_key) found");
        nxpkg_close(P);
        return NULL;
    }
    LOG("[nxpkg] keys: %s\n", P->keys.path);
    char tk[1100];
    snprintf(tk, sizeof tk, "%s", P->keys.path);
    char *sl = strrchr(tk, '/');
    snprintf(sl ? sl + 1 : tk, sizeof tk - (size_t)(sl ? sl + 1 - tk : 0), "title.keys");
    title_keys_file(P, tk);
    scan(P, path, 0, false, NULL);
    for (int i = 0; i < g_nmore; i++) {  // (nxpkg_add_path: not those inside `path`, read already)
        size_t n = strlen(path);
#ifdef _WIN32
        bool inside = !_strnicmp(g_more[i], path, n) && (!g_more[i][n] || g_more[i][n] == '/');
#else
        bool inside = !strncmp(g_more[i], path, n) && (!g_more[i][n] || g_more[i][n] == '/');
#endif
        if (!inside) scan(P, g_more[i], 0, false, NULL);
    }
    read_versions(P);
    for (int i = 0; i < P->nnca; i++)
        if (P->ncas[i].type == 0 || P->ncas[i].type >= 4)
            LOG("[nxpkg] %s: %016llx %s v%u\n", P->pkgs[P->ncas[i].pkg].path, (unsigned long long)P->ncas[i].id,
                P->ncas[i].type ? "add-on content" : P->ncas[i].patch ? "update" : "base game", P->ncas[i].version);
    for (const uint64_t *t = titles; *t && P->base < 0; t++)
        for (int i = 0; i < P->nnca && P->base < 0; i++)
            if (P->ncas[i].type == 0 && !P->ncas[i].patch && P->ncas[i].id == *t) P->base = i, P->title = *t;
    if (P->base < 0) {
        bool upd_only = false;
        for (const uint64_t *t = titles; *t; t++)
            for (int i = 0; i < P->nnca; i++) upd_only |= P->ncas[i].type == 0 && P->ncas[i].id == *t;
        *err = P->compressed && !upd_only ? NXPKG_COMPRESSED : upd_only ? NXPKG_ONLY_UPDATE : NXPKG_NO_GAME;
        snprintf(why, wn, P->compressed ? "compressed packages (.nsz / .xcz) aren't read" : upd_only ? "only the update found, not the base game"
                          : P->npkg ? "the base game isn't in those packages" : "no packages found");
        log_packages(P);
        nxpkg_close(P);
        return NULL;
    }
    for (int i = 0; i < P->nnca; i++) {
        Nca *n = &P->ncas[i];
        if (n->type == 0 && n->patch && n->id == P->title && P->nupd < 16) {
            int k = P->nupd++;  // (newest first)
            while (k > 0 && P->ncas[P->upd[k - 1]].version < n->version) P->upd[k] = P->upd[k - 1], k--;
            P->upd[k] = i;
        }
        if ((n->type == 4 || n->type == 5) && (n->id & ~0xFFFull) == (P->title & ~0xFFFull) + 0x1000 && P->dlc < 0) P->dlc = i;
    }
    for (int i = -1; i < P->nupd; i++)
        if (!nca_key(P, &P->ncas[i < 0 ? P->base : P->upd[i]], why, wn)) {
            *err = NXPKG_KEY_MISSING;
            log_packages(P);
            nxpkg_close(P);
            return NULL;
        }
    if (P->dlc >= 0 && !nca_key(P, &P->ncas[P->dlc], why, wn)) LOG("[nxpkg] add-on content skipped: %s\n", why), P->dlc = -1, why[0] = 0;
    *err = NXPKG_OK;
    return P;
}
