// Maxwell (SM 5.x) shader binary -> Metal Shading Language, or Vulkan GLSL (key->glsl; contract in shader.h).
//
// The binary is an NVN header (0x30), the shader program header (SPH, 0x50; graphics stages), then
// instructions in groups of four 64-bit words (one scheduling word + three instructions).
// Registers are emitted as `uint` variables; float ops use as_type<> conversions. Shaders with
// branches become a `switch (pc)` state machine; the SSY/PBK/PCNT stack is resolved at translation time (flow_static)
// or kept at run time as one stack, as on the hardware.
//
// GLSL: the instruction body is generated in the MSL spelling and converted by glsl_rewrite() (a token-aware
// pass: as_type<T>() -> as_u/as_f overloads, float4 -> vec4, ulong -> uint64_t, char()/short() casts -> sx8/sx16,
// in.x / out.x stage I/O -> GLSL globals and built-ins), helpers with MSL names (saturate, fmin, clz, mulhi, ...)
// are defined in the GLSL prelude, and the structurally different constructs (texture lookups, global memory,
// half-float packing, subgroup shuffles, barriers, discard, declarations and entry point) are emitted per dialect.
// The MSL text is byte-identical to the MSL-only translator (tools/test_translate.c checks it).
#if defined(_WIN32)
#include <malloc.h>  // alloca
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"

uint64_t maxwell_gmem_base, maxwell_gmem_size;
const uint8_t *maxwell_dbg_code;
uint32_t maxwell_dbg_pc;
int maxwell_dbg_reg;
const uint8_t *maxwell_dbg_vs;

#define RZ 255
#define LMEM_WORDS 256  // local memory per invocation (1 KB)
#define PT 7

typedef struct {
    char *buf;
    size_t len, cap;
} Str;

static void sp(Str *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    vsnprintf(s->buf + s->len, n + 1, fmt, ap2);
    va_end(ap2);
    s->len += n;
}

typedef struct {
    const uint8_t *code;   // first instruction group
    size_t ninsts;         // number of 8-byte words to scan
    int stage;
    const ShaderKey *key;
    ShaderInfo *info;
    Str body;
    uint8_t reg_used[256];
    uint8_t pred_used[8];
    uint32_t in_attr_mask, out_attr_mask;
    uint32_t vs_in_comp[32];
    bool uses_branches, uses_cc, uses_local, uses_vid, uses_iid, uses_frag_coord, uses_front_facing, uses_lane;
    bool ccz_modelled;  // (an instruction that sets the zero / sign flags came before: a BRA may test them)
    bool uses_rscale;  // resolution-scale table (fragment/compute only)
    uint32_t flat_mask;           // fragment inputs using constant (flat) interpolation
    uint32_t noperspective_mask;  // fragment inputs using pass (linear) interpolation
    uint32_t omap_target, omap_depth;
    int tex_slot[8192];           // texture index -> Metal slot + 1
    bool tex_weak[64];            // slot type only guessed (from a size query): the next sample decides
    bool fail;
    bool branchy;  // program uses the pc state machine
    int ipa_mode;  // interpolation mode of the IPA being translated (-1 for plain loads)
    int ipa_mul;   // its multiplier register (mode 1: the result is the "pass" value attr/w times this register)
    // GLSL (fields kept last: the MSL path's layout, including its out-of-range reg_used writes, is unchanged)
    bool glsl;               // emit Vulkan GLSL: dialect-specific constructs here, the rest via glsl_rewrite()
    bool uses_gmem_store;    // STG present: the gmem buffer is not readonly
    uint32_t cbuf_need[18];  // highest constant vec4 index + 1 per cbuf (array size of the GLSL block)
    const uint32_t *flow_to;  // flow_static: where SYNC / BRK / CONT at instruction i go (flow_to[i]); NULL: run-time stacks
    bool fwd;                 // GLSL: blocks in order with reached flags and loops, not the state machine (translate_impl)
    Str fwd_decl;             // (its flags' declarations)
    const size_t *brx_at;     // (c->fwd: each BRX's instruction index and its targets, for its switch)
    uint32_t *const *brx_to;
    const int *brx_nto;
    int nbrx;
} Ctx;

static uint64_t bits(uint64_t w, int lo, int n) { return (w >> lo) & ((n == 64) ? ~0ull : ((1ull << n) - 1)); }
static int64_t sbits(uint64_t w, int lo, int n) {
    int64_t v = (int64_t)bits(w, lo, n);
    return (v & (1ll << (n - 1))) ? v - (1ll << n) : v;
}

// A jump from pc `from` to pc `to` (0: nowhere, the end): the state machine's next case, or (c->fwd) that block's flag
// set - back to a loop's start: also its loop's flag - and the rest of this block skipped
static void jump_set(char *set, size_t n, uint32_t to, uint32_t from) {
    if (!to) snprintf(set, n, "exit_now = true;");
    else if (to <= from) snprintf(set, n, "b%u = true; l%u = true;", to, to);
    else snprintf(set, n, "b%u = true;", to);
}
static void jump(Ctx *c, const char *cond, uint32_t to, uint32_t from) {
    if (c->fwd) {
        char set[64];
        jump_set(set, sizeof set, to, from);
        if (cond) sp(&c->body, "    if (%s) { %s break; }\n", cond, set);
        else sp(&c->body, "    %s break;\n", set);
    } else sp(&c->body, cond ? "    if (%s) { pc = %uu; continue; }\n" : "%s    pc = %uu; continue;\n", cond ? cond : "", to);
}

static void fail(Ctx *c, const char *fmt, ...) {
    if (c->fail) return;
    c->fail = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->info->error, sizeof c->info->error, fmt, ap);
    va_end(ap);
}

// Branch targets may point at a scheduling word; execution continues at the next instruction.
static uint32_t norm_target(int64_t t) {
    if (t >= 0 && ((t / 8) % 4) == 0) t += 8;
    return (uint32_t)t;
}

// ------------------------------------------------------------------ operands
static const char *reg(Ctx *c, int r, char *buf) {
    if (r == RZ) return "0u";
    c->reg_used[r] = 1;
    sprintf(buf, "r%d", r);
    return buf;
}

#define R(r) reg(c, (r), alloca(16))
#define F(expr) fmt_f(alloca(512), (expr))
static char *fmt_f(char *b, const char *e) { sprintf(b, "as_type<float>(%s)", e); return b; }

// VMNMX operand: byte / halfword / word `sel` of x (bit 3: signed), zero- or sign-extended to 32 bits
static void video_sel(char *o, const char *x, int sel) {
    int base = sel & 7, sgn = sel & 8;
    if (base <= 3) {
        if (sgn) sprintf(o, "uint(int(%s << %du) >> 24)", x, 24 - 8 * base);
        else sprintf(o, "((%s >> %du) & 0xffu)", x, 8 * base);
    } else if (base <= 5) {
        if (sgn) sprintf(o, "uint(int(%s << %du) >> 16)", x, 16 - 16 * (base - 4));
        else sprintf(o, "((%s >> %du) & 0xffffu)", x, 16 * (base - 4));
    } else {
        sprintf(o, "(%s)", x);
    }
}

static const char *pred(Ctx *c, int p, bool neg, char *buf) {
    if (p == PT) return neg ? "false" : "true";
    c->pred_used[p] = 1;
    sprintf(buf, "%sp%d", neg ? "!" : "", p);
    return buf;
}
#define P(p, n) pred(c, (p), (n), alloca(16))

static const char *cbuf_ref(Ctx *c, int idx, int64_t off, const char *dyn, char *buf) {
    c->info->cbuf_mask |= 1u << idx;
    if (off + 16 > (int64_t)c->info->cbuf_size[idx]) c->info->cbuf_size[idx] = (uint32_t)((off + 16 + 255) & ~255);
    if (dyn) c->info->cbuf_dynamic |= 1u << idx;
    if (!dyn && idx < 18 && off >= 0 && (uint32_t)(off >> 4) + 1 > c->cbuf_need[idx]) c->cbuf_need[idx] = (uint32_t)(off >> 4) + 1;
    // c0[0x644] in a vertex shader: NVN's base instance (the shader adds it to the instance id, which excludes it).
    // Vulkan has it as gl_BaseInstance - the only source for indirect draws, whose base instance the GPU writes (Z-A's
    // instanced props: every instance drew at the first one's place). (c0[0x640], the base vertex, stays 0:
    // gl_VertexIndex already includes it.)
    if (!dyn && idx == 0 && off == 0x644 && c->stage == STAGE_VERTEX && c->glsl) { sprintf(buf, "uint(gl_BaseInstanceARB)"); return buf; }
    if (dyn) sprintf(buf, "cbr(cb%d, (%s) + %lldu)", idx, dyn, (long long)off);
    else sprintf(buf, "cb%d[%lld][%lld]", idx, (long long)(off >> 4), (long long)((off >> 2) & 3));
    return buf;
}

// Operand B in its register / constant buffer / immediate forms, selected by the opcode class.
typedef enum { B_REG, B_CBUF, B_IMM } BForm;

static BForm b_form(uint64_t w) {
    uint64_t top = w >> 60;
    if (top == 0x5) return B_REG;
    if (top == 0x4) return B_CBUF;
    return B_IMM;
}

static const char *operand_b(Ctx *c, uint64_t w, bool float_imm, char *buf) {
    switch (b_form(w)) {
    case B_REG: return reg(c, (int)bits(w, 20, 8), buf);
    case B_CBUF: return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    default: {
        uint32_t v = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19);
        if (float_imm) v <<= 12;
        else if (v & 0x80000u) v |= 0xFFF00000u;  // sign-extend 20 bits
        sprintf(buf, "0x%08xu", v);
        return buf;
    }
    }
}
#define B(fi) operand_b(c, w, (fi), alloca(128))

// The "C" operand of three-source ops: REG_39, or the cbuf/reg swap form (0x51/0x53/... classes).
static const char *operand_c_for_ffma(Ctx *c, uint64_t w, char *buf, const char **b_out, char *bbuf) {
    uint64_t op8 = w >> 56;
    if ((op8 & 0xF8) == 0x50 && (op8 & 7) != 0) {  // e.g. 0x51: B = REG_39, C = cbuf
        *b_out = reg(c, (int)bits(w, 39, 8), bbuf);
        return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    }
    *b_out = operand_b(c, w, true, bbuf);
    return reg(c, (int)bits(w, 39, 8), buf);
}

static void set_reg(Ctx *c, int r, const char *expr) {
    if (r == RZ) return;
    c->reg_used[r] = 1;
    sp(&c->body, "    r%d = %s;\n", r, expr);
}

static const char *fneg_abs(const char *e, bool neg, bool abs_, char *buf) {
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", e);
    else if (abs_) sprintf(buf, "abs(%s)", e);
    else if (neg) sprintf(buf, "(-%s)", e);
    else return e;
    return buf;
}
#define NA(e, n, a) fneg_abs((e), (n), (a), alloca(600))

static const char *fresult(const char *e, bool sat, char *buf) {
    if (sat) sprintf(buf, "as_type<uint>(saturate(%s))", e);
    else sprintf(buf, "as_type<uint>(%s)", e);
    return buf;
}
#define FRES(e, sat) fresult((e), (sat), alloca(700))

static const char *cmp_op(int op) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "num", "nan",
                                      "<", "==", "<=", ">", "!=", ">=", "true"};
    return ops[op & 15];
}

// float comparison including unordered variants
static void fcmp_expr(char *out, int op, const char *a, const char *b) {
    switch (op) {
    case 0: strcpy(out, "false"); break;
    case 15: strcpy(out, "true"); break;
    case 7: sprintf(out, "(!isnan(%s) && !isnan(%s))", a, b); break;
    case 8: sprintf(out, "(isnan(%s) || isnan(%s))", a, b); break;
    default:
        if (op < 7) sprintf(out, "(%s %s %s)", a, cmp_op(op), b);
        else sprintf(out, "((%s %s %s) || isnan(%s) || isnan(%s))", a, cmp_op(op), b, a, b);
    }
}

static const char *bool_op(int op) { return op == 0 ? "&&" : op == 1 ? "||" : "!="; }

static void icmp_expr(char *out, int op, const char *a, const char *b, bool is_signed) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    if (op == 0) strcpy(out, "false");
    else if (op == 7) strcpy(out, "true");
    else if (is_signed) sprintf(out, "(int(%s) %s int(%s))", a, ops[op], b);
    else sprintf(out, "(%s %s %s)", a, ops[op], b);
}

// ------------------------------------------------------------------ attributes
// Maxwell attribute space: 0x70 position, 0x80 + 16*i generic attribute i, 0x2F8 instance id,
// 0x2FC vertex id, 0x3FC front facing, 0x6C point size.
static void attr_load(Ctx *c, int dst, uint32_t addr, const char *idx) {
    char e[256];
    if (idx) { fail(c, "indexed attribute load"); return; }
    if (c->stage == STAGE_VERTEX) {
        if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            c->in_attr_mask |= 1u << a;
            c->vs_in_comp[a] |= 1u << comp;
            int kind = c->key ? c->key->attr_kind[a] : 0;
            if (kind == 0) sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
            else sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
        } else if (addr == 0x2F8) { c->uses_iid = true; strcpy(e, "iid"); }
        else if (addr == 0x2FC) { c->uses_vid = true; strcpy(e, "vid"); }
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else if (c->stage == STAGE_FRAGMENT) {
        if (addr >= 0x70 && addr < 0x80) {
            c->uses_frag_coord = true;
            int comp = (addr >> 2) & 3;
            if (comp < 2) {  // guest pixel units: the render target may be allocated larger
                c->uses_rscale = true;
                sprintf(e, "as_type<uint>((in.pos[%d] - rscale[0][%d]) / rscale[0][%d])", comp, comp + 2, comp);
            } else {
                sprintf(e, "as_type<uint>(in.pos[%d])", comp);
            }
        } else if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            // (MSL: a mask of 0 means unknown. GLSL: the renderer always passes the vertex shader's outputs, and
            // Vulkan requires every fragment input to be a vertex output, so 0 means none.)
            if (c->key && (c->key->vs_out_mask || c->key->glsl) && !(c->key->vs_out_mask & (1u << a))) {
                strcpy(e, comp == 3 ? "0x3f800000u" : "0u");  // not provided by the vertex shader
            } else {
                c->in_attr_mask |= 1u << a;
                // IPA "pass" yields attr/w (the shader multiplies by w itself); Metal gives attr. "multiply" is the
                // pass value times a register - usually w (Metal's attr again), but a shader may pass its own, e.g. a
                // projective screen lookup (LAS Recomp d1c2d8e: attr * pos.w * multiplier, as the hardware does)
                if (c->ipa_mode == 0) sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w)", a, comp);
                else if (c->ipa_mode == 1 && c->ipa_mul != RZ) {
                    char mb[16];
                    sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w * as_type<float>(%s))", a, comp, reg(c, c->ipa_mul, mb));
                } else sprintf(e, "as_type<uint>(in.v%d[%d])", a, comp);
            }
        } else if (addr == 0x3FC) { c->uses_front_facing = true; strcpy(e, "(front_facing ? 0xFFFFFFFFu : 0u)"); }
        else if (addr == 0x2E0 || addr == 0x2E4) { sprintf(e, "as_type<uint>(point_coord[%d])", (addr >> 2) & 1); }
        else if (addr == 0x64 && c->glsl) { strcpy(e, "uint(gl_Layer)"); }  // the layer drawn (Z-A's LUT: the slice = blue)
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else {
        sprintf(e, "0u /* attr %#x */", addr);
    }
    set_reg(c, dst, e);
}

static void attr_store(Ctx *c, int src, uint32_t addr) {
    char b[16];
    const char *v = reg(c, src, b);
    if (c->glsl && c->stage != STAGE_VERTEX) {  // no vertex outputs in this stage (MSL: undeclared `out`)
        if (addr >= 0x80 && addr < 0x280) c->out_attr_mask |= 1u << ((addr - 0x80) >> 4);
        if (addr == 0x6C) c->info->writes_point_size = true;
        sp(&c->body, "    /* store attr %#x */\n", addr);
        return;
    }
    if (addr >= 0x70 && addr < 0x80) {
        sp(&c->body, "    out.pos[%d] = as_type<float>(%s);\n", (addr >> 2) & 3, v);
    } else if (addr >= 0x80 && addr < 0x280) {
        int a = (addr - 0x80) >> 4;
        c->out_attr_mask |= 1u << a;
        sp(&c->body, "    out.v%d[%d] = as_type<float>(%s);\n", a, (addr >> 2) & 3, v);
    } else if (addr == 0x6C) {
        c->info->writes_point_size = true;
        sp(&c->body, "    out.psize = as_type<float>(%s);\n", v);
    } else if (addr == 0x64 && c->glsl) {  // render target layer (array / 3D slice)
        sp(&c->body, "    gl_Layer = int(%s);\n", v);
    } else {
        sp(&c->body, "    /* store attr %#x */\n", addr);
    }
}


// ------------------------------------------------------------------ half precision (SM53)
// Swizzle: 0 H1_H0 (both halves), 1 F32 (one f32 used for both lanes), 2 H0_H0, 3 H1_H1.
// Merge: 0 H1_H0 (pack both), 1 F32 (lane 0 as f32), 2 MRG_H0 (replace low half), 3 MRG_H1 (replace high half).
static const char *hsrc(Ctx *c, const char *v, int swz, bool neg, bool abs_, char *buf) {
    (void)c;
    char t[300];
    sprintf(t, "hx(%s, %du)", v, swz);
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", t);
    else if (abs_) sprintf(buf, "abs(%s)", t);
    else if (neg) sprintf(buf, "(-%s)", t);
    else strcpy(buf, t);
    return buf;
}
#define HS(v, sw, n, ab) hsrc(c, (v), (sw), (n), (ab), alloca(400))

static void hstore(Ctx *c, int d, const char *expr, int merge, bool sat) {
    char t[2048], e[1700];
    if (sat) sprintf(e, "saturate(%s)", expr); else strcpy(e, expr);
    if (d == RZ) return;
    c->reg_used[d] = 1;
    sprintf(t, "hpack(%s, %du, r%d)", e, merge, d);
    set_reg(c, d, t);
}

// immediate halves: 9 bits of each half's top (the low 6 mantissa bits are zero) plus separate signs
static uint32_t himm(uint64_t w) {
    uint32_t lo = ((uint32_t)bits(w, 20, 9) << 6) | ((uint32_t)bits(w, 29, 1) << 15);
    uint32_t hi = ((uint32_t)bits(w, 30, 9) << 6) | ((uint32_t)bits(w, 56, 1) << 15);
    return lo | (hi << 16);
}

static bool emit_half(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    char t[1800], ib[32], cb[256];
    uint16_t op = (uint16_t)(w >> 48);
    // ---- HADD2 / HMUL2 (reg 5d10/5d08, cbuf 7a8x/788x, imm 7a0x/780x, 32I 2c/2a)
    int kind = -1;  // 0 add, 1 mul
    int form = -1;  // 0 reg, 1 cbuf, 2 imm, 3 32i
    if ((op & 0xfff8) == 0x5d10) { kind = 0; form = 0; }
    else if ((op & 0xfff8) == 0x5d08) { kind = 1; form = 0; }
    else if ((op & 0xfe80) == 0x7a80) { kind = 0; form = 1; }
    else if ((op & 0xfe80) == 0x7a00) { kind = 0; form = 2; }
    else if ((op & 0xfe80) == 0x7880) { kind = 1; form = 1; }
    else if ((op & 0xfe80) == 0x7800) { kind = 1; form = 2; }
    else if ((op & 0xfe00) == 0x2c00) { kind = 0; form = 3; }
    else if ((op & 0xfe00) == 0x2a00) { kind = 1; form = 3; }
    if (kind >= 0) {
        int merge, swa, swb = 0;
        bool sat, nega, absa = false, negb = false, absb = false;
        const char *bv;
        if (form == 3) {
            merge = 0; sat = bits(w, 52, 1); swa = (int)bits(w, 53, 2); nega = kind == 0 ? bits(w, 56, 1) : false;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib;
        } else {
            // field layout per Ryujinx's InstDecoders (InstHadd2*/InstHmul2*)
            merge = (int)bits(w, 49, 2); swa = (int)bits(w, 47, 2); nega = bits(w, 43, 1); absa = bits(w, 44, 1);
            if (form == 0) {
                sat = bits(w, 32, 1); absb = bits(w, 30, 1); swb = (int)bits(w, 28, 2);
                if (kind == 0) negb = bits(w, 31, 1);
                else { nega = bits(w, 31, 1); negb = false; }  // HMUL2 reg: the negate bit is A's at 31
                bv = R((int)bits(w, 20, 8));
            } else if (form == 1) {
                sat = bits(w, 52, 1); negb = kind == 0 ? bits(w, 56, 1) : false; absb = bits(w, 54, 1); swb = 1;
                bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            } else {
                sat = bits(w, 52, 1); swb = 0;
                sprintf(ib, "0x%08xu", himm(w)); bv = ib;
            }
        }
        sprintf(t, kind == 0 ? "(%s + %s)" : "(%s * %s)", HS(R(a), swa, nega, absa), HS(bv, swb, negb, absb));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HFMA2 (reg 5d00, cr 70xx|0x80, rc 60xx|0x80, imm 70xx, 32I 28)
    form = -1;
    if ((op & 0xfff8) == 0x5d00) form = 0;
    else if ((op & 0xf880) == 0x7080) form = 1;  // b = cbuf, c = reg
    else if ((op & 0xf880) == 0x6080) form = 2;  // b = reg, c = cbuf
    else if ((op & 0xf880) == 0x7000) form = 3;  // b = imm, c = reg
    else if ((op & 0xfe00) == 0x2800) form = 4;  // 32I: c = d
    if (form >= 0) {
        int merge, swa = (int)bits(w, 47, 2), swb = 0, swc = 0;
        bool sat, negb, negc;
        const char *bv, *cv;
        char cb2[256];
        switch (form) {  // field layout per Ryujinx's InstDecoders (InstHfma2*); NegA is applied to B (same product)
        case 0:
            merge = (int)bits(w, 49, 2); sat = bits(w, 32, 1); negb = bits(w, 31, 1); negc = bits(w, 30, 1);
            swb = (int)bits(w, 28, 2); swc = (int)bits(w, 35, 2);
            bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8));
            break;
        case 1:  // b = cbuf, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = 1; swc = (int)bits(w, 53, 2);
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = R((int)bits(w, 39, 8));
            break;
        case 2:  // b = reg (field at 39), c = cbuf
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = (int)bits(w, 53, 2); swc = 1;
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb2);
            break;
        case 3:  // b = imm, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = false; negc = bits(w, 51, 1);
            swb = 0; swc = (int)bits(w, 53, 2);
            sprintf(ib, "0x%08xu", himm(w)); bv = ib; cv = R((int)bits(w, 39, 8));
            break;
        default:  // 32I: c = d
            merge = 0; sat = false; negb = false; negc = bits(w, 51, 1);
            swa = (int)bits(w, 47, 2); swb = 0; swc = 0;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib; cv = R(d);
            break;
        }
        sprintf(t, "fma(%s, %s, %s)", HS(R(a), swa, false, false), HS(bv, swb, negb, false), HS(cv, swc, negc, false));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HSETP2 / HSET2 (reg forms)
    if ((op & 0xfff8) == 0x5d20 || (op & 0xfff8) == 0x5d18) {
        bool setp = (op & 0xfff8) == 0x5d20;
        const char *x = HS(R(a), (int)bits(w, 47, 2), bits(w, 43, 1), bits(w, 44, 1));
        const char *y = HS(R((int)bits(w, 20, 8)), (int)bits(w, 28, 2), bits(w, 31, 1), bits(w, 30, 1));
        int cop = (int)bits(w, 35, 4), bop = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        char lx[512], ly[512];
        sp(&c->body, "    { float2 hx_ = %s, hy_ = %s; bool pp = %s;\n", x, y, pp);
        fcmp_expr(lx, cop, "hx_.x", "hy_.x");
        fcmp_expr(ly, cop, "hx_.y", "hy_.y");
        sp(&c->body, "      bool l = %s, h = %s;\n", lx, ly);
        if (setp) {
            int pa = (int)bits(w, 3, 3), pb = (int)bits(w, 0, 3);
            if (bits(w, 49, 1)) {  // .H_AND: each half is combined with the predicate first, then they're ANDed
                sp(&c->body, "      l = (l %s pp) && (h %s pp); h = !l;\n", bool_op(bop), bool_op(bop));
                if (pa != PT) { c->pred_used[pa] = 1; sp(&c->body, "      p%d = l;\n", pa); }
                if (pb != PT) { c->pred_used[pb] = 1; sp(&c->body, "      p%d = h;\n", pb); }
            } else {
                if (pa != PT) { c->pred_used[pa] = 1; sp(&c->body, "      p%d = l %s pp;\n", pa, bool_op(bop)); }
                if (pb != PT) { c->pred_used[pb] = 1; sp(&c->body, "      p%d = h %s pp;\n", pb, bool_op(bop)); }
            }
        } else {
            bool bf = bits(w, 49, 1);
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      l = l %s pp; h = h %s pp;\n", bool_op(bop), bool_op(bop));
                sp(&c->body, bf ? "      r%d = (l ? 0x3c00u : 0u) | (h ? 0x3c000000u : 0u);\n"
                                : "      r%d = (l ? 0xFFFFu : 0u) | (h ? 0xFFFF0000u : 0u);\n", d);
            }
        }
        sp(&c->body, "    }\n");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ textures
static int tex_slot(Ctx *c, uint32_t index, int type, bool shadow, bool array) {
    if (index >= 8192) return 0;
    if (c->tex_slot[index]) {
        int s = c->tex_slot[index] - 1;
        if (type >= 0 && c->tex_weak[s]) {
            ShaderTexture *t = &c->info->tex[s];
            t->type = (uint8_t)type;
            t->shadow = shadow;
            t->is_array = array;
            c->tex_weak[s] = false;
        } else if (c->glsl && type >= 0 && (c->info->tex[s].type != type || c->info->tex[s].shadow != shadow)) {
            fail(c, "texture %u used as two types", index);  // one GLSL sampler type per binding
        }
        return s;
    }
    bool weak = type < 0;
    if (weak) type = TEXTYPE_2D;
    int s = c->info->ntex++;
    if (s >= 31) { fail(c, "too many textures"); return 0; }
    c->tex_slot[index] = s + 1;
    c->tex_weak[s] = weak;
    ShaderTexture *t = &c->info->tex[s];
    t->handle_offset = index * 4;
    int nslot = ((int)index * 4 - 0x20) / 8;
    t->stencil = c->glsl && c->key && nslot >= 0 && nslot < 32 && (c->key->tex_stencil >> nslot & 1);
    t->type = (uint8_t)type;
    t->shadow = shadow;
    t->is_array = array;
    return s;
}

static const char *tex_type_name(int t) {
    switch (t) {
    case TEXTYPE_1D: return "1d";
    case TEXTYPE_3D: return "3d";
    case TEXTYPE_CUBE: return "cube";
    case TEXTYPE_2D_ARRAY: return "2d_array";
    case TEXTYPE_CUBE_ARRAY: return "cube_array";
    case TEXTYPE_1D_ARRAY: return "1d_array";
    default: return "2d";
    }
}

// Write texture results: component mask selects which of r,g,b,a go to consecutive registers.
static void write_tex_result(Ctx *c, int slot, const char *call, uint32_t mask, int d0, int d1, bool texs, bool f16) {
    // (a stencil view: integer results, the stencil value in .x - which the game takes as is, e.g. material bits)
    sp(&c->body, "    { %s t = %s;\n", slot >= 0 && slot < c->info->ntex && c->info->tex[slot].stencil ? "uint4" : "float4", call);
    if (f16) {
        // TEXS/TLDS .F16: components (in mask order) packed as half pairs into d0 (lo, hi) and d1 (lo, hi)
        int comps[4], n = 0;
        for (int comp = 0; comp < 4; comp++) if (mask & (1u << comp)) comps[n++] = comp;
        char v[4][16];
        for (int i = 0; i < 4; i++) { if (i < n) sprintf(v[i], "t[%d]", comps[i]); else strcpy(v[i], "0.0f"); }
        const char *pk = c->glsl ? "      r%d = packHalf2x16(vec2(%s, %s));\n" : "      r%d = as_type<uint>(half2(half(%s), half(%s)));\n";
        if (d0 != RZ) { c->reg_used[d0] = 1; sp(&c->body, pk, d0, v[0], v[1]); }
        if (d1 != RZ) { c->reg_used[d1] = 1; sp(&c->body, pk, d1, v[2], v[3]); }
        sp(&c->body, "    }\n");
        return;
    }
    int regs[4], n = 0;
    if (texs) {
        // TEXS: up to two components in d0/d0+1, two more in d1/d1+1
        int out[4] = {d0, d0 == RZ ? RZ : d0 + 1, d1, d1 == RZ ? RZ : d1 + 1};
        for (int i = 0; i < 4; i++) regs[i] = out[i];
    } else {
        for (int i = 0; i < 4; i++) regs[i] = d0 == RZ ? RZ : d0 + i;
    }
    for (int comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        int r = regs[n++];
        if (r != RZ) { c->reg_used[r] = 1; sp(&c->body, "      r%d = as_type<uint>(t[%d]);\n", r, comp); }
    }
    sp(&c->body, "    }\n");
}

// Integer texel coordinates in guest units -> the (possibly larger) allocation of texture slot `slot`.
static const char *RSC2(Ctx *c, int slot, const char *fmt, ...) {
    static __thread char bufs[4][1024];  // (per thread: programs translate on several workers at once)
    static __thread int bi;
    char *o = bufs[bi++ & 3];
    char inner[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(inner, sizeof inner, fmt, ap);
    va_end(ap);
    if (c->stage == STAGE_VERTEX) { snprintf(o, 1024, "%s", inner); return o; }
    c->uses_rscale = true;
    snprintf(o, 1024, "rsc(%s, rscale[%d])", inner, slot + 1);
    return o;
}

// TEXS / TLDS: texture_info selects target and coordinate layout
static const char *alayer(Ctx *c, const char *r, int slot, char *buf) {
    if (c->glsl) sprintf(buf, "min(%s, uint(tsize%d(0).z) - 1u)", r, slot);
    else sprintf(buf, "min(%s, t%d.get_array_size() - 1u)", r, slot);
    return buf;
}
#define ALAYER(r, slot) alayer(c, (r), (slot), alloca(96))

// GLSL texture lookup on slot `slot` (its type is final here: a conflicting use fails in tex_slot).
// co: coordinate expressions (1 for 1D, 2 for 2D, 3 for 3D / cube), layer: uint expression or NULL, dref: depth
// reference or NULL, lod_kind 0 implicit / 1 level / 2 bias (lod), off: ivec2 texel offset or NULL.
// GLSL offsets must be constants, so register offsets move the coordinates by offset / size of the level
// (level 0 for implicit-LOD lookups). Non-fragment stages have no implicit LOD: level 0 (+ bias).
static void glsl_sample(Ctx *c, char *out, int slot, const char *const *co, const char *layer, const char *dref,
                        int lod_kind, const char *lod, const char *off) {
    if (c->fail) { strcpy(out, "vec4(0.0f)"); return; }  // e.g. a type conflict: tex[slot] is not this lookup's type
    const ShaderTexture *t = &c->info->tex[slot];
    int type = t->type;
    bool cube = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY;
    int n = (type == TEXTYPE_1D || type == TEXTYPE_1D_ARRAY) ? 1 : (type == TEXTYPE_2D || type == TEXTYPE_2D_ARRAY) ? 2 : 3;
    if (c->stage != STAGE_FRAGMENT && lod_kind != 1) {
        if (lod_kind == 0) lod = "0.0f";
        lod_kind = 1;
    }
    if (t->shadow && type == TEXTYPE_3D) { fail(c, "3d shadow texture"); strcpy(out, "vec4(0.0f)"); return; }
    if (layer && type != TEXTYPE_1D_ARRAY && type != TEXTYPE_2D_ARRAY && type != TEXTYPE_CUBE_ARRAY) {
        fail(c, "array lookup on a 3d texture");  // TEX .3D with the array bit
        strcpy(out, "vec4(0.0f)");
        return;
    }
    char L[600], xs[1600], P[2000];
    if (lod_kind == 1) snprintf(L, sizeof L, "int(%s)", lod); else strcpy(L, "0");
    if (n == 1) {
        if (off) snprintf(xs, sizeof xs, "(%s + float((%s).x) / float(tsize%d(%s).x))", co[0], off, slot, L);
        else snprintf(xs, sizeof xs, "%s", co[0]);
    } else if (n == 2) {
        if (off) snprintf(xs, sizeof xs, "(vec2(%s, %s) + vec2(%s) / vec2(tsize%d(%s).xy))", co[0], co[1], off, slot, L);
        else snprintf(xs, sizeof xs, "vec2(%s, %s)", co[0], co[1]);
    } else {
        if (off && !cube) snprintf(xs, sizeof xs, "vec3(vec2(%s, %s) + vec2(%s) / vec2(tsize%d(%s).xy), %s)", co[0], co[1], off, slot, L, co[2]);
        else snprintf(xs, sizeof xs, "vec3(%s, %s, %s)", co[0], co[1], co[2]);  // cube lookups take no offset
    }
    const char *extra = NULL;  // separate compare argument (cube array shadow)
    if (!t->shadow) {
        if (layer) snprintf(P, sizeof P, "vec%d(%s, float(%s))", n + 1, xs, layer);
        else snprintf(P, sizeof P, "%s", xs);
    } else if (type == TEXTYPE_CUBE_ARRAY) {
        snprintf(P, sizeof P, "vec4(%s, float(%s))", xs, layer ? layer : "0u");
        extra = dref;
    } else if (type == TEXTYPE_1D) {
        snprintf(P, sizeof P, "vec3(%s, 0.0f, %s)", xs, dref);
    } else if (layer) {
        snprintf(P, sizeof P, "vec%d(%s, float(%s), %s)", n + 2, xs, layer, dref);
    } else {
        snprintf(P, sizeof P, "vec%d(%s, %s)", n + 1, xs, dref);
    }
    char e[2000], ex[300] = "";
    if (extra) snprintf(ex, sizeof ex, ", %s", extra);
    if (lod_kind == 0) snprintf(e, sizeof e, "texture(t%d, %s%s)", slot, P, ex);
    else if (lod_kind == 1) snprintf(e, sizeof e, "textureLod(t%d, %s%s, %s)", slot, P, ex, lod);
    else snprintf(e, sizeof e, "texture(t%d, %s%s, %s)", slot, P, ex, lod);
    if (t->shadow) sprintf(out, "vec4(%s)", e);
    else strcpy(out, e);
}

// TEXS / TLDS in GLSL (same slot allocation order and result layout as the MSL switch in emit_texs)
static bool glsl_texs(Ctx *c, uint64_t w, bool fetch, int *slot_out, char *call, const char *A, const char *A1,
                      const char *Bv, const char *B1) {
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int slot;
    const char *co[3];
    char fa[64], fa1[64], fb[64], fb1[64];
    sprintf(fa, "as_type<float>(%s)", A); sprintf(fa1, "as_type<float>(%s)", A1);
    sprintf(fb, "as_type<float>(%s)", Bv); sprintf(fb1, "as_type<float>(%s)", B1);
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false); co[0] = fa;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb, NULL); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 0, NULL, NULL); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb1, 1, fb, NULL); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 1, "0.0f", NULL); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 0, NULL, NULL); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 1, "0.0f", NULL); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, A, fb1, 1, "0.0f", NULL); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb1, NULL); break;
        default: fail(c, "texs info %d", info); return false;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), 0)", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), int(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "texelFetch(t%d, ivec3(%s, %s, %s), 0)", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);  // (Ra: the layer; Rb, Rb + 1: x, y)
            sprintf(call, "texelFetch(t%d, ivec3(ivec2(%s), int(%s)), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", Bv, B1), A); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return false;
        }
    }
    *slot_out = slot;
    return true;
}

// TLD4S: a 2D gather of one component (bits 52-53) at (a, b), results to d0, d0 + 1 and d1, d1 + 1 (only d0's pair
// when d1 is RZ). The depth-compare and offset forms take other operands: not translated.
// .DC / .AOFFI (Ryujinx / yuzu TLD4S): the coordinates move to Ra, Ra + 1 and Rb holds the packed offset (6-bit x, y),
// then the depth reference (.DC without .AOFFI: Rb is the reference).
static void emit_tld4s(Ctx *c, uint64_t w) {
    if (!c->glsl) { fail(c, "tld4s (MSL)"); return; }
    bool dc = bits(w, 50, 1), aoffi = bits(w, 51, 1);
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    int slot = tex_slot(c, (uint32_t)bits(w, 36, 13), TEXTYPE_2D, dc, false);
    char x[16], y[16], off[16] = "", ref[16] = "";
    snprintf(x, sizeof x, "%s", R(a));
    if (dc || aoffi) {
        snprintf(y, sizeof y, "%s", a == RZ ? "0u" : R(a + 1));
        if (aoffi) snprintf(off, sizeof off, "%s", R(b));
        if (dc) snprintf(ref, sizeof ref, "%s", aoffi ? (b == RZ ? "0u" : R(b + 1)) : R(b));
    } else snprintf(y, sizeof y, "%s", R(b));
    char co[96], ofs[160];
    snprintf(co, sizeof co, "vec2(uintBitsToFloat(%s), uintBitsToFloat(%s))", x, y);
    snprintf(ofs, sizeof ofs, "ivec2(bitfieldExtract(int(%s), 0, 6), bitfieldExtract(int(%s), 8, 6))", off, off);
    if (dc && aoffi) sp(&c->body, "    { vec4 g = textureGatherOffset(t%d, %s, uintBitsToFloat(%s), %s);\n", slot, co, ref, ofs);
    else if (dc) sp(&c->body, "    { vec4 g = textureGather(t%d, %s, uintBitsToFloat(%s));\n", slot, co, ref);
    else if (aoffi) sp(&c->body, "    { vec4 g = textureGatherOffset(t%d, %s, %s, %d);\n", slot, co, ofs, (int)bits(w, 52, 2));
    else sp(&c->body, "    { vec4 g = textureGather(t%d, %s, %d);\n", slot, co, (int)bits(w, 52, 2));
    static const char *const comp[4] = {"g.x", "g.y", "g.z", "g.w"};
    for (int k = 0; k < 4; k++) {
        int base = k < 2 ? d0 : d1, r = base == RZ ? RZ : base + (k & 1);
        if (r == RZ || r >= RZ) continue;
        c->reg_used[r] = 1;
        sp(&c->body, "      r%d = floatBitsToUint(%s);\n", r, comp[k]);
    }
    sp(&c->body, "    }\n");
}

// TLD (full form, yuzu texture_fetch.cpp): texelFetch with a component mask. Ra: the coordinates (array: the layer in
// Ra's low 16 bits first); Rb, in order: the level (.LL; .LZ: 0), the packed offset (.AOFFI: 4-bit signed x, y, z),
// the sample (.MS). The masked components go to consecutive registers from d.
static void emit_tld(Ctx *c, uint64_t w) {
    if (!c->glsl) { fail(c, "tld (MSL)"); return; }
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8), dim = (int)bits(w, 28, 3), mask = (int)bits(w, 31, 4);
    bool aoffi = bits(w, 35, 1), ms = bits(w, 50, 1), ll = bits(w, 55, 1);
    if (d == RZ || !mask) return;
    char ra[4][16], rb[3][16];
    for (int k = 0; k < 4; k++) {
        if (a == RZ || a + k >= RZ) strcpy(ra[k], "0u");
        else { c->reg_used[a + k] = 1; sprintf(ra[k], "r%d", a + k); }
    }
    for (int k = 0; k < 3; k++) {
        if (b == RZ || b + k >= RZ) strcpy(rb[k], "0u");
        else { c->reg_used[b + k] = 1; sprintf(rb[k], "r%d", b + k); }
    }
    int nb = 0;
    const char *lod = ll ? rb[nb++] : "0u";
    const char *off = aoffi ? rb[nb++] : NULL;
    const char *smp = ms ? rb[nb++] : NULL;
    char call[600], o[200] = "";
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int slot;
    switch (dim) {
    case 0:  // 1D
        slot = tex_slot(c, index, TEXTYPE_1D, false, false);
        if (off) sprintf(o, " + bitfieldExtract(int(%s), 0, 4)", off);
        sprintf(call, "texelFetch(t%d, int(%s)%s, int(%s))", slot, ra[0], o, lod);
        break;
    case 2:  // 2D (multisampled with .MS)
        slot = tex_slot(c, index, ms ? TEXTYPE_2D_MS : TEXTYPE_2D, false, false);
        if (off) sprintf(o, " + ivec2(bitfieldExtract(int(%s), 0, 4), bitfieldExtract(int(%s), 4, 4))", off, off);
        if (ms) sprintf(call, "texelFetch(t%d, ivec2(%s)%s, int(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", ra[0], ra[1]), o, smp);
        else sprintf(call, "texelFetch(t%d, ivec2(%s)%s, int(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", ra[0], ra[1]), o, lod);
        break;
    case 3:  // 2D array
        slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
        if (off) sprintf(o, " + ivec2(bitfieldExtract(int(%s), 0, 4), bitfieldExtract(int(%s), 4, 4))", off, off);
        sprintf(call, "texelFetch(t%d, ivec3(ivec2(%s)%s, int(%s & 0xFFFFu)), int(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", ra[1], ra[2]),
                o, ra[0], lod);
        break;
    case 4:  // 3D
        slot = tex_slot(c, index, TEXTYPE_3D, false, false);
        if (off) sprintf(o, " + ivec3(bitfieldExtract(int(%s), 0, 4), bitfieldExtract(int(%s), 4, 4), bitfieldExtract(int(%s), 8, 4))", off, off, off);
        sprintf(call, "texelFetch(t%d, ivec3(int(%s), int(%s), int(%s))%s, int(%s))", slot, ra[0], ra[1], ra[2], o, lod);
        break;
    default: fail(c, "tld dim %d", dim); return;
    }
    sp(&c->body, "    { vec4 g = vec4(%s);\n", call);
    static const char *const cn[4] = {"g.x", "g.y", "g.z", "g.w"};
    for (int k = 0, r = d; k < 4; k++) {
        if (!(mask >> k & 1)) continue;
        if (r < RZ) { c->reg_used[r] = 1; sp(&c->body, "      r%d = floatBitsToUint(%s);\n", r, cn[k]); }
        r++;
    }
    sp(&c->body, "    }\n");
}

// TLD4 (full form): gather of one component - or a depth comparison (.DC) - from a 2D / 2D-array / cube (array)
// texture. Ra: the array index first, then the coordinates; Rb: the packed offset (.AOFFI: 6-bit x, y), then the
// reference. The masked components go to consecutive registers from d. (PTP's four offsets: not translated.)
static void emit_tld4(Ctx *c, uint64_t w) {
    if (!c->glsl) { fail(c, "tld4 (MSL)"); return; }
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8), dim = (int)bits(w, 28, 3);
    int mask = (int)bits(w, 31, 4), toff = (int)bits(w, 54, 2), comp = (int)bits(w, 56, 2);
    bool dc = bits(w, 50, 1), array = dim & 1;
    if (d == RZ || !mask) return;
    if (toff == 2 || toff == 3) { fail(c, "tld4 ptp"); return; }
    int type = dim == 2 ? TEXTYPE_2D : dim == 3 ? TEXTYPE_2D_ARRAY : dim == 6 ? TEXTYPE_CUBE : dim == 7 ? TEXTYPE_CUBE_ARRAY : -1;
    if (type < 0) { fail(c, "tld4 dim %d", dim); return; }
    int ncoord = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY ? 3 : 2;
    char ra[5][24], rb[2][24];
    int na = 0, nb = 0;
    for (int k = 0; k < 5; k++) {  // registers in order: Ra, Ra + 1, ... (RZ and past it: 0)
        if (a == RZ || a + k >= RZ) strcpy(ra[k], "0u");
        else { c->reg_used[a + k] = 1; sprintf(ra[k], "r%d", a + k); }
    }
    for (int k = 0; k < 2; k++) {
        if (b == RZ || b + k >= RZ) strcpy(rb[k], "0u");
        else { c->reg_used[b + k] = 1; sprintf(rb[k], "r%d", b + k); }
    }
    const char *layer = array ? ra[na++] : NULL;
    char co[160];
    int n = sprintf(co, "vec%d(", ncoord + (array ? 1 : 0));
    for (int k = 0; k < ncoord; k++) n += sprintf(co + n, "%suintBitsToFloat(%s)", k ? ", " : "", ra[na++]);
    if (layer) n += sprintf(co + n, ", float(int(%s))", layer);
    sprintf(co + n, ")");
    const char *off = toff == 1 ? rb[nb++] : NULL;
    const char *ref = dc ? rb[nb++] : NULL;
    int slot = tex_slot(c, (uint32_t)bits(w, 36, 13), type, dc, array);
    char call[400], ofs[128] = "";
    if (off && ncoord != 2) { fail(c, "tld4 offset on cube"); return; }
    if (off) sprintf(ofs, "ivec2(bitfieldExtract(int(%s), 0, 6), bitfieldExtract(int(%s), 8, 6))", off, off);
    if (dc && off) sprintf(call, "textureGatherOffset(t%d, %s, uintBitsToFloat(%s), %s)", slot, co, ref, ofs);
    else if (dc) sprintf(call, "textureGather(t%d, %s, uintBitsToFloat(%s))", slot, co, ref);
    else if (off) sprintf(call, "textureGatherOffset(t%d, %s, %s, %d)", slot, co, ofs, comp);
    else sprintf(call, "textureGather(t%d, %s, %d)", slot, co, comp);
    sp(&c->body, "    { vec4 g = %s;\n", call);
    static const char *const cn[4] = {"g.x", "g.y", "g.z", "g.w"};
    for (int k = 0, r = d; k < 4; k++) {
        if (!(mask >> k & 1)) continue;
        if (r < RZ) { c->reg_used[r] = 1; sp(&c->body, "      r%d = floatBitsToUint(%s);\n", r, cn[k]); }
        r++;
    }
    sp(&c->body, "    }\n");
}

static void emit_texs(Ctx *c, uint64_t w, bool fetch) {
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int msel = (int)bits(w, 50, 3);
    uint32_t mask;
    if (d1 == RZ) {
        static const uint32_t m2[] = {1, 2, 4, 8, 3, 9, 10, 12};
        mask = m2[msel];
    } else {
        static const uint32_t m4[] = {7, 11, 13, 14, 15, 15, 15, 15};
        mask = m4[msel];
    }
    char ra[16], ra1[16], rb[16], rb1[16];
    const char *A = reg(c, a, ra), *A1 = reg(c, a == RZ ? RZ : a + 1, ra1), *Bv = reg(c, b, rb),
               *B1 = reg(c, b == RZ ? RZ : b + 1, rb1);
    char call[1024];
    int slot;
    if (c->glsl) {
        char gcall[4096];
        if (!glsl_texs(c, w, fetch, &slot, gcall, A, A1, Bv, B1)) return;
        write_tex_result(c, slot, gcall, mask, d0, d1, true, !bits(w, 59, 1));
        return;
    }
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.sample(s%d, %s)", slot, slot, F(A)); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, float2(%s, %s))", slot, slot, F(A), F(Bv)); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, float2(%s, %s), level(0))", slot, slot, F(A), F(Bv)); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, float2(%s, %s), level(%s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s, level(%s)))", slot, slot, F(A), F(A1), F(B1), F(Bv)); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s, level(0)))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, float2(%s, %s), %s)", slot, slot, F(A1), F(Bv), ALAYER(A, slot)); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, float2(%s, %s), %s, level(0))", slot, slot, F(A1), F(Bv), ALAYER(A, slot)); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true);
            sprintf(call, "float4(t%d.sample_compare(s%d, float2(%s, %s), %s, %s, level(0)))", slot, slot, F(A1), F(Bv), A, F(B1)); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(0))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(%s))", slot, slot, F(A), F(A1), F(Bv), F(B1)); break;
        default: fail(c, "texs info %d", info); return;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s))", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s), uint(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.read(uint3(%s, %s, %s))", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", Bv, B1), A); break;  // (Ra layer; Rb, Rb + 1: x, y)
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return;
        }
    }
    write_tex_result(c, slot, call, mask, d0, d1, true, !bits(w, 59, 1));  // 0xd0/0xd2: .F16 results
}

// TEX (full form): coordinates from consecutive registers at gpr8, extra parameters at gpr20.
static void emit_tex(Ctx *c, uint64_t w, bool bindless) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = bindless ? 0 : (uint32_t)bits(w, 36, 13);
    int target = (int)bits(w, 29, 2);
    bool array = bits(w, 28, 1), dc = bits(w, 50, 1), aoffi = bindless ? bits(w, 36, 1) : bits(w, 54, 1);
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    int lod_mode = bindless ? (int)bits(w, 37, 3) : (int)bits(w, 55, 3);  // 0 none, 1 lz, 2 lb, 3 ll, 6 lba, 7 lla
    if (bindless) { fail(c, "bindless tex"); return; }
    int type = target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D)
             : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D)
             : target == 2 ? TEXTYPE_3D
                           : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
    int ncoord = target == 0 ? 1 : target == 1 ? 2 : 3;
    int slot = tex_slot(c, index, type, dc, array);
    if (c->glsl) {  // same register layout as below
        char cs[3][40], lay[96], lodv[48], offv[96], drv[48], gcall[4096];
        const char *co[3], *layp = NULL, *lodp = NULL, *offp = NULL, *drp = NULL;
        int ar2 = a, p2 = b, lk = 0;
        if (array) { sprintf(lay, "min(uint(r%d), uint(tsize%d(0).z) - 1u)", a, slot); c->reg_used[a] = 1; ar2 = a + 1; layp = lay; }
        for (int i = 0; i < ncoord; i++) { sprintf(cs[i], "as_type<float>(r%d)", ar2 + i); c->reg_used[ar2 + i] = 1; co[i] = cs[i]; }
        if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
            c->reg_used[p2] = 1;
            sprintf(lodv, "as_type<float>(r%d)", p2);
            lodp = lodv;
            lk = lod_mode == 2 || lod_mode == 6 ? 2 : 1;
            p2++;
        } else if (lod_mode == 1) {
            lk = 1;
            lodp = "0.0f";
        }
        if (aoffi) { c->reg_used[p2] = 1; sprintf(offv, "int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", p2, p2); offp = offv; p2++; }
        if (dc) { c->reg_used[p2] = 1; sprintf(drv, "as_type<float>(r%d)", p2); drp = drv; p2++; }
        glsl_sample(c, gcall, slot, co, layp, drp, lk, lodp, offp);
        write_tex_result(c, slot, gcall, mask, d, RZ, false, false);
        return;
    }
    char coords[256] = "", layer[64] = "", tmp[64];
    int ar = a;
    // the hardware clamps the array index to the layer count
    if (array) { sprintf(layer, "min(uint(r%d), t%d.get_array_size() - 1u)", a, slot); c->reg_used[a] = 1; ar = a + 1; }
    for (int i = 0; i < ncoord; i++) {
        sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", ar + i);
        c->reg_used[ar + i] = 1;
        strcat(coords, tmp);
    }
    int pr = b;
    char lod[96] = "", dref[64] = "", off[128] = "";
    if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
        c->reg_used[pr] = 1;
        sprintf(lod, lod_mode == 2 || lod_mode == 6 ? ", bias(as_type<float>(r%d))" : ", level(as_type<float>(r%d))", pr);
        pr++;
    } else if (lod_mode == 1) {
        strcpy(lod, ", level(0)");
    }
    if (aoffi) {
        c->reg_used[pr] = 1;
        sprintf(off, ", int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", pr, pr);
        pr++;
    }
    if (dc) { c->reg_used[pr] = 1; sprintf(dref, "as_type<float>(r%d)", pr); pr++; }
    const char *vec = ncoord == 1 ? "" : ncoord == 2 ? "float2" : "float3";
    char call[1024];
    if (dc)
        sprintf(call, "float4(t%d.sample_compare(s%d, %s(%s)%s%s, %s%s%s))", slot, slot, vec, coords, array ? ", " : "",
                layer, dref, lod, off);
    else
        sprintf(call, "t%d.sample(s%d, %s(%s)%s%s%s%s)", slot, slot, vec, coords, array ? ", " : "", layer, lod, off);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// ------------------------------------------------------------------ instruction translation
// ---- surfaces (SUST / SURED): storage images
// NVN puts image handles in driver cbuf 2 from 0x120 (after the 32 texture handles at 0x20), 8 bytes per slot. A bindless
// access (.B) takes the handle from a register: Z-A's load it just before with MOV r, c[2][0x120 + 8 * slot], which
// the scan back below finds (the first earlier instruction writing that register).
static int image_slot_bindless(Ctx *c, uint32_t pc, int reg) {
    const uint64_t *code = (const uint64_t *)c->code;
    for (int i = (int)(pc / 8) - 1, n = 0; i >= 0 && n < 256; i--, n++) {
        if (i % 4 == 0) continue;  // scheduling word
        uint64_t v = code[i];
        if ((int)bits(v, 0, 8) != reg) continue;
        if ((v & 0xfff8000000000000ull) == 0x4c98000000000000ull && bits(v, 34, 5) == 2) {  // MOV r, c[2][off]
            int off = (int)bits(v, 20, 14) * 4;
            if (off >= 0x120 && off < 0x160 && !(off & 7)) return (off - 0x120) / 8;
        }
        return -1;
    }
    return -1;
}

static int image_binding(Ctx *c, int slot, int dim, bool atomic) {
    for (int i = 0; i < c->info->nimg; i++)
        if (c->info->img[i].slot == slot) {
            if (c->info->img[i].dim != dim || c->info->img[i].atomic != atomic) { fail(c, "image %d used two ways", slot); return 0; }
            return i;
        }
    if (c->info->nimg == 8) { fail(c, "too many images"); return 0; }
    int i = c->info->nimg++;
    c->info->img[i] = (ShaderImage){(uint8_t)slot, (uint8_t)dim, atomic, c->key ? c->key->img_kind[slot] : 0};
    return i;
}

static const char *image_glsl_dim(int dim) {
    switch (dim) {
    case SUDIM_1D: return "1D";
    case SUDIM_1D_ARRAY: return "1DArray";
    case SUDIM_2D: return "2D";
    case SUDIM_2D_ARRAY: return "2DArray";
    default: return "3D";
    }
}

// coordinates from consecutive registers at a (x[, y[, z or layer]]), RZ and past it reading 0
static void image_coords(Ctx *c, char *o, int a, int dim) {
    int n = dim == SUDIM_1D ? 1 : dim == SUDIM_1D_ARRAY || dim == SUDIM_2D ? 2 : 3;
    char r[3][16];
    for (int k = 0; k < n; k++) {
        if (a == RZ || a + k >= RZ) strcpy(r[k], "0u");
        else { c->reg_used[a + k] = 1; sprintf(r[k], "r%d", a + k); }
    }
    if (n == 1) sprintf(o, "int(%s)", r[0]);
    else if (n == 2) sprintf(o, "ivec2(int(%s), int(%s))", r[0], r[1]);
    else sprintf(o, "ivec3(int(%s), int(%s), int(%s))", r[0], r[1], r[2]);
}

// SUST.P (formatted: the RGBA mask, data in consecutive registers from d) and SURED, bound (TidB) or bindless (.B).
// (SUST.D, typed by a size field, has opcodes of its own: not translated.)
static void emit_surface(Ctx *c, uint64_t w, uint32_t pc, bool red, bool bindless) {
    if (!c->glsl) { fail(c, "surface access (MSL)"); return; }
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), dim = (int)bits(w, 33, 3);
    int slot = bindless ? image_slot_bindless(c, pc, (int)bits(w, 39, 8)) : ((int)bits(w, 36, 13) * 4 - 0x120) / 8;
    if (slot < 0 || slot >= 8) { fail(c, "surface handle not from c[2] at %#x", pc); return; }
    if (dim == SUDIM_1D_BUFFER) { fail(c, "buffer surface"); return; }
    char coord[96];
    image_coords(c, coord, a, dim);
    if (!red) {
        int i = image_binding(c, slot, dim, false), mask = (int)bits(w, 20, 4), n = 0;
        int kind = c->info->img[i].kind;  // the data registers as the image's type: float / int / uint bits
        static const char *const zero[3] = {"0.0", "0", "0u"}, *const conv[3] = {"uintBitsToFloat(r%d)", "int(r%d)", "r%d"};
        static const char *const vec[3] = {"vec4", "ivec4", "uvec4"};
        char v[4][32];
        for (int k = 0; k < 4; k++) {
            if (!(mask >> k & 1)) { strcpy(v[k], zero[kind]); continue; }
            int r = d == RZ || d + n >= RZ ? RZ : d + n;
            n++;
            if (r == RZ) strcpy(v[k], zero[kind]);
            else { c->reg_used[r] = 1; sprintf(v[k], conv[kind], r); }
        }
        sp(&c->body, "    imageStore(img%d, %s, %s(%s, %s, %s, %s));\n", i, coord, vec[kind], v[0], v[1], v[2], v[3]);
    } else {
        int size = (int)bits(w, 20, 3), op = (int)bits(w, 24, 3);
        static const char *const fn[8] = {"imageAtomicAdd", "imageAtomicMin", "imageAtomicMax", NULL, NULL,
                                          "imageAtomicAnd", "imageAtomicOr", "imageAtomicXor"};
        if (size > 1 || !fn[op] || (size == 1 && (op == 1 || op == 2))) { fail(c, "SURED size %d op %d", size, op); return; }
        int i = image_binding(c, slot, dim, true);
        sp(&c->body, "    %s(img%d, %s, %s);\n", fn[op], i, coord, R(d));
    }
    // (not gmem_stores: an image store writes the image, never guest memory - it had made exec.c read every vertex
    // stream in place from the game's memory, over PCIe, for the whole session: 4x the GPU time in Legends: Arceus)
}

// ATOM (result to d) / RED (no result): a 32-bit atomic on global memory at [Ra (+ Ra + 1 with .E) + imm20]. Sizes
// U32 / S32 / F32 (add); ops add, min, max, inc, dec, and, or, xor, exch. GLSL atomics where they match, else a
// compare-and-swap loop (signed min / max, the wrapping inc / dec, float add). 64-bit: add only (two words + carry).
static void emit_gatomic(Ctx *c, uint64_t w, bool red) {
    if (!c->glsl) { fail(c, "global atomic (MSL)"); return; }
    int a = (int)bits(w, 8, 8), d = red ? RZ : (int)bits(w, 0, 8), b = red ? (int)bits(w, 0, 8) : (int)bits(w, 20, 8);
    int size = red ? (int)bits(w, 20, 3) : (int)bits(w, 49, 3), op = red ? (int)bits(w, 23, 3) : (int)bits(w, 52, 4);
    bool wide = bits(w, 48, 1);
    int64_t off = sbits(w, 28, 20);
    bool s64 = size == 2 || size == 5;
    if ((s64 && op != 0) || size == 4 || size > 5 || op > 8 || (size == 3 && op != 0)) { fail(c, "global atomic size %d op %d", size, op); return; }
    c->info->uses_gmem = true;
    c->uses_gmem_store = true;
    c->info->gmem_stores = true;
    char addr[256];
    if (a == RZ) sprintf(addr, "ulong(%lld)", (long long)off);
    else if (wide) sprintf(addr, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(a + 1), R(a), (long long)off);
    else sprintf(addr, "(ulong(%s) + ulong(%lld))", R(a), (long long)off);
    const char *v = R(b);
    char lim[32];  // (GLSL: the size imported, from the heap's uniform - past it is no memory at all with an address)
    if (c->glsl) snprintf(lim, sizeof lim, "gmb_.gsize");
    else snprintf(lim, sizeof lim, "0x%llxul", (unsigned long long)maxwell_gmem_size);
    sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga < %s; uint wi = uint(ga >> 2); uint av = %s, old = 0u;\n", addr,
       (unsigned long long)maxwell_gmem_base, lim, v);
    if (s64) {  // 64-bit add: the low word, then the high word plus the carry - the sum is exact (adds commute); the old
                // value a result would return is the two halves read apart
        sp(&c->body, "      uint avh = %s, oldh = 0u;\n", b == RZ || b + 1 >= RZ ? "0u" : R(b + 1));
        sp(&c->body, "      if (ok) { old = atomicAdd(gmem_.w[wi], av); oldh = atomicAdd(gmem_.w[wi + 1u], avh + (old + av < old ? 1u : 0u)); }\n");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = old;\n", d); }
        if (d != RZ && d + 1 < RZ) { c->reg_used[d + 1] = 1; sp(&c->body, "      r%d = oldh;\n", d + 1); }
        sp(&c->body, "    }\n");
        return;
    }
    static const char *const direct[9] = {"atomicAdd", "atomicMin", "atomicMax", NULL, NULL, "atomicAnd", "atomicOr", "atomicXor",
                                          "atomicExchange"};
    const char *nv = NULL;  // the new value from `prev` (CAS loop)
    if (size == 3) nv = "floatBitsToUint(uintBitsToFloat(prev) + uintBitsToFloat(av))";
    else if (size == 1 && op == 1) nv = "uint(min(int(prev), int(av)))";
    else if (size == 1 && op == 2) nv = "uint(max(int(prev), int(av)))";
    else if (op == 3) nv = "(prev >= av ? 0u : prev + 1u)";
    else if (op == 4) nv = "((prev == 0u || prev > av) ? av : prev - 1u)";
    if (nv)
        sp(&c->body, "      if (ok) { uint cur = gmem_.w[wi], prev; do { prev = cur; cur = atomicCompSwap(gmem_.w[wi], prev, %s); } "
                     "while (cur != prev); old = prev; }\n", nv);
    else
        sp(&c->body, "      if (ok) old = %s(gmem_.w[wi], av);\n", direct[op]);
    if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = old;\n", d); }
    sp(&c->body, "    }\n");
}

static void emit_insn(Ctx *c, uint64_t w, uint32_t pc) {
    char t[2048], u[2048];
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    int pr = (int)bits(w, 16, 3);
    bool prn = bits(w, 19, 1);
    uint16_t op13 = (uint16_t)(w >> 51);  // top 13 bits
    // predicate guard. SSY/PBK/PCNT/PRET take no predicate: those bits are zero in the encoding,
    // which would otherwise read as @P0.
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull ||
        top12 == 0xe270000000000000ull) {
        pr = PT;
        prn = false;
    }
    bool guarded = !(pr == PT && !prn);
    if (pr == PT && prn) return;  // @!pt: never executes
    if (guarded) sp(&c->body, "  if (%s) {\n", P(pr, prn));

#define OPM(val, mask) ((w & (uint64_t)(mask)) == (uint64_t)(val))
    if (OPM(0xe300000000000000ull, 0xfff0000000000000ull)) {  // EXIT
        sp(&c->body, c->branchy ? "    exit_now = true; break;\n" : "    break;\n");
    } else if (OPM(0xe330000000000000ull, 0xfff0000000000000ull)) {  // KIL
        c->info->uses_discard = true;
        if (getenv("LARECOMP_NO_DISCARD")) {
        } else if (!c->glsl) sp(&c->body, "    discard_fragment();\n");
        else if (c->stage == STAGE_FRAGMENT) sp(&c->body, "    discard;\n");
        else sp(&c->body, "    /* kil */\n");  // GLSL has discard in fragment shaders only
    } else if (OPM(0xe240000000000000ull, 0xfff0000000000020ull)) {  // BRA
        uint32_t target = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
        c->uses_branches = true;
        // bits 0-4: the condition code test (15 T: always) on the flags a .CC instruction left (zero ccz, sign ccs;
        // overflow isn't tracked: 0). The U forms (unordered too) test the same flags.
        static const char *const cc_test[16] = {"false", "ccs", "ccz", "(ccz || ccs)", "(!ccz && !ccs)", "!ccz", "!ccs", NULL,
                                                NULL, "ccs", "ccz", "(ccz || ccs)", "(!ccz && !ccs)", "!ccz", "!ccs", NULL};
        uint32_t cond = (uint32_t)bits(w, 0, 5);
        if (cond != 15 && cond < 16 && cc_test[cond] && c->ccz_modelled) {
            c->uses_cc = true;
            jump(c, cc_test[cond], target, pc);
        } else jump(c, NULL, target, pc);  // (T, or flags no modelled instruction set: as before)
    } else if (OPM(0xe290000000000000ull, 0xfff0000000000020ull)) {  // SSY
        c->uses_branches = true;
        if (!c->flow_to) sp(&c->body, "    crs[crs_n & 31u] = %uu; crk[crs_n & 31u] = 0u; crs_n++;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe2a0000000000000ull, 0xfff0000000000020ull)) {  // PBK
        c->uses_branches = true;
        if (!c->flow_to) sp(&c->body, "    crs[crs_n & 31u] = %uu; crk[crs_n & 31u] = 1u; crs_n++;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe2b0000000000000ull, 0xfff0000000000020ull)) {  // PCNT
        c->uses_branches = true;
        if (!c->flow_to) sp(&c->body, "    crs[crs_n & 31u] = %uu; crk[crs_n & 31u] = 2u; crs_n++;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xf0f8000000000000ull, 0xfff8000000000000ull)) {  // SYNC
        if (c->flow_to) jump(c, NULL, c->flow_to[pc / 8], pc);
        else sp(&c->body, "    crs_n--; pc = crs[crs_n & 31u]; continue;\n");
    } else if (OPM(0xe340000000000000ull, 0xfff0000000000000ull)) {  // BRK
        if (c->flow_to) jump(c, NULL, c->flow_to[pc / 8], pc);
        else sp(&c->body, "    while (crs_n > 1u && crk[(crs_n - 1u) & 31u] != 1u) crs_n--; crs_n--; pc = crs[crs_n & 31u]; continue;\n");
    } else if (OPM(0xe350000000000000ull, 0xfff0000000000000ull)) {  // CONT
        if (c->flow_to) jump(c, NULL, c->flow_to[pc / 8], pc);
        else sp(&c->body, "    while (crs_n > 1u && crk[(crs_n - 1u) & 31u] != 2u) crs_n--; pc = crs[(crs_n - 1u) & 31u]; continue;\n");
    } else if (OPM(0xf0f0000000000000ull, 0xfff8000000000000ull) || OPM(0x50b0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0xef98000000000000ull, 0xfff8000000000000ull)) {
        // DEPBAR / NOP / MEMBAR
    } else if (OPM(0xe00000000000ff00ull, 0xff0000400000ff00ull) || OPM(0xe000004000000000ull, 0xff00004000000000ull)) {
        // IPA: interpolate fragment input
        bool idx = bits(w, 38, 1);
        uint32_t addr = (uint32_t)bits(w, 28, 10);
        int mode = (int)bits(w, 54, 2);  // 0 pass, 1 multiply, 2 constant, 3 sc
        if (idx) { fail(c, "ipa idx"); }
        if (addr >= 0x80 && addr < 0x280) {
            int at = (addr - 0x80) >> 4;
            if (mode == 2) c->flat_mask |= 1u << at;
        }
        c->ipa_mode = mode;
        c->ipa_mul = (int)bits(w, 20, 8);
        attr_load(c, d, addr, NULL);
        c->ipa_mode = -1;
        if (bits(w, 51, 1) && d != RZ) sp(&c->body, "    r%d = as_type<uint>(saturate(as_type<float>(r%d)));\n", d, d);
    } else if (OPM(0xefd8000000000000ull, 0xfff8000000000000ull)) {  // LD attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        int ia = (int)bits(w, 8, 8);
        if (ia != RZ) { fail(c, "indexed ld a[]"); }
        for (int i = 0; i < n; i++) attr_load(c, d + i, addr + 4 * i, NULL);
    } else if (OPM(0xeff0000000000000ull, 0xfff8000000000000ull)) {  // ST attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        for (int i = 0; i < n; i++) attr_store(c, d + i, addr + 4 * i);
    } else if (OPM(0xef90000000000000ull, 0xfff8000000000000ull)) {  // LD c[idx][reg + imm]
        int idx = (int)bits(w, 36, 5);
        int64_t off = (int64_t)bits(w, 20, 16);  // unsigned (Ryujinx InstLdc.CbufOffset)
        int sz = (int)bits(w, 48, 3);
        int n = sz == 5 ? 2 : 1;
        char rb[16];
        const char *dyn = a == RZ ? NULL : reg(c, a, rb);
        // LDC.64 R4, c[i][R4 + x]: the address register is one of the destinations. Read it once before the
        // writes - with r4 written first, the second word came from c[i][<loaded value> + x + 4] (skinned models: the
        // second bone's w translation read as 0, vertices thrown at the camera - letsgo-recomp's docs/HISTORICO.md).
        bool overlap = dyn && n > 1 && a >= d && a < d + n;
        if (overlap) { sp(&c->body, "    { uint ldc_addr = %s;\n", dyn); dyn = "ldc_addr"; }
        for (int i = 0; i < n; i++) {
            char cb[256];
            cbuf_ref(c, idx, off + 4 * i, dyn, cb);
            if (dyn) c->info->cbuf_size[idx] = 0x10000;
            set_reg(c, d + i, cb);
        }
        if (overlap) sp(&c->body, "    }\n");
    } else if (OPM(0xc800000000000000ull, 0xfc00000000000000ull)) {  // TLD4
        emit_tld4(c, w);
    } else if (OPM(0xdf00000000000000ull, 0xffc0000000000000ull)) {  // TLD4S
        emit_tld4s(c, w);
    } else if (OPM(0xdc38000000000000ull, 0xfff8000000000000ull)) {  // TLD
        emit_tld(c, w);
    } else if (OPM(0xeb20000000000000ull, 0xfff8000000000000ull)) {  // SUST.B (bindless)
        emit_surface(c, w, pc, false, true);
    } else if (OPM(0xeb28000000000000ull, 0xfff8000000000000ull)) {  // SUST
        emit_surface(c, w, pc, false, false);
    } else if (OPM(0xeb50000000000000ull, 0xfff8000000000000ull)) {  // SURED.B (bindless)
        emit_surface(c, w, pc, true, true);
    } else if (OPM(0xeb58000000000000ull, 0xfff8000000000000ull)) {  // SURED
        emit_surface(c, w, pc, true, false);
    } else if (OPM(0xd000000000000000ull, 0xf600000000000000ull)) {  // TEXS
        emit_texs(c, w, false);
    } else if (OPM(0xd200000000000000ull, 0xf600000000000000ull)) {  // TLDS
        emit_texs(c, w, true);
    } else if (OPM(0xc038000000000000ull, 0xfc38000000000000ull)) {  // TEX
        emit_tex(c, w, false);
    } else if (OPM(0xdeb8000000000000ull, 0xfff8000000000000ull)) {  // TEX.B (bindless)
        emit_tex(c, w, true);
    } else if (OPM(0xdf48000000000000ull, 0xfff8000000000000ull)) {  // TXQ
        uint32_t index = (uint32_t)bits(w, 36, 13);
        int qt = (int)bits(w, 22, 6);
        uint32_t mask = (uint32_t)bits(w, 31, 4);
        int slot = tex_slot(c, index, -1, false, false);  // the type comes from a sample, if any
        if (qt == 1 && c->glsl) {  // tsize<slot>() is emitted with the slot's final type
            if (c->stage != STAGE_VERTEX) {
                c->uses_rscale = true;
                sp(&c->body, "    { uint lv = %s; uvec4 q = uvec4(uint(floor(float(tsize%d(int(lv)).x) / rscale[%d].x + 0.5f)), "
                   "uint(floor(float(tsize%d(int(lv)).y) / rscale[%d].y + 0.5f)), 1u, uint(textureQueryLevels(t%d)));\n",
                   R(a), slot, slot + 1, slot, slot + 1, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uvec4 q = uvec4(uint(tsize%d(int(lv)).x), uint(tsize%d(int(lv)).y), 1u, uint(textureQueryLevels(t%d)));\n",
                   R(a), slot, slot, slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else if (qt == 1) {  // dimension
            if (c->stage != STAGE_VERTEX) {  // report the guest size of scaled allocations
                c->uses_rscale = true;
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(uint(round(float(t%d.get_width(lv)) / rscale[%d].x)), "
                   "uint(round(float(t%d.get_height(lv)) / rscale[%d].y)), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot + 1, slot, slot + 1, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(t%d.get_width(lv), t%d.get_height(lv), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot, slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else {
            fail(c, "txq type %d", qt);
        }
    } else if (OPM(0xdf58000000000000ull, 0xfff8000000000000ull)) {  // TMML: level of detail a sample would use
        // Same layout as TEX (index, array, target, mask); coordinates from Ra (layer first). Results are 8.8 fixed
        // point in the inverse order of textureQueryLod: x = computed lod, y = the level accessed (Ryujinx InstEmitTexture).
        uint32_t index = (uint32_t)bits(w, 36, 13);
        int target = (int)bits(w, 29, 2);
        bool array = bits(w, 28, 1);
        uint32_t mask = (uint32_t)bits(w, 31, 4);
        int type = target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D)
                 : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D)
                 : target == 2 ? TEXTYPE_3D
                               : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
        int ncoord = target == 0 ? 1 : target == 1 ? 2 : 3;
        int slot = tex_slot(c, index, type, false, array);
        int ar = a + (array ? 1 : 0);
        if (array) c->reg_used[a] = 1;
        char co[160] = "", tmp[48];
        for (int i = 0; i < ncoord; i++) {
            sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", ar + i);
            c->reg_used[ar + i] = 1;
            strcat(co, tmp);
        }
        const char *gv = ncoord == 1 ? "" : ncoord == 2 ? "vec2" : "vec3", *mv = ncoord == 1 ? "" : ncoord == 2 ? "float2" : "float3";
        if (c->stage != STAGE_FRAGMENT)  // (no derivatives outside fragment shaders: the base level)
            sp(&c->body, "    { float2 lq = float2(0.0f);\n");
        else if (c->glsl)
            sp(&c->body, "    { vec2 lq = textureQueryLod(t%d, %s(%s)).yx;\n", slot, gv, co);
        else
            sp(&c->body, "    { float2 lq = float2(t%d.calculate_unclamped_lod(s%d, %s(%s)), t%d.calculate_clamped_lod(s%d, %s(%s)));\n",
               slot, slot, mv, co, slot, slot, mv, co);
        int n = 0;
        for (int i = 0; i < 2; i++)
            if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = uint(int(lq[%d] * 256.0f));\n", d + n, i); n++; }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5080000000000000ull, 0xfff8000000000000ull)) {  // MUFU
        int fn = (int)bits(w, 20, 4);
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1));
        switch (fn) {
        case 0: sprintf(t, "cos(%s)", x); break;
        case 1: sprintf(t, "sin(%s)", x); break;
        case 2: sprintf(t, "exp2(%s)", x); break;
        case 3: sprintf(t, "log2(%s)", x); break;
        case 4: sprintf(t, "(1.0f / %s)", x); break;
        case 5: sprintf(t, "rsqrt(%s)", x); break;
        case 8: sprintf(t, "sqrt(%s)", x); break;
        default: fail(c, "mufu %d", fn); return;
        }
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x5c90000000000000ull, 0xfff8000000000000ull) || OPM(0x4c90000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3890000000000000ull, 0xfef8000000000000ull)) {  // RRO: range reduction (no-op for us)
        const char *x = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        set_reg(c, d, FRES(x, false));
    } else if (OPM(0x5c58000000000000ull, 0xfff8000000000000ull) || OPM(0x4c58000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3858000000000000ull, 0xfef8000000000000ull)) {  // FADD
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1)));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0800000000000000ull, 0xfc00000000000000ull)) {  // FADD32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 56, 1), bits(w, 54, 1)), NA(u, bits(w, 53, 1), bits(w, 57, 1)));
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5c68000000000000ull, 0xfff8000000000000ull) || OPM(0x4c68000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3868000000000000ull, 0xfef8000000000000ull)) {  // FMUL
        static const char *const scale[] = {"", " * 0.5f", " * 0.25f", " * 0.125f", " * 8.0f", " * 4.0f", " * 2.0f", ""};
        sprintf(t, "(%s * %s%s)", F(R(a)), NA(F(B(true)), bits(w, 48, 1), false), scale[bits(w, 41, 3)]);
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x1e00000000000000ull, 0xff00000000000000ull)) {  // FMUL32I
        sprintf(t, "(%s * as_type<float>(0x%08xu))", F(R(a)), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5980000000000000ull, 0xff80000000000000ull) || OPM(0x4980000000000000ull, 0xff80000000000000ull) ||
               OPM(0x5180000000000000ull, 0xff80000000000000ull) || OPM(0x3280000000000000ull, 0xfe80000000000000ull)) {  // FFMA
        const char *bv;
        char cbuf_c[256], bbuf[256];
        const char *cv = operand_c_for_ffma(c, w, cbuf_c, &bv, bbuf);
        sprintf(t, "fma(%s, %s, %s)", F(R(a)), NA(F(bv), bits(w, 48, 1), false), NA(F(cv), bits(w, 49, 1), false));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0c00000000000000ull, 0xfc00000000000000ull)) {  // FFMA32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "fma(%s, %s, %s)", NA(F(R(a)), bits(w, 56, 1), false), u, NA(F(R(d)), bits(w, 57, 1), false));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5c60000000000000ull, 0xfff8000000000000ull) || OPM(0x4c60000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3860000000000000ull, 0xfef8000000000000ull)) {  // FMNMX
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), *y = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        sprintf(t, "(%s ? fmin(%s, %s) : fmax(%s, %s))", P((int)bits(w, 39, 3), bits(w, 42, 1)), x, y, x, y);
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5bb0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bb0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36b0000000000000ull, 0xfef0000000000000ull)) {  // FSETP
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 7, 1)), *y = NA(F(B(true)), bits(w, 6, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n      p%d = cmp %s pp;\n", cmp, pc_, p, bool_op(bo)); }
        else sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pc_);
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5800000000000000ull, 0xff00000000000000ull) || OPM(0x4800000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3000000000000000ull, 0xfe00000000000000ull)) {  // FSET
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 54, 1)), *y = NA(F(B(true)), bits(w, 53, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 52, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
        if (bits(w, 47, 1)) {  // .CC: zero/sign flags of the result, often with RZ as the destination - a test for a
                               // following BRA's condition (Z-A's contact shadows: FSET.GT.CC RZ then BRA NEU)
            c->uses_cc = true;
            sp(&c->body, "    { uint ccv = %s; ccz = ccv == 0u; ccs = int(ccv) < 0; }\n", u);
            c->ccz_modelled = true;
        }
    } else if (OPM(0x5ba0000000000000ull, 0xfff0000000000000ull) || OPM(0x4ba0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36a0000000000000ull, 0xfef0000000000000ull) || OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {  // FCMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, true, bb);
            cv = R((int)bits(w, 39, 8));
        }
        fcmp_expr(cmp, (int)bits(w, 48, 4), F(cv), "0.0f");
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c98000000000000ull, 0xfff8000000000000ull) || OPM(0x4c98000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3898000000000000ull, 0xfef8000000000000ull)) {  // MOV
        set_reg(c, d, B(false));
    } else if (OPM(0x0100000000000000ull, 0xfff0000000000000ull)) {  // MOV32I
        sprintf(t, "0x%08xu", (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ca0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a0000000000000ull, 0xfef8000000000000ull)) {  // SEL
        sprintf(t, "(%s ? %s : %s)", P((int)bits(w, 39, 3), bits(w, 42, 1)), R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5cb0000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b0000000000000ull, 0xfef8000000000000ull)) {  // F2I (src fmt bits 10-11: 1 F16; dst size bits 8-9)
        char srcv[512];
        if (bits(w, 10, 2) == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (bits(w, 10, 2) == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int rnd = (int)bits(w, 39, 2);
        bool sgn = bits(w, 12, 1), i16 = bits(w, 8, 2) == 1;
        static const char *const rf[] = {"rint", "floor", "ceil", "trunc"};
        if (i16)
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -32768.0f, 32767.0f)))" : "uint(clamp(%s(%s), 0.0f, 65535.0f))", rf[rnd], x);
        else
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -2147483648.0f, 2147483520.0f)))" : "uint(clamp(%s(%s), 0.0f, 4294967040.0f))", rf[rnd], x);
        set_reg(c, d, t);
    } else if (OPM(0x5cb8000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b8000000000000ull, 0xfef8000000000000ull)) {  // I2F
        bool sgn = bits(w, 13, 1);
        int sz = (int)bits(w, 10, 2), sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        if (sz == 0) sprintf(src, sgn ? "int(char((%s >> %d) & 0xFF))" : "((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (sz == 1) sprintf(src, sgn ? "int(short((%s >> %d) & 0xFFFF))" : "((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, sgn ? "int(%s)" : "%s", bv);
        sprintf(u, "float(%s)", src);
        if (bits(w, 8, 2) == 1) {  // F16 result in the low half
            char h[700];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))",
                     NA(u, bits(w, 45, 1), bits(w, 49, 1)));
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(NA(u, bits(w, 45, 1), bits(w, 49, 1)), false));
        }
    } else if (OPM(0x5ca8000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a8000000000000ull, 0xfef8000000000000ull)) {  // F2F (formats: 1 F16, 2 F32; bit 41 = high half)
        int dfmt = (int)bits(w, 8, 2), sfmt = (int)bits(w, 10, 2);
        char srcv[512];
        if (sfmt == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (sfmt == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int mode = (int)((bits(w, 39, 2)) | (bits(w, 42, 1) << 2));
        switch (mode) {
        case 4: sprintf(t, "rint(%s)", x); break;
        case 5: sprintf(t, "floor(%s)", x); break;
        case 6: sprintf(t, "ceil(%s)", x); break;
        case 7: sprintf(t, "trunc(%s)", x); break;
        default: sprintf(t, "%s", x);
        }
        if (dfmt == 1) {  // F16 result in the low half, high half zero
            char h[1100];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))",
                     bits(w, 50, 1) ? (sprintf(u, "saturate(%s)", t), u) : t);
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(t, bits(w, 50, 1)));
        }
    } else if (OPM(0x5c10000000000000ull, 0xfff8000000000000ull) || OPM(0x4c10000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3810000000000000ull, 0xfef8000000000000ull)) {  // IADD
        if (bits(w, 43, 1) || bits(w, 47, 1)) c->uses_cc = true;
        sprintf(t, "(%s%s + %s%s%s)", bits(w, 49, 1) ? "-" : "", R(a), bits(w, 48, 1) ? "-" : "", B(false),
                bits(w, 43, 1) ? " + cc" : "");
        if (bits(w, 47, 1)) {
            sp(&c->body, "    { uint s = %s%s; uint t2 = %s%s; uint res = s + t2%s; cc = (res < s) ? 1u : 0u;\n", bits(w, 49, 1) ? "-" : "",
               R(a), bits(w, 48, 1) ? "-" : "", B(false), bits(w, 43, 1) ? " + cc" : "");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1c00000000000000ull, 0xfe00000000000000ull)) {  // IADD32I (CC bit 52, X bit 53)
        bool wcc = bits(w, 52, 1), x = bits(w, 53, 1);
        if (wcc || x) c->uses_cc = true;
        sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(0x%08xu)%s;%s\n", bits(w, 56, 1) ? "-" : "", R(a), (uint32_t)bits(w, 20, 32),
           x ? " + ulong(cc)" : "", wcc ? " cc = uint(sum >> 32) & 1u;" : "");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5cc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4cc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x38c0000000000000ull, 0xfef0000000000000ull)) {  // IADD3
        bool wcc3 = bits(w, 47, 1), x3 = bits(w, 48, 1);
        if (wcc3 || x3) {  // carry in/out (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(%s%s) + ulong(%s%s)%s;\n", bits(w, 51, 1) ? "-" : "", R(a),
               bits(w, 50, 1) ? "-" : "", B(false), bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)), x3 ? " + ulong(cc)" : "");
            if (wcc3) sp(&c->body, "      cc = uint(sum >> 32) & 1u;\n");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
            sp(&c->body, "    }\n");
        } else {
            sprintf(t, "(%s%s + %s%s + %s%s)", bits(w, 51, 1) ? "-" : "", R(a), bits(w, 50, 1) ? "-" : "", B(false),
                    bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)));
            set_reg(c, d, t);
        }
    } else if (OPM(0x5c18000000000000ull, 0xfff8000000000000ull) || OPM(0x4c18000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3818000000000000ull, 0xfef8000000000000ull)) {  // ISCADD
        sprintf(t, "((%s%s << %du) + %s%s)", bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "",
                B(false));
        if (bits(w, 47, 1)) {  // .CC: carry out for a following IADD.X (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { uint s = %s%s << %du; uint t2 = %s%s; uint res = s + t2; cc = (res < s) ? 1u : 0u;\n",
               bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "", B(false));
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1400000000000000ull, 0xfc00000000000000ull)) {  // ISCADD32I
        sprintf(t, "((%s << %du) + 0x%08xu)", R(a), (int)bits(w, 53, 5), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5b60000000000000ull, 0xfff0000000000000ull) || OPM(0x4b60000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3660000000000000ull, 0xfef0000000000000ull)) {  // ISETP
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pp);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5b50000000000000ull, 0xfff0000000000000ull) || OPM(0x4b50000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3650000000000000ull, 0xfef0000000000000ull)) {  // ISET
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x5c20000000000000ull, 0xfff8000000000000ull) || OPM(0x4c20000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3820000000000000ull, 0xfef8000000000000ull)) {  // IMNMX
        bool sgn = bits(w, 48, 1);
        const char *x = R(a), *y = B(false), *p = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (sgn) sprintf(t, "uint(%s ? min(int(%s), int(%s)) : max(int(%s), int(%s)))", p, x, y, x, y);
        else sprintf(t, "(%s ? min(%s, %s) : max(%s, %s))", p, x, y, x, y);
        set_reg(c, d, t);
    } else if (OPM(0x5bd0000000000000ull, 0xfff8000000000000ull) || OPM(0x4bd0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36d0000000000000ull, 0xfef8000000000000ull)) {  // LEA: d = (+-a << shift) + b (envytools gm107)
        uint32_t sh = (uint32_t)bits(w, 39, 5);
        const char *bv = B(false);
        if (bits(w, 45, 1)) sprintf(t, "((0u - %s) << %uu) + %s", R(a), sh, bv);
        else sprintf(t, "(%s << %uu) + %s", R(a), sh, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5bd8000000000000ull, 0xfff8000000000000ull) || OPM(0x1800000000000000ull, 0xfc00000000000000ull)) {
        // LEA.HI: d = low word of ((c:a) >> (32 - shift)) + b, c the high word of a 64-bit value
        bool reg_form = (w >> 56) == 0x5b;
        uint32_t sh = (uint32_t)(reg_form ? bits(w, 28, 5) : bits(w, 51, 5));
        const char *bv = reg_form ? R((int)bits(w, 20, 8)) : cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, alloca(128));
        const char *hi = R((int)bits(w, 39, 8)), *lo = R(a);
        if (bits(w, reg_form ? 37 : 56, 1)) fail(c, "LEA.HI with .neg at %#x", pc);
        if (sh == 0) sprintf(t, "%s + %s", hi, bv);
        else sprintf(t, "((%s >> %uu) | (%s << %uu)) + %s", lo, 32 - sh, hi, sh, bv);
        set_reg(c, d, t);
    } else if (OPM(0x3a00000000000000ull, 0xfe00000000000000ull)) {  // VMNMX (video min/max; Ryujinx InstEmitVideoMinMax)
        int sa = (int)(bits(w, 36, 3) | bits(w, 48, 1) << 3), sb = (int)(bits(w, 28, 3) | bits(w, 49, 1) << 3);
        int vop = (int)bits(w, 51, 3);
        bool dfmt = bits(w, 54, 1), mx = bits(w, 56, 1);
        char ea[96], eb[96], r[400];
        video_sel(ea, R(a), sa);
        if (bits(w, 50, 1)) video_sel(eb, R((int)bits(w, 20, 8)), sb);
        else {
            uint32_t imm = (uint32_t)bits(w, 20, 16);
            if (sb & 8) imm = (uint32_t)(int32_t)(int16_t)imm;
            sprintf(eb, "%uu", imm);
        }
        if ((sa & 8) != (sb & 8)) {  // mixed signedness: max fits a u32, min an s32; a "negative" unsigned operand wins
            if (mx) sprintf(r, "max(%s, %s)", ea, eb);
            else sprintf(r, "uint(min(int(%s), int(%s)))", ea, eb);
            const char *u = (sa & 8) ? eb : ea;
            char tmp[400];
            sprintf(tmp, "(int(%s) < 0 ? %s : %s)", u, u, r);
            strcpy(r, tmp);
        } else if (sa & 8) sprintf(r, "uint(%s(int(%s), int(%s)))", mx ? "max" : "min", ea, eb);
        else sprintf(r, "%s(%s, %s)", mx ? "max" : "min", ea, eb);
        const char *cc = R((int)bits(w, 39, 8));
        switch (vop) {
        case 0: sprintf(t, "((%s & 0xffffu) | (%s << 16u))", cc, r); break;          // MRG_16H
        case 1: sprintf(t, "((%s & 0xffff0000u) | (%s & 0xffffu))", cc, r); break;   // MRG_16L
        case 2: sprintf(t, "((%s & 0xffffff00u) | (%s & 0xffu))", cc, r); break;     // MRG_8B0
        case 3: sprintf(t, "((%s & 0xff00ffffu) | ((%s & 0xffu) << 16u))", cc, r); break;  // MRG_8B2
        case 4: sprintf(t, "(%s + %s)", r, cc); break;                                // ACC
        case 5: if (dfmt) sprintf(t, "uint(min(int(%s), int(%s)))", r, cc); else sprintf(t, "min(%s, %s)", r, cc); break;
        case 6: if (dfmt) sprintf(t, "uint(max(int(%s), int(%s)))", r, cc); else sprintf(t, "max(%s, %s)", r, cc); break;
        default: sprintf(t, "%s", r);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5c48000000000000ull, 0xfff8000000000000ull) || OPM(0x4c48000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3848000000000000ull, 0xfef8000000000000ull)) {  // SHL
        const char *bv = B(false);
        if (bits(w, 39, 1)) sprintf(t, "(%s << (%s & 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s << (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c28000000000000ull, 0xfff8000000000000ull) || OPM(0x4c28000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3828000000000000ull, 0xfef8000000000000ull)) {  // SHR
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (sgn) sprintf(t, "uint(int(%s) >> min(%s, 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s >> (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c40000000000000ull, 0xfff8000000000000ull) || OPM(0x4c40000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3840000000000000ull, 0xfef8000000000000ull)) {  // LOP
        char x[64], y[256];
        sprintf(x, bits(w, 39, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 40, 1) ? "(~%s)" : "%s", B(false));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 41, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        int pd = (int)bits(w, 48, 3);
        if (pd != PT) {
            c->pred_used[pd] = 1;
            int pop = (int)bits(w, 44, 2);
            sp(&c->body, "    { uint res = %s;\n", t);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "      p%d = %s; }\n", pd, pop == 2 ? "(res == 0u)" : pop == 3 ? "(res != 0u)" : pop == 1 ? "true" : "false");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x0400000000000000ull, 0xfc00000000000000ull)) {  // LOP32I
        char x[64], y[64];
        sprintf(x, bits(w, 55, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 56, 1) ? "(~0x%08xu)" : "0x%08xu", (uint32_t)bits(w, 20, 32));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 53, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        set_reg(c, d, t);
    } else if (OPM(0x5be0000000000000ull, 0xfff8000000000000ull) || OPM(0x3c00000000000000ull, 0xfc00000000000000ull) ||
               OPM(0x0200000000000000ull, 0xfe00000000000000ull)) {  // LOP3
        uint32_t lut;
        const char *bv, *cv;
        char bb[256];
        if ((w >> 56) == 0x5b) { lut = (uint32_t)bits(w, 28, 8); bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8)); }
        else if ((w >> 58) == 0xf) { lut = (uint32_t)bits(w, 48, 8); sprintf(bb, "0x%08xu", (uint32_t)((uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19) | (bits(w, 56, 1) ? 0xFFF00000u : 0))); bv = bb; cv = R((int)bits(w, 39, 8)); }
        else { lut = (uint32_t)bits(w, 48, 8); bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, bb); cv = R((int)bits(w, 39, 8)); }
        char e[1024] = "0u";
        // sum of minterms
        int first = 1;
        for (int i = 0; i < 8; i++) {
            if (!(lut & (1u << i))) continue;
            char term[256];
            sprintf(term, "(%s%s & %s%s & %s%s)", (i & 4) ? "" : "~", R(a), (i & 2) ? "" : "~", bv, (i & 1) ? "" : "~", cv);
            if (first) { strcpy(e, term); first = 0; }
            else { strcat(e, " | "); strcat(e, term); }
        }
        sprintf(t, "(%s)", e);
        set_reg(c, d, t);
    } else if (OPM(0x5c00000000000000ull, 0xfff8000000000000ull) || OPM(0x4c00000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3800000000000000ull, 0xfef8000000000000ull)) {  // BFE
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (bits(w, 40, 1)) fail(c, "bfe brev");
        sprintf(t, sgn ? "bfe_s(%s, %s)" : "bfe_u(%s, %s)", R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5bf0000000000000ull, 0xfff8000000000000ull) || OPM(0x4bf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x36f0000000000000ull, 0xfef8000000000000ull) || OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {  // BFI
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            sprintf(t, "bfi(%s, %s, %s)", R(a), cv, bv);  // insert a into base (cbuf) with b = position/length
            // bfi with cbuf form: B = REG_39 as position/length, C = cbuf as base
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        } else {
            bv = B(false); cv = R((int)bits(w, 39, 8));
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b00000000000000ull, 0xffc0000000000000ull) || OPM(0x4e00000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3600000000000000ull, 0xfec0000000000000ull) || OPM(0x5100000000000000ull, 0xff80000000000000ull)) {  // XMAD
        // per Ryujinx's InstXmad* decoders and EmitXmad
        const char *bv, *cv;
        char cb[256], bb[256];
        bool hb, psl, mrg, ext, ha = bits(w, 53, 1);
        int cmode;
        uint64_t top = w >> 56;
        if (top == 0x5b) {  // reg, reg
            bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 35, 1); psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else if ((top & 0xfe) == 0x4e) {  // cbuf B
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 52, 1); psl = bits(w, 55, 1); mrg = bits(w, 56, 1); ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        } else if ((top & 0xfe) == 0x36) {  // imm16 B
            sprintf(bb, "0x%xu", (uint32_t)bits(w, 20, 16)); bv = bb; cv = R((int)bits(w, 39, 8));
            hb = false; psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else {  // 0x51: B = reg39, C = cbuf
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            hb = bits(w, 52, 1); psl = false; mrg = false; ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        }
        bool sa = bits(w, 48, 1), sb = bits(w, 49, 1), wcc = bits(w, 47, 1);
        if (ext || wcc) c->uses_cc = true;
        sp(&c->body, "    { uint xa = (%s >> %d) & 0xFFFFu; uint xb = (%s >> %d) & 0xFFFFu; uint xbu = %s;\n", R(a), ha ? 16 : 0, bv, hb ? 16 : 0, bv);
        if (sa) sp(&c->body, "      xa = uint(int(short(xa)));\n");
        if (sb) sp(&c->body, "      xb = uint(int(short(xb)));\n");
        sp(&c->body, "      uint prod = xa * xb;%s\n", psl ? " prod <<= 16;" : "");
        switch (cmode) {
        case 1: sp(&c->body, "      uint cc_ = %s & 0xFFFFu;\n", cv); break;                                  // clo
        case 2: sp(&c->body, "      uint cc_ = %s >> 16;\n", cv); break;                                      // chi
        case 3: sp(&c->body, "      uint cc_ = %s - (((xa >> 31) << 16) + ((xb >> 31) << 16));\n", cv); break;  // csfu
        case 4: sp(&c->body, "      uint cc_ = %s + (xbu << 16);\n", cv); break;                              // cbcc
        default: sp(&c->body, "      uint cc_ = %s;\n", cv);                                                  // cfull
        }
        if (ext) sp(&c->body, "      uint res = prod + (cc & 1u);\n");
        else sp(&c->body, "      uint res = prod + cc_;\n");
        if (wcc) sp(&c->body, "      cc = (res < prod) ? 1u : 0u;\n");
        if (mrg) sp(&c->body, "      res = (res & 0xFFFFu) | (xbu << 16);\n");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5090000000000000ull, 0xfff8000000000000ull)) {  // PSETP
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        const char *pcc = P((int)bits(w, 39, 3), bits(w, 42, 1));
        int bo1 = (int)bits(w, 24, 2), bo2 = (int)bits(w, 45, 2);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3);
        sp(&c->body, "    { bool v = (%s %s %s); bool pp = %s;\n", pa, bool_op(bo1), pb, pcc);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = v %s pp;\n", p, bool_op(bo2)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !v %s pp;\n", q, bool_op(bo2)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5088000000000000ull, 0xfff8000000000000ull)) {  // PSET
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        sprintf(t, "(((%s %s %s)) %s %s)", pa, bool_op((int)bits(w, 24, 2)), pb, bool_op((int)bits(w, 45, 2)),
                P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        int sr = (int)bits(w, 20, 8);
        switch (sr) {
        case 0x12: set_reg(c, d, "0xbf800000u"); break;  // y_direction: -1 (lower-left)
        case 0x21: set_reg(c, d, "tid.x"); break;
        case 0x22: set_reg(c, d, "tid.y"); break;
        case 0x23: set_reg(c, d, "tid.z"); break;
        case 0x25: set_reg(c, d, "ctaid.x"); break;
        case 0x26: set_reg(c, d, "ctaid.y"); break;
        case 0x27: set_reg(c, d, "ctaid.z"); break;
        // Lane id and lane masks: Vulkan's subgroup = the Maxwell warp where the subgroup is 32 wide (NVIDIA). They were 0,
        // which broke warp-aggregated atomics (one lane adds for the ballot's lanes; each lane's slot = popc(ballot & ltmask)):
        // a Z-A compute shader's per-entry counters then grew without end, its GPU time with them (0.04 -> 6 ms a frame).
        case 0x00: set_reg(c, d, "gl_SubgroupInvocationID"); break;  // SR_LANEID
        case 0x38: set_reg(c, d, "gl_SubgroupEqMask.x"); break;      // SR_EQMASK
        case 0x39: set_reg(c, d, "gl_SubgroupLtMask.x"); break;      // SR_LTMASK
        case 0x3a: set_reg(c, d, "gl_SubgroupLeMask.x"); break;      // SR_LEMASK
        case 0x3b: set_reg(c, d, "gl_SubgroupGtMask.x"); break;      // SR_GTMASK
        case 0x3c: set_reg(c, d, "gl_SubgroupGeMask.x"); break;      // SR_GEMASK
        default: sprintf(t, "0u /* sr %#x */", sr); set_reg(c, d, t);
        }
    } else if (OPM(0x5c30000000000000ull, 0xfff8000000000000ull) || OPM(0x4c30000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3830000000000000ull, 0xfef8000000000000ull)) {  // FLO
        const char *bv = B(false);
        sprintf(t, bits(w, 48, 1) ? "(int(%s) < 0 ? (31u - clz(~%s)) : (31u - clz(%s)))" : "(31u - clz(%s))", bv, bv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c08000000000000ull, 0xfff8000000000000ull) || OPM(0x4c08000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3808000000000000ull, 0xfef8000000000000ull)) {  // POPC
        sprintf(t, bits(w, 40, 1) ? "popcount(~%s)" : "popcount(%s)", B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5c38000000000000ull, 0xfff8000000000000ull) || OPM(0x4c38000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3838000000000000ull, 0xfef8000000000000ull)) {  // IMUL
        if (bits(w, 39, 1)) sprintf(t, bits(w, 40, 1) ? "uint(mulhi(int(%s), int(%s)))" : "mulhi(%s, %s)", R(a), B(false));
        else sprintf(t, "(%s * %s)", R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x1f00000000000000ull, 0xff00000000000000ull)) {  // IMUL32I
        sprintf(t, "(%s * 0x%08xu)", R(a), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ce0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ce0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38e0000000000000ull, 0xfef8000000000000ull)) {  // I2I
        int dt = (int)((bits(w, 8, 2)) | (bits(w, 12, 1) << 2)), st = (int)((bits(w, 10, 2)) | (bits(w, 13, 1) << 2));
        int sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        int ssz = st & 3;
        bool ssg = st & 4;
        if (ssz == 0) sprintf(src, ssg ? "int(char((%s >> %d) & 0xFFu))" : "int((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (ssz == 1) sprintf(src, ssg ? "int(short((%s >> %d) & 0xFFFFu))" : "int((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, "int(%s)", bv);
        if (bits(w, 49, 1)) { sprintf(u, "abs(%s)", src); strcpy(src, u); }
        if (bits(w, 45, 1)) { sprintf(u, "(-%s)", src); strcpy(src, u); }
        int dsz = dt & 3;
        bool dsg = dt & 4, sat = bits(w, 50, 1);
        if (sat && !ssg) {  // .SAT from an unsigned source: compare unsigned (a U32 value above 0x7FFFFFFF isn't negative)
            static const char *const hi[2][3] = {{"0xFFu", "0xFFFFu", ""}, {"0x7Fu", "0x7FFFu", "0x7FFFFFFFu"}};
            if (dsz < 2 || dsg) sprintf(t, "min(uint(%s), %s)", src, hi[dsg][dsz < 2 ? dsz : 2]);
            else sprintf(t, "uint(%s)", src);
        } else if (dsz == 0) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -128, 127))" : "uint(clamp(%s, 0, 255))") : "(uint(%s) & 0xFFu)", src);
        else if (dsz == 1) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -32768, 32767))" : "uint(clamp(%s, 0, 65535))") : "(uint(%s) & 0xFFFFu)", src);
        else if (sat && !dsg) sprintf(t, "uint(max(%s, 0))", src);  // S32 -> U32 .SAT: negatives become 0
        else sprintf(t, "uint(%s)", src);
        set_reg(c, d, t);
        if (bits(w, 47, 1)) {  // .CC: zero/sign flags of the result
            c->uses_cc = true;
            sp(&c->body, "    { uint ccv = %s; ccz = ccv == 0u; ccs = int(ccv) < 0; }\n", t);
            c->ccz_modelled = true;
        }
    } else if (OPM(0x5bc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36c0000000000000ull, 0xfef0000000000000ull) || OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) {  // PRMT
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) { bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); }
        else { bv = B(false); cv = R((int)bits(w, 39, 8)); }
        if (bits(w, 48, 4)) fail(c, "prmt mode");
        sprintf(t, "prmt(%s, %s, %s)", R(a), cv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5cf8000000000000ull, 0xfff8000000000000ull) || OPM(0x38f8000000000000ull, 0xfef8000000000000ull) ||
               OPM(0x5bf8000000000000ull, 0xfff8000000000000ull) || OPM(0x36f8000000000000ull, 0xfef8000000000000ull)) {  // SHF
        // funnel shift of hi:lo (C:A). .W (bit 50) wraps the shift amount, otherwise it's clamped to the width;
        // bits 37-38: 2 .U64 / 3 .S64 shift the full 64 bits (width 64), else 32. The immediate is 6 bits (20-25:
        // bits 37-38 of the 19-bit B immediate are the width field). .CC / .X / .HI (47-49) aren't modelled: ignored as
        // before (LAS Recomp d4c2ff6 fails the program on them, which would drop its draws here).
        bool right = (w >> 56) == 0x5c || (w >> 57) == (0x38 >> 1);
        char shb[16];
        const char *sh = ((w >> 60) == 3) ? (sprintf(shb, "%uu", (unsigned)bits(w, 20, 6)), shb) : R((int)bits(w, 20, 8));
        const char *hi = R((int)bits(w, 39, 8));
        int width = bits(w, 38, 1) ? 64 : 32;
        bool s64 = bits(w, 37, 2) == 3, wrap = bits(w, 50, 1);
        sp(&c->body, wrap ? "    { uint s = %s & %du; ulong v = (ulong(%s) << 32) | ulong(%s);\n"
                          : "    { uint s = min(%s, %du); ulong v = (ulong(%s) << 32) | ulong(%s);\n",
           sh, wrap ? width - 1 : width, hi, R(a));
        if (right && width == 64)
            sprintf(t, "s >= 64u ? %s : uint(%s >> s)", s64 ? "uint(int(v >> 32) >> 31)" : "0u", s64 ? "long(v)" : "v");
        else if (right) sprintf(t, "uint(v >> s)");
        else if (width == 64) sprintf(t, "s >= 64u ? 0u : uint((v << s) >> 32)");
        else sprintf(t, "uint((v << s) >> 32)");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = %s;\n", d, t); }
        sp(&c->body, "    }\n");
    } else if (OPM(0xed00000000000000ull, 0xff00000000000000ull)) {  // ATOM (global)
        emit_gatomic(c, w, false);
    } else if (OPM(0xebf8000000000000ull, 0xfff8000000000000ull)) {  // RED (global)
        emit_gatomic(c, w, true);
    } else if (OPM(0xeed0000000000000ull, 0xfff8000000000000ull) || OPM(0xeed8000000000000ull, 0xfff8000000000000ull)) {  // LDG / STG
        bool store = bits(w, 51, 1);
        bool wide = bits(w, 45, 1);
        int size = (int)bits(w, 48, 3);
        int64_t off = sbits(w, 20, 24);
        c->info->uses_gmem = true;
        char addr[256];
        if (a == RZ) sprintf(addr, "ulong(%lld)", (long long)off);
        else if (wide) sprintf(addr, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(a + 1), R(a), (long long)off);
        else sprintf(addr, "(ulong(%s) + ulong(%lld))", R(a), (long long)off);
        char lim[32];  // (GLSL: the size imported, from the heap's uniform)
        if (c->glsl) snprintf(lim, sizeof lim, "gmb_.gsize");
        else snprintf(lim, sizeof lim, "0x%llxul", (unsigned long long)maxwell_gmem_size);
        sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga < %s;\n", addr, (unsigned long long)maxwell_gmem_base, lim);
        static const char *const ld_small[] = {"uint(gmem[ga])", "uint(int(char(gmem[ga])))",
                                               "uint(*(device ushort *)(gmem + ga))", "uint(int(*(device short *)(gmem + ga)))"};
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        if (c->glsl) {  // gmem_.w[]: 32-bit words; sub-word accesses within the containing word
            static const char *const gl_small[] = {
                "(gmem_.w[uint(ga >> 2)] >> (uint(ga & 3ul) * 8u)) & 0xFFu",
                "uint(sx8(gmem_.w[uint(ga >> 2)] >> (uint(ga & 3ul) * 8u)))",
                "(gmem_.w[uint(ga >> 2)] >> (uint(ga & 2ul) * 8u)) & 0xFFFFu",
                "uint(sx16(gmem_.w[uint(ga >> 2)] >> (uint(ga & 2ul) * 8u)))"};
            if (!store) {
                if (size < 4) {
                    if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      if (ok) r%d = %s; else r%d = 0u;\n", d, gl_small[size], d); }
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) {
                            c->reg_used[d + k] = 1;
                            sp(&c->body, "      if (ok) r%d = gmem_.w[uint(ga >> 2) + %du]; else r%d = 0u;\n", d + k, k, d + k);
                        }
                }
            } else {
                c->uses_gmem_store = true;
                c->info->gmem_stores = true;
                if (size < 4)
                    sp(&c->body, "      if (ok) { uint wi = uint(ga >> 2); uint sh = uint(ga & %s) * 8u; atomicAnd(gmem_.w[wi], ~(0x%xu << sh)); "
                       "atomicOr(gmem_.w[wi], (%s & 0x%xu) << sh); }\n", size < 2 ? "3ul" : "2ul", size < 2 ? 0xFFu : 0xFFFFu, R(d),
                       size < 2 ? 0xFFu : 0xFFFFu);
                else
                    for (int k = 0; k < nw; k++)
                        sp(&c->body, "      if (ok) gmem_.w[uint(ga >> 2) + %du] = %s;\n", k, R(d == RZ ? RZ : d + k));
            }
        } else if (!store) {
            if (size < 4) {
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = ok ? %s : 0u;\n", d, ld_small[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = ok ? *(device uint *)(gmem + ga + %du) : 0u;\n", d + k, k * 4); }
            }
        } else {
            if (size == 0 || size == 1) sp(&c->body, "      if (ok) gmem[ga] = uchar(%s);\n", R(d));
            else if (size == 2 || size == 3) sp(&c->body, "      if (ok) *(device ushort *)(gmem + ga) = ushort(%s);\n", R(d));
            else
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      if (ok) *(device uint *)(gmem + ga + %du) = %s;\n", k * 4, R(d == RZ ? RZ : d + k));
        }
        sp(&c->body, "    }\n");
    } else if (emit_half(c, w)) {
    } else if (OPM(0x50d8000000000000ull, 0xfff8000000000000ull)) {  // VOTE (.ALL / .ANY / .EQ): predicate, ballot
        if (!c->glsl) { fail(c, "vote (MSL)"); return; }
        int d = (int)bits(w, 0, 8), sp_ = (int)bits(w, 39, 3), vp = (int)bits(w, 45, 3), mode = (int)bits(w, 48, 2);
        bool spn = bits(w, 42, 1);
        static const char *const fn[3] = {"subgroupAll", "subgroupAny", "subgroupAllEqual"};
        if (mode > 2) { fail(c, "vote mode %d", mode); return; }
        const char *src = P(sp_, spn);
        sp(&c->body, "    { bool vsrc = %s;\n", src);
        if (vp != PT) { c->pred_used[vp] = 1; sp(&c->body, "      p%d = %s(vsrc);\n", vp, fn[mode]); }
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = subgroupBallot(vsrc).x;\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x50f8000000000000ull, 0xfff8000000000000ull)) {  // FSWZADD
        // per quad lane (laneid & 3), 2 bits of the swizzle pick: a+b, b-a, a-b, b
        uint32_t swz = (uint32_t)bits(w, 28, 8);
        if (c->stage != STAGE_FRAGMENT) {
            sprintf(t, "as_type<uint>(%s + %s)", F(R(a)), F(R((int)bits(w, 20, 8))));
        } else {
            c->uses_lane = true;
            sprintf(t, "as_type<uint>(fswzadd(%s, %s, 0x%02xu, lane))", F(R(a)), F(R((int)bits(w, 20, 8))), swz);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b40000000000000ull, 0xfff0000000000000ull) || OPM(0x4b40000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3640000000000000ull, 0xfef0000000000000ull) || OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {  // ICMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, false, bb);
            cv = R((int)bits(w, 39, 8));
        }
        icmp_expr(cmp, (int)bits(w, 49, 3), cv, "0u", bits(w, 48, 1));
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0xef40000000000000ull, 0xfff8000000000000ull) || OPM(0xef50000000000000ull, 0xfff8000000000000ull)) {  // LDL / STL
        // thread-private local memory, modelled as a word array (byte/short accesses within a word)
        bool store = bits(w, 52, 1);
        int size = (int)bits(w, 48, 3);
        int32_t off = (int32_t)sbits(w, 20, 24);
        c->uses_local = true;
        char addr[128];
        if (a == RZ) sprintf(addr, "%du", off);
        else sprintf(addr, "(%s + %du)", R(a), (uint32_t)off);
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        sp(&c->body, "    { uint la = %s;\n", addr);
        if (!store) {
            if (size < 4) {
                static const char *const ext[] = {"(v >> sh) & 0xFFu", "uint(int(char((v >> sh) & 0xFFu)))", "(v >> sh) & 0xFFFFu",
                                                  "uint(int(short((v >> sh) & 0xFFFFu)))"};
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      uint v = lmem[(la >> 2) & %du]; uint sh = (la & 3u) * 8u; r%d = %s;\n", LMEM_WORDS - 1, d, ext[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = lmem[((la >> 2) + %du) & %du];\n", d + k, k, LMEM_WORDS - 1); }
            }
        } else {
            if (size < 4) {
                uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                sp(&c->body, "      uint sh = (la & 3u) * 8u; uint i = (la >> 2) & %du; lmem[i] = (lmem[i] & ~(0x%xu << sh)) | ((%s & 0x%xu) << sh);\n",
                   LMEM_WORDS - 1, m, R(d), m);
            } else {
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      lmem[((la >> 2) + %du) & %du] = %s;\n", k, LMEM_WORDS - 1, R(d == RZ ? RZ : d + k));
            }
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0xef10000000000000ull, 0xfff8000000000000ull)) {  // SHFL
        int mode = (int)bits(w, 30, 2);
        char bb[32];
        const char *bv = bits(w, 28, 1) ? (sprintf(bb, "%uu", (unsigned)bits(w, 20, 5)), bb) : R((int)bits(w, 20, 8));
        int pd = (int)bits(w, 48, 3);
        if (c->stage == STAGE_VERTEX) {
            set_reg(c, d, R(a));  // no SIMD-group functions in vertex functions: lane-local value
        } else if (c->glsl) {  // a quad xor (lane mask < 4) is the subgroup xor within the quad
            static const char *const fn[] = {"subgroupShuffle", "subgroupShuffleUp", "subgroupShuffleDown", "subgroupShuffleXor"};
            sprintf(t, "%s(%s, uint(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        } else if (mode == 3) {
            bool quad = bits(w, 28, 1) && bits(w, 20, 5) < 4;
            sprintf(t, quad ? "quad_shuffle_xor(%s, ushort(%s))" : "simd_shuffle_xor(%s, ushort(%s))", R(a), bv);
            set_reg(c, d, t);
        } else {
            static const char *const fn[] = {"simd_shuffle", "simd_shuffle_up", "simd_shuffle_down"};
            sprintf(t, "%s(%s, ushort(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        }
        if (pd != PT) { c->pred_used[pd] = 1; sp(&c->body, "    p%d = true;\n", pd); }
    } else if (OPM(0x38f0000000000000ull, 0xfef8000000000000ull) || OPM(0x5cf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4cf0000000000000ull, 0xfff8000000000000ull)) {  // R2P
        if (bits(w, 40, 1)) { fail(c, "r2p cc"); }
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        for (int i = 0; i < 7; i++) {
            c->pred_used[i] = 1;
            sp(&c->body, c->glsl ? "    if (((%s >> %du) & 1u) != 0u) p%d = ((%s >> %du) & 1u) != 0u;\n"
                                 : "    if ((%s >> %du) & 1u) p%d = ((%s >> %du) & 1u) != 0u;\n", mask, i, i, R(a), shift + i);
        }
    } else if (OPM(0x38e8000000000000ull, 0xfef8000000000000ull) || OPM(0x5ce8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4ce8000000000000ull, 0xfff8000000000000ull)) {  // P2R
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        if (bits(w, 40, 1)) {  // .CC: the flags Z, S, C, O as bits 0-3 (overflow isn't tracked: 0)
            c->uses_cc = true;
            sp(&c->body, "    { uint pr = (ccz ? 1u : 0u) | (ccs ? 2u : 0u) | ((cc & 1u) << 2);\n");
            sp(&c->body, "      uint m = (%s & 0xFu) << %du;\n", mask, shift);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
            sp(&c->body, "    }\n");
            goto p2r_done;
        }
        sp(&c->body, "    { uint pr = 0u;\n");
        for (int i = 0; i < 7; i++) { c->pred_used[i] = 1; sp(&c->body, "      pr |= (p%d ? 1u : 0u) << %du;\n", i, i); }
        sp(&c->body, "      uint m = (%s & 0x7Fu) << %du;\n", mask, shift);
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
        sp(&c->body, "    }\n");
    p2r_done:;
    } else if (OPM(0xe250000000000000ull, 0xfff0000000000020ull)) {  // BRX reg + imm (relative)
        // computed targets can name a scheduling word; execution continues at the next slot (as for BRA)
        if (c->fwd) {  // (its targets found ahead: translate_impl)
            int b = 0;
            while (b < c->nbrx && c->brx_at[b] != pc / 8) b++;
            sp(&c->body, "    { uint t = %uu + %s + 0x%08xu; if (((t >> 3) & 3u) == 0u) t += 8u;\n      switch (t) {", pc + 8, R(a),
               (uint32_t)sbits(w, 20, 24));
            for (int j = 0; b < c->nbrx && j < c->brx_nto[b]; j++) {
                uint32_t to = c->brx_to[b][j];
                int k = 0;
                while (k < j && c->brx_to[b][k] != to) k++;
                if (k < j) continue;  // (a target twice)
                char set[64];
                jump_set(set, sizeof set, to, pc);
                sp(&c->body, "%s case %uu: %s break;", j % 4 == 3 ? "\n       " : "", to, set);
            }
            sp(&c->body, " default: exit_now = true; break; }\n      break; }\n");
        } else
            sp(&c->body, "    { uint t = %uu + %s + 0x%08xu; if (((t >> 3) & 3u) == 0u) t += 8u; pc = t; continue; }\n", pc + 8, R(a),
               (uint32_t)sbits(w, 20, 24));
    } else if (OPM(0x50a0000000000000ull, 0xfff8000000000000ull)) {  // CSETP (zero/sign flags only)
        int test = (int)bits(w, 8, 5);
        const char *cond;
        switch (test) {
        case 0: cond = "false"; break;
        case 15: cond = "true"; break;
        case 2: case 10: cond = "ccz"; break;
        case 5: case 13: cond = "!ccz"; break;
        case 1: case 9: cond = "ccs"; break;
        case 6: case 14: cond = "!ccs"; break;
        case 4: case 12: cond = "(!ccz && !ccs)"; break;
        case 3: case 11: cond = "(ccz || ccs)"; break;
        default: cond = "false"; fail(c, "csetp test %d", test);
        }
        c->uses_cc = true;
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cond, pc_);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5098000000000000ull, 0xfff8000000000000ull)) {  // CSET
        fail(c, "cset");
    } else if (OPM(0xf0a8000000000000ull, 0xfff8000000000000ull)) {  // BAR
        if (!c->glsl) sp(&c->body, "    threadgroup_barrier(mem_flags::mem_threadgroup);\n");
        else if (c->stage == STAGE_COMPUTE) sp(&c->body, "    barrier();\n");
        else sp(&c->body, "    /* bar */\n");  // GLSL barrier() is compute / tessellation only
    } else {
        fail(c, "unknown instruction %016llx at %#x", (unsigned long long)w, pc);
    }
    (void)op13;
    if (guarded) sp(&c->body, "  }\n");
}

// ------------------------------------------------------------------ program assembly
static bool is_branch_target_op(uint64_t w) {
    return ((w & 0xfff0000000000020ull) == 0xe240000000000000ull);
}

// ------------------------------------------------------------------ GLSL output
static bool id_start(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static bool id_char(char ch) { return id_start(ch) || (ch >= '0' && ch <= '9'); }

// MSL-spelled instruction body -> GLSL (token-aware; comments and numbers are copied as they are).
static void glsl_rewrite(Ctx *c, Str *o, const char *src) {
    static const char *const map[][2] = {
        {"float2", "vec2"}, {"float3", "vec3"}, {"float4", "vec4"}, {"int2", "ivec2"}, {"int3", "ivec3"},
        {"int4", "ivec4"}, {"uint2", "uvec2"}, {"uint3", "uvec3"}, {"uint4", "uvec4"}, {"bool2", "bvec2"},
        {"ulong", "uint64_t"}, {"long", "int64_t"}, {"rsqrt", "inversesqrt"}, {"rint", "roundEven"},
        {"front_facing", "gl_FrontFacing"}, {"point_coord", "gl_PointCoord"}, {"lane", "gl_SubgroupInvocationID"},
        // Maxwell's instance id excludes the draw's base instance (like GL's gl_InstanceID); Vulkan's gl_InstanceIndex
        // includes it (PLA batches its text glyphs with a base instance: they read the wrong glyph's data, off screen)
        {"vid", "uint(gl_VertexIndex)"}, {"iid", "uint(gl_InstanceIndex - gl_BaseInstanceARB)"},
    };
    const char *p = src;
    while (*p) {
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            e = e ? e + 2 : p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            const char *e = strchr(p, '\n');
            if (!e) e = p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (*p >= '0' && *p <= '9') {  // number (0x..u, 1.0f, ...)
            const char *e = p;
            while (id_char(*e) || *e == '.') e++;
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (!id_start(*p)) { sp(o, "%c", *p); p++; continue; }
        const char *e = p;
        while (id_char(*e)) e++;
        int n = (int)(e - p);
        char id[64];
        if (n >= (int)sizeof id || (p > src && p[-1] == '.')) {  // member / swizzle
            sp(o, "%.*s", n, p);
            p = e;
            continue;
        }
        memcpy(id, p, n);
        id[n] = 0;
        p = e;
        if (!strcmp(id, "as_type") && *p == '<') {
            const char *gt = strchr(p, '>');
            if (!gt) { fail(c, "glsl: bad as_type"); return; }
            int tn = (int)(gt - p - 1);
            if (tn == 5 && !strncmp(p + 1, "float", 5)) sp(o, "as_f");
            else if (tn == 4 && !strncmp(p + 1, "uint", 4)) sp(o, "as_u");
            else if (tn == 3 && !strncmp(p + 1, "int", 3)) sp(o, "as_i");
            else if (tn == 5 && !strncmp(p + 1, "half2", 5)) sp(o, "unpack_h2");
            else { fail(c, "glsl: as_type<%.*s>", tn, p + 1); return; }
            p = gt + 1;
            continue;
        }
        if ((!strcmp(id, "in") || !strcmp(id, "out")) && *p == '.') {  // stage I/O struct members
            const char *m = p + 1, *me = m;
            while (id_char(*me)) me++;
            char mem[32];
            snprintf(mem, sizeof mem, "%.*s", (int)(me - m), m);
            if (!strcmp(mem, "pos")) sp(o, c->stage == STAGE_FRAGMENT ? "gl_FragCoord" : "gl_Position");
            else if (!strcmp(mem, "psize")) sp(o, "gl_PointSize");
            else sp(o, "%s", mem);  // a<i> (vertex input), v<i> (varying)
            p = me;
            continue;
        }
        if ((!strcmp(id, "char") || !strcmp(id, "short")) && *p == '(') { sp(o, id[0] == 'c' ? "sx8" : "sx16"); continue; }
        if (!strcmp(id, "tid") || !strcmp(id, "ctaid")) {
            sp(o, c->stage != STAGE_COMPUTE ? "uvec3(0u)" : id[0] == 't' ? "gl_LocalInvocationID" : "gl_WorkGroupID");
            continue;
        }
        if (id[0] == 'r' && n > 1 && n < 6) {  // registers beyond r255 (RZ) come from out-of-range encodings
            bool num = true;
            for (int i = 1; i < n; i++) num = num && id[i] >= '0' && id[i] <= '9';
            if (num && atoi(id + 1) > 255) { fail(c, "register %s out of range", id); return; }
        }
        const char *rep = id;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcmp(id, map[i][0])) { rep = map[i][1]; break; }
        sp(o, "%s", rep);
    }
}

// The guest heap's words in the GLSL: gmem_.w[<index>] (how the emitters write them, as MSL's device array) becomes
// GW(<index>).v - a buffer reference to that one word, its address made in 64 bits. Indexing a uint array in a buffer
// reference wrapped at 4 GB: the game's text, in memory above that, came out blank.
static char *glsl_gmem_words(char *src) {
    static const char pat[] = "gmem_.w[";
    size_t n = strlen(src), cap = n + 64;
    char *o = malloc(cap);
    size_t w = 0;
    for (size_t i = 0; i < n;) {
        if (!strncmp(src + i, pat, sizeof pat - 1)) {
            size_t j = i + sizeof pat - 1;
            int depth = 1;
            size_t k = j;
            while (k < n && depth) { if (src[k] == '[') depth++; else if (src[k] == ']') depth--; if (depth) k++; }
            if (k < n) {
            size_t need = w + (k - j) + 16;
            if (need > cap) { cap = need * 2; o = realloc(o, cap); }
            memcpy(o + w, "GW(", 3), w += 3;
            memcpy(o + w, src + j, k - j), w += k - j;
            memcpy(o + w, ").v", 3), w += 3;
            i = k + 1;
            continue;
            }  // (unbalanced: copied as it is)
        }
        if (w + 2 > cap) { cap *= 2; o = realloc(o, cap); }
        o[w++] = src[i++];
    }
    o[w] = 0;
    free(src);
    return o;
}

static const char glsl_prelude[] =
    "#version 460\n"
    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
    "#extension GL_EXT_buffer_reference : require\n"
    "#extension GL_KHR_shader_subgroup_basic : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle_relative : require\n"
    "#extension GL_KHR_shader_subgroup_vote : require\n"
    "#extension GL_KHR_shader_subgroup_ballot : require\n"
    "#extension GL_EXT_texture_shadow_lod : require\n"
    "#extension GL_ARB_shader_draw_parameters : require\n"
    "#extension GL_ARB_shader_viewport_layer_array : require\n"  // (gl_Layer from the vertex shader: 3D / layered targets)
    "#define cbr(cb, off) cb[min((off) >> 4, uint(cb.length() - 1))][((off) >> 2) & 3u]\n"
    "uint as_u(float x) { return floatBitsToUint(x); }\n"
    "uint as_u(uint x) { return x; }\n"
    "uint as_u(int x) { return uint(x); }\n"
    "float as_f(uint x) { return uintBitsToFloat(x); }\n"
    "float as_f(int x) { return intBitsToFloat(x); }\n"
    "float as_f(float x) { return x; }\n"
    "int as_i(uint x) { return int(x); }\n"
    "int as_i(float x) { return floatBitsToInt(x); }\n"
    "int sx8(uint v) { return bitfieldExtract(int(v), 0, 8); }\n"
    "int sx8(int v) { return bitfieldExtract(v, 0, 8); }\n"
    "int sx16(uint v) { return bitfieldExtract(int(v), 0, 16); }\n"
    "int sx16(int v) { return bitfieldExtract(v, 0, 16); }\n"
    // MSL semantics: saturate(NaN) = 0, fmin/fmax return the non-NaN operand
    "float saturate(float x) { return isnan(x) ? 0.0f : clamp(x, 0.0f, 1.0f); }\n"
    "vec2 saturate(vec2 x) { return vec2(saturate(x.x), saturate(x.y)); }\n"
    "float fmin(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : min(a, b)); }\n"
    "float fmax(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : max(a, b)); }\n"
    "uint clz(uint v) { return v == 0u ? 32u : uint(31 - findMSB(v)); }\n"
    "uint popcount(uint v) { return uint(bitCount(v)); }\n"
    "uint mulhi(uint a, uint b) { uint hi, lo; umulExtended(a, b, hi, lo); return hi; }\n"
    "int mulhi(int a, int b) { int hi, lo; imulExtended(a, b, hi, lo); return hi; }\n"
    "uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return bitfieldExtract(v, int(pos), int(min(len, 32u - pos))); }\n"
    "uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(bitfieldExtract(int(v), int(pos), int(min(len, 32u - pos)))); }\n"
    "uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return bitfieldInsert(base, ins, int(pos), int(min(len, 32u - pos))); }\n"
    "float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n";
// rsc: an integer texel coordinate of the guest's size into a texture rendered at another scale. Pre-raster and
// compute stages take each guest texel's centre. Fragment shaders at an integer scale k (4K = 2) add the fragment's own
// position inside its k x k block: a fetch at the fragment's position (soft particles fading on the depth buffer) is
// then exact at every pixel instead of once per k x k block (square artifacts on particles at 4K, 2026-10-18).
static const char glsl_rsc[] =
    "uvec2 rsc(uvec2 c, vec4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uvec2(floor((vec2(c) + 0.5f) * s.xy)); }\n";
static const char glsl_rsc_fragment[] =
    "uvec2 rsc(uvec2 c, vec4 s) {\n"
    "  if (s.x == 1.0f && s.y == 1.0f) return c;\n"
    "  if (s.x >= 1.0f && s.y >= 1.0f && fract(s.x) == 0.0f && fract(s.y) == 0.0f)\n"
    "    return uvec2(vec2(c) * s.xy + mod(floor(gl_FragCoord.xy), s.xy));\n"
    "  return uvec2(floor((vec2(c) + 0.5f) * s.xy));\n"
    "}\n";
// The half <-> float helpers (glsl_finish: glsl_h2f_exact or glsl_h2f_native, then glsl_prelude_half).
// Half -> float in vertex (and other pre-raster) shaders: by bits, not unpackHalf2x16. NVIDIA's driver recognises
// unpacked halves and runs some of the fp32 math after them in native fp16, differently per shader, so a depth
// pre-pass and its colour pass (EQUAL test) placed wind-animated foliage apart: black slashes / flicker (notes/PLAN.md
// 2026-10-08).
static const char glsl_h2f_exact[] =
    "float h2f(uint h) { uint s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;"
    " if (e == 0u) { float d = float(m) * 5.9604644775390625e-8; return s != 0u ? -d : d; }"
    " if (e == 31u) return uintBitsToFloat(s | 0x7F800000u | (m << 13));"
    " return uintBitsToFloat(s | ((e + 112u) << 23) | (m << 13)); }\n"
    "vec2 unpack_h2(uint v) { return vec2(h2f(v & 0xFFFFu), h2f(v >> 16)); }\n";
// ... and in fragment / compute shaders the native conversion: positions don't depend on them, and the by-bits
// version (~12 integer ops and two branches per half) was half of the main lighting pass's GPU time on an Intel UHD
// 770 and a third on an RTX 3080 Ti (the game's shaders use packed halves heavily). LARECOMP_VK_EXACT_H2F=1: by bits
// in every stage.
static const char glsl_h2f_native[] =
    "float h2f(uint h) { return unpackHalf2x16(h).x; }\n"
    "vec2 unpack_h2(uint v) { return unpackHalf2x16(v); }\n";
static const char glsl_prelude_half[] =
    "vec2 hx(uint v, uint swz) { if (swz == 1u) return vec2(uintBitsToFloat(v)); vec2 h = unpack_h2(v); return swz == 2u ? h.xx : (swz == 3u ? h.yy : h); }\n"
    "uint hpack(vec2 r, uint merge, uint old) { if (merge == 1u) return floatBitsToUint(r.x); uint lo = packHalf2x16(vec2(r.x, 0.0f)), hi = packHalf2x16(vec2(r.y, 0.0f)); if (merge == 2u) return (old & 0xFFFF0000u) | lo; if (merge == 3u) return (old & 0xFFFFu) | (hi << 16); return lo | (hi << 16); }\n"
    "uint prmt(uint a, uint b, uint sel) { uint r = 0u; for (uint i = 0u; i < 4u; i++) { uint s = (sel >> (i * 4u)) & 7u; uint bt = s < 4u ? (a >> (s * 8u)) & 0xFFu : (b >> ((s - 4u) * 8u)) & 0xFFu; r |= bt << (i * 8u); } return r; }\n";

static const char *glsl_sampler_type(const ShaderTexture *t) {
    static const char *const names[][2] = {
        {"sampler1D", "sampler1DShadow"}, {"sampler2D", "sampler2DShadow"}, {"sampler3D", "sampler3D"},
        {"samplerCube", "samplerCubeShadow"}, {"sampler2DArray", "sampler2DArrayShadow"},
        {"samplerCubeArray", "samplerCubeArrayShadow"}, {"sampler1DArray", "sampler1DArrayShadow"}};
    int ty = t->type <= TEXTYPE_1D_ARRAY ? t->type : TEXTYPE_2D;
    if (t->stencil) {
        static const char *const unames[] = {"usampler1D", "usampler2D", "usampler3D", "usamplerCube", "usampler2DArray",
                                             "usamplerCubeArray", "usampler1DArray"};
        return unames[ty];
    }
    return names[ty][t->shadow ? 1 : 0];
}

// Declarations + entry point around the (rewritten) body; mirrors the MSL assembly in maxwell_translate.
static ShaderInfo *glsl_finish(Ctx *c, bool any_branch, bool dbg) {
    ShaderInfo *info = c->info;
    const ShaderKey *key = c->key;
    int stage = c->stage;
    Str body = {0};
    glsl_rewrite(c, &body, c->body.buf ? c->body.buf : "");
    if (info->uses_gmem && (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX))) fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
    if (info->cbuf_mask >> 18) fail(c, "cbuf index out of range");
    for (int i = 0; i < info->ntex; i++)
        if (info->tex[i].shadow && info->tex[i].type == TEXTYPE_3D) fail(c, "3d shadow texture");
    if (c->fail) { free(body.buf); free(c->body.buf); free(c->fwd_decl.buf); free(c); return info; }
    int set = stage == STAGE_FRAGMENT ? 1 : 0;
    Str s = {0};
    sp(&s, "%s%s", glsl_prelude, stage == STAGE_FRAGMENT ? glsl_rsc_fragment : glsl_rsc);
    static int exact = -1;
    if (exact < 0) exact = getenv("LARECOMP_VK_EXACT_H2F") && atoi(getenv("LARECOMP_VK_EXACT_H2F")) != 0;
    sp(&s, "%s%s", !exact && (stage == STAGE_FRAGMENT || stage == STAGE_COMPUTE) ? glsl_h2f_native : glsl_h2f_exact,
       glsl_prelude_half);
    for (int i = 0; i < 18; i++) {
        if (!(info->cbuf_mask & (1u << i))) continue;
        uint32_t n = info->cbuf_size[i] / 16;
        if (c->cbuf_need[i] > n) n = c->cbuf_need[i];
        if ((info->cbuf_dynamic & (1u << i)) && n < 4096) n = 4096;
        if (!n) n = 1;
        sp(&s, "layout(std140, set = %d, binding = %d) uniform CB%d { uvec4 cb%d[%u]; };\n", set, i, i, i, n);
    }
    if (info->uses_gmem) {  // the guest heap through its GPU address (runtime: exec.c gmem_ref_info): 64-bit, no 4 GB limit
        // (each word's address made in 64 bits - glsl_gmem_words: a uint index into a runtime array wrapped at 4 GB)
        sp(&s, "layout(buffer_reference, std430, buffer_reference_align = 4) %sbuffer GWord { uint v; };\n",
           c->uses_gmem_store ? "" : "readonly ");
        sp(&s, "layout(std140, set = %d, binding = %d) uniform GMemBase { uint64_t gbase; uint64_t gsize; } gmb_;\n", set,
           GMEM_BUFFER_INDEX);
        sp(&s, "#define GW(i) GWord(gmb_.gbase + uint64_t(i) * 4ul)\n");
    }
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&s, "layout(std140, set = %d, binding = 18) uniform RScale { vec4 rscale[65]; };\n", set);
        info->uses_rscale = true;
    }
    bool tint = stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&s, "layout(std140, set = %d, binding = 19) uniform Tint { vec4 ltint[2]; };\n", set);
        info->uses_tint = true;
    }
    for (int i = 0; i < info->nimg; i++) {
        const ShaderImage *im = &info->img[i];
        if (im->atomic) sp(&s, "layout(set = %d, binding = %d, r32ui) uniform uimage%s img%d;\n", set, 64 + i, image_glsl_dim(im->dim), i);
        else sp(&s, "layout(set = %d, binding = %d) uniform writeonly %simage%s img%d;\n", set, 64 + i,
                im->kind == 2 ? "u" : im->kind == 1 ? "i" : "", image_glsl_dim(im->dim), i);
    }
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        sp(&s, "layout(set = %d, binding = %d) uniform %s t%d;\n", set, 32 + i, glsl_sampler_type(t), i);
        // size of level l as (width, height, depth or layers)
        switch (t->type) {
        case TEXTYPE_1D: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1, 1); }\n", i, i); break;
        case TEXTYPE_1D_ARRAY: sp(&s, "ivec3 tsize%d(int l) { ivec2 s = textureSize(t%d, l); return ivec3(s.x, 1, s.y); }\n", i, i); break;
        case TEXTYPE_3D: case TEXTYPE_2D_ARRAY: case TEXTYPE_CUBE_ARRAY:
            sp(&s, "ivec3 tsize%d(int l) { return textureSize(t%d, l); }\n", i, i); break;
        default: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1); }\n", i, i);
        }
    }
    if (stage == STAGE_VERTEX) {
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "layout(location = %d) in %s a%d;\n", i, kind == 1 ? "ivec4" : kind == 2 ? "uvec4" : "vec4", i);
        }
        // depth pre-pass + EQUAL test needs identical positions from different shaders: invariant, and precise so no
        // compiler fuses or reorders the math feeding it (MSL gets this from preserveInvariance; without it foliage
        // drawn twice flickered black)
        sp(&s, "invariant gl_Position;\nprecise gl_Position;\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "layout(location = %d) out vec4 v%d;\n", i, i);
    } else if (stage == STAGE_FRAGMENT) {
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? "flat " : (c->noperspective_mask & (1u << i)) ? "noperspective " : "";
            sp(&s, "layout(location = %d) %sin vec4 v%d;\n", i, interp, i);
        }
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "layout(location = %d) out %s c%d;\n", rt, kind == 1 ? "ivec4" : kind == 2 ? "uvec4" : "vec4", rt);
        }
        if (c->omap_depth) info->writes_depth = true;
    } else {
        sp(&s, "layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;\n");
    }
    sp(&s, "void main() {\n");
    if (stage == STAGE_VERTEX) {
        sp(&s, "  gl_Position = vec4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  v%d = vec4(0, 0, 0, 1);\n", i);
        if (info->writes_point_size) sp(&s, "  gl_PointSize = 0.0f;\n");
    }
    // LARECOMP_VK_PRECISE=1 (research): `precise` registers - no contraction. Maxwell rounds FMUL and FADD separately
    // (FFMA, emitted as fma(), is the fused one); a compiler fusing a*b + c rounds once, an ulp apart. No game needed it
    // so far (Z-A's stripes were its stencil read as depth), and it costs the compiler freedom.
    static int noprec = -1;
    if (noprec < 0) noprec = getenv("LARECOMP_VK_PRECISE") == NULL;
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  %suint r%d = 0u;\n", noprec ? "" : "precise ", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0u; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d]; for (int li_ = 0; li_ < %d; li_++) lmem[li_] = 0u;\n", LMEM_WORDS, LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    if (any_branch && c->fwd) {  // (the blocks, the last one closed here: translate_impl)
        sp(&s, "%s  do {\n%s  } while (false);\n", c->fwd_decl.buf, body.buf ? body.buf : "");
    } else if (any_branch) {
        sp(&s, "  uint pc = 8u; uint crs[32]; uint crk[32]; uint crs_n = 0u;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0u; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", body.buf ? body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", body.buf ? body.buf : "");
    }
    free(body.buf);
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  gl_Position.y = -gl_Position.y;\n");
        if (key && key->depth_minus_one) sp(&s, "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5f;\n");
    } else if (stage == STAGE_FRAGMENT) {
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            const char *cv = kind == 1 ? "int" : kind == 2 ? "uint" : "";
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    if (comp == 3) sp(&s, "  c%d[%d] = %s(1.0f);\n", rt, comp, cv);
                    else sp(&s, "  c%d[%d] = %s(dbgv);\n", rt, comp, cv);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  c%d[%d] = as_f(r%d);\n", rt, comp, r);
                    else sp(&s, "  c%d[%d] = %s(r%d);\n", rt, comp, cv, r);
                } else {
                    sp(&s, "  c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { vec3 g = c0.rgb; float l = dot(g, vec3(0.299f, 0.587f, 0.114f));\n"
                   "    c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  gl_FragDepth = as_f(r%d);\n", r + 1);
            else sp(&s, "  gl_FragDepth = gl_FragCoord.z;\n");
        }
    }
    sp(&s, "}\n");
    info->msl = info->uses_gmem ? glsl_gmem_words(s.buf) : s.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c->fwd_decl.buf);
    free(c);
    return info;
}

// The instruction that last wrote register r before instruction i on the one path into it (no label in between,
// up to 24 instructions back): its word, and its index in *at; 0 when none is found that way. Any instruction whose
// destination field (bits 0-7) covers r counts as writing it (up to 4 registers: vector loads).
static uint64_t writer_before(const uint8_t *code, const uint8_t *label, size_t i, int r, size_t *at) {
    size_t after = i;  // the next instruction on the path
    for (size_t j = i, k = 0; j-- > 0 && k < 24;) {
        if (j % 4 == 0) continue;
        k++;
        if (label[after]) return 0;  // (another path joins there)
        after = j;
        uint64_t x = ((const uint64_t *)code)[j];
        int d = (int)bits(x, 0, 8);
        if (d == RZ || r < d || r > d + 3) continue;
        *at = j;
        return x;
    }
    return 0;
}

// The SSY / PBK / PCNT stack worked out ahead: its contents at every instruction, followed along each path from the
// entry (branches, BRX tables, SYNC / BRK / CONT). As on the hardware the three push onto one stack: SYNC pops an SSY
// entry, BRK unwinds to the nearest PBK entry and pops it, CONT unwinds to the nearest PCNT entry and keeps it. When
// every path into an instruction brings the same stack, a push emits nothing and SYNC / BRK / CONT jump to a constant
// pc (flow_to[i]): no per-invocation arrays (local memory) and no jumps the GPU compiler can't follow. false (paths
// disagree, the stack runs out or overflows, or a flow instruction this doesn't model: CAL / RET / JMP ...): the stacks
// stay at run time.
typedef struct { uint32_t n; uint8_t kind[32]; uint32_t v[32]; } FlowStack;  // kind: 0 SSY, 1 PBK, 2 PCNT (no padding: memcmp)
static bool flow_static(const uint64_t *code, size_t end, const size_t *brx, int nbrx, uint32_t *const *brx_to, const int *brx_nto,
                        uint32_t *flow_to) {
    FlowStack *st = calloc(end + 4, sizeof *st);
    uint8_t *seen = calloc(end + 4, 1);
    size_t *work = malloc((end + 4) * sizeof *work), nwork = 0;
    bool ok = st && seen && work;
    const char *why = "";
    size_t from_ = 0;  // (the log)
#define FLOW_GO(idx, stk) do { \
        size_t gi_ = (idx); \
        if (gi_ >= end || gi_ % 4 == 0) { ok = false; why = "target out of range"; break; } \
        if (!seen[gi_]) { seen[gi_] = 1; st[gi_] = (stk); work[nwork++] = gi_; } \
        else if (memcmp(&st[gi_], &(stk), sizeof(FlowStack))) { \
            ok = false, why = "paths disagree"; \
            if (getenv("LARECOMP_XLAT_FLOW_LOG")) { \
                fprintf(stderr, "[xlat]   at pc %zx from pc %zx:", gi_ * 8, from_ * 8); \
                for (uint32_t q_ = 0; q_ < st[gi_].n; q_++) fprintf(stderr, " %d:%x", st[gi_].kind[q_], st[gi_].v[q_]); \
                fprintf(stderr, " /"); \
                for (uint32_t q_ = 0; q_ < (stk).n; q_++) fprintf(stderr, " %d:%x", (stk).kind[q_], (stk).v[q_]); \
                fprintf(stderr, "\n"); \
            } \
        } \
    } while (0)
    if (ok) { FlowStack e; memset(&e, 0, sizeof e); FLOW_GO(1, e); }
    while (ok && nwork) {
        size_t i = work[--nwork];
        from_ = i;
        FlowStack s = st[i];
        uint64_t w = code[i], top12 = w & 0xfff0000000000000ull;
        uint32_t pc = (uint32_t)(i * 8);
        int pr = (int)bits(w, 16, 3);
        bool prn = bits(w, 19, 1) != 0;
        bool push = top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull;
        if (push || top12 == 0xe270000000000000ull) pr = PT, prn = false;  // (no predicate: emit_insn)
        bool always = pr == PT && !prn, never = pr == PT && prn;
        size_t next = i + 1 + ((i + 1) % 4 == 0);
        uint32_t target = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
        if (never) { FLOW_GO(next, s); continue; }
        if (top12 == 0xe300000000000000ull) {  // EXIT
            if (!always) FLOW_GO(next, s);
        } else if (top12 == 0xe240000000000000ull) {  // BRA
            if (w & 0x20) { ok = false; why = "BRA CC"; break; }  // (not modelled)
            FLOW_GO(target / 8, s);
            if (!always) FLOW_GO(next, s);
        } else if (push) {  // SSY / PBK / PCNT
            if ((w & 0x20) || s.n >= 32) { ok = false; why = "push"; break; }
            s.kind[s.n] = top12 == 0xe290000000000000ull ? 0 : top12 == 0xe2a0000000000000ull ? 1 : 2;
            s.v[s.n++] = target;
            FLOW_GO(next, s);
        } else if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || top12 == 0xe340000000000000ull ||
                   top12 == 0xe350000000000000ull) {  // SYNC / BRK / CONT
            int k = top12 == 0xe340000000000000ull ? 1 : top12 == 0xe350000000000000ull ? 2 : 0;
            int at = s.n - 1;
            if (k) while (at >= 0 && s.kind[at] != k) at--;
            if (at < 0 || s.kind[at] != k) { ok = false; why = "pop without its push"; break; }
            uint32_t to = s.v[at];
            if (flow_to[i] && flow_to[i] != to) { ok = false; why = "two targets"; break; }
            flow_to[i] = to;
            FlowStack t = s;
            int keep = k == 2 ? at + 1 : at;  // (CONT keeps its entry)
            while (t.n > keep) { t.n--; t.kind[t.n] = 0; t.v[t.n] = 0; }
            FLOW_GO(to / 8, t);
            if (!always) FLOW_GO(next, s);
        } else if (top12 == 0xe250000000000000ull) {  // BRX: its table's targets
            int b = 0;
            while (b < nbrx && brx[b] != i) b++;
            if (b == nbrx) { ok = false; why = "BRX"; break; }
            for (int j = 0; j < brx_nto[b] && ok; j++) FLOW_GO(brx_to[b][j] / 8, s);
            if (!always) FLOW_GO(next, s);
        } else if ((w >> 60) == 0xe && (((w >> 56) & 0xf) == 2 || ((w >> 56) & 0xf) == 3) && top12 != 0xe330000000000000ull) {
            ok = false, why = "flow instruction";  // (CAL, RET, JMP, PRET, PEXIT, ...)
            if (getenv("LARECOMP_XLAT_FLOW_LOG")) fprintf(stderr, "[xlat] flow op %016llx\n", (unsigned long long)w);
        } else {
            FLOW_GO(next, s);
        }
    }
#undef FLOW_GO
    static int flog = -1;  // (LARECOMP_XLAT_FLOW_LOG=1, research: why a program's stacks stay at run time)
    if (flog < 0) flog = getenv("LARECOMP_XLAT_FLOW_LOG") != NULL;
    if (flog && !ok) fprintf(stderr, "[xlat] flow stacks at run time: %s\n", why);
    free(st);
    free(seen);
    free(work);
    return ok;
}

// c->fwd: the loops that end at label i (or the end), the innermost first: again while their jump back set l<pc>, with
// the flags of the blocks inside cleared
static void fwd_loops_end(Ctx *c, const uint8_t *label, const size_t *lend, size_t i) {
    for (size_t h = i; h-- > 0;) {
        if (lend[h] != i) continue;
        sp(&c->body, "  if (!l%u) break;\n  l%u = false;", (uint32_t)(h * 8), (uint32_t)(h * 8));
        for (size_t j = h + 1; j < i; j++)
            if (label[j] && j % 4) sp(&c->body, " b%u = false;", (uint32_t)(j * 8));
        sp(&c->body, "\n  }\n  if (exit_now) break;\n");
    }
}

static ShaderInfo *translate_impl(const uint8_t *blob, size_t max_size, const uint8_t *consts, size_t nconsts, int stage,
                                  const ShaderKey *key) {
    ShaderInfo *info = calloc(1, sizeof *info);
    info->stage = stage;
    Ctx *c = calloc(1, sizeof *c);
    c->info = info;
    c->key = key;
    c->stage = stage;
    c->ipa_mode = -1;
    c->glsl = key && key->glsl;
    const uint8_t *sph = blob + 0x30;
    const uint8_t *code = blob + 0x80;
    // compute programs have neither the 0x30-byte NVN header nor an SPH: code starts at the blob
    if (stage == STAGE_COMPUTE) code = blob;
    // newer GLSLC (Legends Z-A): a compute program starts with a 0x100-byte header, magic 0x12345679, then the code
    if (stage == STAGE_COMPUTE && max_size > 0x100 && *(const uint32_t *)blob == 0x12345679u) code = blob + 0x100;
    c->code = code;
    if (stage == STAGE_FRAGMENT) {
        c->omap_target = *(const uint32_t *)(sph + 0x48);
        c->omap_depth = (*(const uint32_t *)(sph + 0x4C) >> 1) & 1;
    }
    // find the end: the last EXIT followed by self-branch padding, or max_size
    size_t n = (max_size - (code - blob)) / 8;
    if (n > 65536) n = 65536;
    size_t end = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t w = ((const uint64_t *)code)[i];
        if (i % 4 == 0) continue;
        if ((w & 0xfff0000000000000ull) == 0xe240000000000000ull && sbits(w, 20, 24) == -8 && end) break;  // bra self
        if (w == 0 && i > 4) break;
        end = i + 1;
    }
    // collect branch targets (labels)
    uint8_t *label = calloc(end + 4, 1);
    label[1] = 1;
    bool any_branch = false;
    size_t brx[64];
    int nbrx = 0;
    bool brx_any = false;
    uint32_t *brx_to[64] = {0};  // (each BRX's targets, for flow_static)
    int brx_nto[64] = {0};
    for (size_t i = 0; i < end; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = ((const uint64_t *)code)[i];
        uint32_t pc = (uint32_t)(i * 8);
        bool is_flow = (w & 0xfff0000000000020ull) == 0xe240000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe290000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2a0000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2b0000000000000ull;
        if (is_flow) {
            int64_t t = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
            any_branch = true;
        }
        if ((w & 0xfff0000000000000ull) == 0xe250000000000000ull) {  // BRX: its targets below
            any_branch = true;
            if (nbrx < 64) brx[nbrx++] = i;
            else brx_any = true;
        }
        if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || (w & 0xfff0000000000000ull) == 0xe340000000000000ull ||
            (w & 0xfff0000000000000ull) == 0xe350000000000000ull)
            any_branch = true;
        (void)is_branch_target_op;
    }
    // BRX (computed jump) targets. Without them every instruction needs a case label, which leaves the GPU's compiler
    // a basic block per instruction (most of Z-A's shaders: switch statements, jump tables). The usual source is a jump
    // table in the program's compiler constants (c1): "LDC Ra, c[0x1][Rx + off]" shortly before "BRX Ra, imm", on one
    // path (no label in between). Every word of c1 is then taken as a possible offset - the index can't leave c1 (and
    // reads past its end give 0) - so the targets are a superset of the real ones. Any other source: every instruction.
    for (int pass = 0; pass < 2 && !brx_any; pass++) {  // (pass 1: again, with every table's targets marked)
        for (int b = 0; b < nbrx && !brx_any; b++) {
            size_t i = brx[b], ldc = 0;
            int a = (int)bits(((const uint64_t *)code)[i], 8, 8);
            uint64_t x = 0;
            bool table = (x = writer_before(code, label, i, a, &ldc)) &&
                         (x & 0xfff8000000000000ull) == 0xef90000000000000ull && bits(x, 36, 5) == 1 && bits(x, 48, 3) == 4 &&
                         (int)bits(x, 0, 8) == a && bits(x, 16, 4) == PT;
            if (!table || !consts || !nconsts) { brx_any = true; break; }
            if (pass) continue;
            // the table's length: the index clamped first - "IMNMX y, y, N (min)", "SHL x, y, 2", then the LDC - N + 1
            // entries from its offset; else every word of c1 (more labels; the stacks then usually stay at run time)
            uint32_t koff = (uint32_t)bits(x, 20, 16), nent = 0;
            size_t at;
            int xr = (int)bits(x, 8, 8);
            uint64_t sh = writer_before(code, label, ldc, xr, &at), mn;
            if (sh && (sh & 0xfef8000000000000ull) == 0x3848000000000000ull && (sh >> 56) == 0x38 && !bits(sh, 39, 1) &&
                bits(sh, 20, 19) == 2 && (int)bits(sh, 0, 8) == xr &&
                (mn = writer_before(code, label, at, (int)bits(sh, 8, 8), &at)) && (mn >> 56) == 0x38 &&
                (mn & 0xfef8000000000000ull) == 0x3820000000000000ull && bits(mn, 39, 3) == PT && !bits(mn, 42, 1) &&
                (int)bits(mn, 0, 8) == (int)bits(sh, 8, 8) && !bits(mn, 56, 1) && bits(mn, 20, 19) < 1024)
                nent = (uint32_t)bits(mn, 20, 19) + 1;
            uint32_t base = (uint32_t)(i * 8 + 8) + (uint32_t)sbits(((const uint64_t *)code)[i], 20, 24);
            size_t o0 = nent ? koff : 0, o1 = nent ? koff + 4 * (size_t)nent : nconsts;
            for (size_t o = o0; o + 4 <= o1 && o + 4 <= nconsts && o < 0x10000; o += 4) {
                uint32_t v;
                memcpy(&v, consts + o, 4);
                uint32_t t = base + v;
                if (((t >> 3) & 3u) == 0u) t += 8u;
                if (!(t & 7u) && t / 8 < end) {
                    label[t / 8] = 1;
                    if (!(brx_nto[b] & 63)) brx_to[b] = realloc(brx_to[b], (size_t)(brx_nto[b] + 64) * sizeof *brx_to[b]);
                    brx_to[b][brx_nto[b]++] = t;
                } else if (nent) { brx_any = true; break; }  // (an exact table names only instructions)
            }
        }
    }
    if (brx_any) memset(label, 1, end);
    uint32_t *flow_to = NULL;
    static int flow_dyn = -1;  // (LARECOMP_XLAT_FLOW_DYN=1, research: the SSY / PBK / PCNT stacks at run time always)
    if (flow_dyn < 0) flow_dyn = getenv("LARECOMP_XLAT_FLOW_DYN") != NULL;
    if (any_branch && !brx_any && !flow_dyn) {
        flow_to = calloc(end + 4, sizeof *flow_to);
        if (!flow_static((const uint64_t *)code, end, brx, nbrx, brx_to, brx_nto, flow_to)) { free(flow_to); flow_to = NULL; }
    }
    // GLSL with the stacks worked out ahead: the program as its blocks in address order, each run when the one before
    // fell into it or a jump set its flag (b<pc>; a jump: the flag, then `break` out of its block's do-while), a jump back
    // making a loop around the blocks from its target through its own (a for loop that runs again while the jump back
    // set its flag l<pc>, the flags inside cleared for the next round), instead of the switch (pc) state machine in a
    // loop - which the GPU's compiler gets as one loop around everything (every block an iteration, nothing kept in a
    // register across a jump): Z-A, 86% of its programs. Loops that overlap without one inside the other (none seen so
    // far) keep the state machine. LARECOMP_XLAT_NO_FWD=1: the state machine always.
    static int no_fwd = -1;
    if (no_fwd < 0) no_fwd = getenv("LARECOMP_XLAT_NO_FWD") != NULL;
    c->fwd = c->glsl && flow_to && !no_fwd;
    size_t *lend = NULL;  // lend[h]: a loop starts at instruction h and ends before the label lend[h] (0: none)
    if (c->fwd) {
        lend = calloc(end + 4, sizeof *lend);
        size_t *last = calloc(end + 4, sizeof *last);  // (the last jump back to h)
        for (size_t i = 0; i < end; i++) {
            if (i % 4 == 0) continue;
            uint64_t w = ((const uint64_t *)code)[i], top12 = w & 0xfff0000000000000ull;
            uint32_t pc = (uint32_t)(i * 8);
            if (bits(w, 16, 3) == PT && bits(w, 19, 1) && top12 != 0xe290000000000000ull && top12 != 0xe2a0000000000000ull &&
                top12 != 0xe2b0000000000000ull && top12 != 0xe270000000000000ull) continue;  // (@!PT: never runs)
            uint32_t one, *to = NULL;
            int nto = 0;
            if ((w & 0xfff0000000000020ull) == 0xe240000000000000ull) one = norm_target((int64_t)pc + 8 + sbits(w, 20, 24)), to = &one, nto = 1;
            else if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || top12 == 0xe340000000000000ull ||
                     top12 == 0xe350000000000000ull) one = flow_to[i], to = &one, nto = one != 0;
            else if (top12 == 0xe250000000000000ull)
                for (int b = 0; b < nbrx; b++) if (brx[b] == i) to = brx_to[b], nto = brx_nto[b];
            for (int j = 0; j < nto; j++)
                if (to[j] <= pc && to[j] / 8 < end) last[to[j] / 8] = i > last[to[j] / 8] ? i : last[to[j] / 8], lend[to[j] / 8] = 1;
        }
        for (size_t h = 0; h < end; h++) {
            if (!lend[h]) continue;
            size_t e = last[h] + 1;
            while (e < end && !label[e]) e++;
            lend[h] = e;
        }
        for (size_t h1 = 0; h1 < end && c->fwd; h1++)  // (loops one inside the other, or apart)
            for (size_t h2 = h1 + 1; lend[h1] && h2 < lend[h1] && c->fwd; h2++)
                if (lend[h2] > lend[h1]) c->fwd = false;
        free(last);
    }
    if (c->fwd) {
        sp(&c->fwd_decl, "  bool exit_now = false;\n  bool b8 = true");
        for (size_t i = 2; i < end; i++)
            if (label[i] && i % 4) sp(&c->fwd_decl, i % 16 == 0 ? ",\n    b%u = false" : ", b%u = false", (uint32_t)(i * 8));
        sp(&c->fwd_decl, ";\n");
        for (size_t h = 0; h < end; h++)
            if (lend[h]) sp(&c->fwd_decl, "  bool l%u = false;\n", (uint32_t)(h * 8));
        c->brx_at = brx, c->brx_to = brx_to, c->brx_nto = brx_nto, c->nbrx = nbrx;
    }
    // translate
    bool dbg = maxwell_dbg_code && blob == maxwell_dbg_code && stage == STAGE_FRAGMENT;
    static int annotate = -1;
    if (annotate < 0) annotate = getenv("LARECOMP_SAVE_MSL") != NULL;
    c->branchy = any_branch;
    c->flow_to = flow_to;
    for (size_t i = 0; i < end && !c->fail; i++) {
        if (i % 4 == 0) continue;
        uint32_t pc = (uint32_t)(i * 8);
        if (any_branch && label[i] && c->fwd) {  // (the block before falls into this one: its flag)
            if (i > 1) sp(&c->body, "    b%u = true;\n  } while (false);\n  if (exit_now) break;\n", pc);
            fwd_loops_end(c, label, lend, i);
            if (lend[i]) sp(&c->body, "  for (uint g%u = 0u; g%u < 100000u; g%u++) {\n", pc, pc, pc);
            sp(&c->body, "  do { if (!b%u) break;\n", pc);
        } else if (any_branch && label[i]) sp(&c->body, "  case %uu:\n", pc);
        if (annotate) sp(&c->body, "    // pc %x: %016llx\n", pc, (unsigned long long)((const uint64_t *)code)[i]);
        emit_insn(c, ((const uint64_t *)code)[i], pc);
        if (dbg && pc == maxwell_dbg_pc) {
            int rr = maxwell_dbg_reg % 1000, half = maxwell_dbg_reg / 1000;  // +1000: low f16, +2000: high f16
            c->reg_used[rr] = 1;
            if (half) sp(&c->body, c->glsl ? "    dbgv = unpackHalf2x16(r%d)[%d];\n" : "    dbgv = float(as_type<half2>(r%d)[%d]);\n", rr, half - 1);
            else sp(&c->body, "    dbgv = as_type<float>(r%d);\n", rr);
        }
        info->num_insts++;
    }
    if (c->fwd) {
        sp(&c->body, "  } while (false);\n  if (exit_now) break;\n");
        fwd_loops_end(c, label, lend, end);
    }
    for (int b = 0; b < nbrx; b++) free(brx_to[b]);
    c->nbrx = 0;
    free(lend);
    free(label);
    free(flow_to);
    c->flow_to = NULL;
    if (c->fail) {
        // LARECOMP_DUMP_FAILED_SHADERS=1: the code to build/failed_<stage>_<n>.bin (nvdisasm -b SM50 -raw)
        static int ndump;
        if (getenv("LARECOMP_DUMP_FAILED_SHADERS") && ndump < 32) {
            char path[64];
            snprintf(path, sizeof path, "build/failed_%d_%02d.bin", stage, ndump++);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(code, 8, end, f); fclose(f); }
            snprintf(path, sizeof path, "build/failed_%d_%02d.blob", stage, ndump - 1);  // (and the blob's start)
            if ((f = fopen(path, "wb"))) { fwrite(blob, 1, max_size < 0x400 ? max_size : 0x400, f); fclose(f); }
        }
        free(c->body.buf);
        free(c->fwd_decl.buf);
        free(c);
        return info;
    }
    if (c->glsl) return glsl_finish(c, any_branch, dbg);

    // ---------------------------------------------------------- MSL source
    Str s = {0};
    sp(&s, "#include <metal_stdlib>\nusing namespace metal;\n");
    sp(&s, "static inline uint cbr(constant uint4 *cb, uint off) { return cb[off >> 4][(off >> 2) & 3]; }\n");
    sp(&s, "static inline uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return extract_bits(v, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(extract_bits(int(v), pos, min(len, 32u - pos))); }\n");
    sp(&s, "static inline uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return insert_bits(base, ins, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n");
    sp(&s, "static inline uint2 rsc(uint2 c, float4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uint2(floor((float2(c) + 0.5f) * s.xy)); }\n");
    sp(&s, "static inline float2 hx(uint v, uint swz) { if (swz == 1u) return float2(as_type<float>(v)); float2 h = float2(as_type<half2>(v)); return swz == 2u ? h.xx : swz == 3u ? h.yy : h; }\n");
    sp(&s, "static inline uint hpack(float2 r, uint merge, uint old) { if (merge == 1u) return as_type<uint>(r.x); uint lo = uint(as_type<ushort>(half(r.x))), hi = uint(as_type<ushort>(half(r.y))); if (merge == 2u) return (old & 0xFFFF0000u) | lo; if (merge == 3u) return (old & 0xFFFFu) | (hi << 16); return lo | (hi << 16); }\n");
    sp(&s, "static inline uint prmt(uint a, uint b, uint sel) { uint r = 0; for (uint i = 0; i < 4; i++) { uint s = (sel >> (i * 4)) & 7u; uint byte = s < 4 ? (a >> (s * 8)) & 0xFFu : (b >> ((s - 4) * 8)) & 0xFFu; r |= byte << (i * 8); } return r; }\n");

    // inputs/outputs
    if (stage == STAGE_VERTEX) {
        sp(&s, "struct VIn {\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "  %s4 a%d [[attribute(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", i, i);
        }
        sp(&s, "};\n");
        sp(&s, "struct VOut {\n  float4 pos [[position, invariant]];\n");  // depth pre-pass + EQUAL test needs identical positions
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  float4 v%d [[user(locn%d)]];\n", i, i);
        if (info->writes_point_size) sp(&s, "  float psize [[point_size]];\n");
        sp(&s, "};\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "struct FIn {\n  float4 pos [[position]];\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? " [[flat]]" : (c->noperspective_mask & (1u << i)) ? " [[center_no_perspective]]" : "";
            sp(&s, "  float4 v%d [[user(locn%d)]]%s;\n", i, i, interp);
        }
        sp(&s, "};\n");
        sp(&s, "struct FOut {\n");
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "  %s4 c%d [[color(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", rt, rt);
        }
        if (c->omap_depth) { sp(&s, "  float depth [[depth(any)]];\n"); info->writes_depth = true; }
        sp(&s, "};\n");
    }

    // entry point
    Str args = {0};
    for (int i = 0; i < 18; i++)
        if (info->cbuf_mask & (1u << i)) sp(&args, ", constant uint4 *cb%d [[buffer(%d)]]", i, i);
    if (info->uses_gmem) {
        if (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX)) {
            free(args.buf);
            args = (Str){0};  // (was a use after free + double free of args.buf)
            fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
        }
        sp(&args, ", device uchar *gmem [[buffer(%d)]]", GMEM_BUFFER_INDEX);
    }
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&args, ", constant float4 *rscale [[buffer(18)]]");
        info->uses_rscale = true;
    }
    // Runtime highlight (e.g. golden unchecked pots): fragment shaders writing a float color target 0 get a
    // per-draw tint, float4 [0] = colour.rgb + strength, [1] = added glow.rgb (strength 0 = untouched).
    bool tint = stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&args, ", constant float4 *ltint [[buffer(19)]]");
        info->uses_tint = true;
    }
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        const char *tn = tex_type_name(t->type);
        if (t->shadow) sp(&args, ", depth%s<float> t%d [[texture(%d)]], sampler s%d [[sampler(%d)]]", tn, i, i, i, i);
        else sp(&args, ", texture%s<float> t%d [[texture(%d)]], sampler s%d [[sampler(%d)]]", tn, i, i, i, i);
    }
    if (stage == STAGE_VERTEX) {
        sp(&s, "vertex VOut main0(VIn in [[stage_in]], uint vid [[vertex_id]], uint iid [[instance_id]]%s%s) {\n",
           (maxwell_dbg_vs && blob == maxwell_dbg_vs) ? ", device float4 *dbgout [[buffer(29)]]" : "",
           args.buf ? args.buf : "");
        sp(&s, "  VOut out = {};\n  out.pos = float4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  out.v%d = float4(0, 0, 0, 1);\n", i);
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "fragment FOut main0(FIn in [[stage_in]], bool front_facing [[front_facing]]%s%s) {\n",
           c->uses_lane ? ", uint lane [[thread_index_in_simdgroup]]" : "", args.buf ? args.buf : "");
    } else {
        sp(&s, "kernel void main0(uint3 tid [[thread_position_in_threadgroup]], uint3 ctaid [[threadgroup_position_in_grid]]%s) {\n",
           args.buf ? args.buf : "");
    }
    free(args.buf);
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  uint r%d = 0;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d] = {};\n", LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    if (any_branch) {
        sp(&s, "  uint pc = 8u; uint crs[32]; uint crk[32]; uint crs_n = 0u;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", c->body.buf ? c->body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", c->body.buf ? c->body.buf : "");
    }
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  out.pos.y = -out.pos.y;\n");
        if (key && key->depth_minus_one) sp(&s, "  out.pos.z = (out.pos.z + out.pos.w) * 0.5f;\n");
        if (maxwell_dbg_vs && blob == maxwell_dbg_vs) sp(&s, "  if (iid < 64u && vid < 8u) { dbgout[iid * 8u + vid] = out.pos; dbgout[512u + iid * 8u + vid] = %s; }\n", (c->out_attr_mask & 2u) ? "out.v1" : "float4(0)");
        sp(&s, "  return out;\n}\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "  FOut o;\n");
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    sp(&s, comp == 3 ? "  o.c%d[%d] = 1.0f;\n" : "  o.c%d[%d] = dbgv;\n", rt, comp);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  o.c%d[%d] = as_type<float>(r%d);\n", rt, comp, r);
                    else sp(&s, "  o.c%d[%d] = r%d;\n", rt, comp, r);
                } else {
                    sp(&s, "  o.c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { float3 g = o.c0.rgb; float l = dot(g, float3(0.299f, 0.587f, 0.114f));\n"
                   "    o.c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  o.depth = as_type<float>(r%d);\n", r + 1);
            else sp(&s, "  o.depth = in.pos.z;\n");
        }
        sp(&s, "  return o;\n}\n");
    } else {
        sp(&s, "  return;\n}\n");
    }
    info->msl = s.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c->fwd_decl.buf);
    free(c);
    return info;
}

// ---- Translation disk cache: cache/xlat/<hash>.bin, the translated program (GLSL / MSL and what it reads) by its
// code, header, key and stage, so a shader seen before costs a file read instead of a translation - with the SPIR-V
// and pipeline caches after it, a known shader no longer holds its draws back for a frame (the pop-in on known
// ground). A changed translator starts a new cache: its version is a hash of its source (Makefile XLAT_SRC_HASH; the build
// time where that isn't given), and each version has its folder, cache/xlat/<tag> (older ones are deleted, warmup.c).
// Off with LARECOMP_NO_XLAT_CACHE=1 and while a switch that changes the output is set.
#include <pthread.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#endif
#ifdef XLAT_SRC_HASH
static const char k_xlat_build[] = XLAT_SRC_HASH;
#else
static const char k_xlat_build[] = __DATE__ " " __TIME__;
#endif
const char *maxwell_build_id(void) { return k_xlat_build; }  // (runtime/nvn/vulkan/warmup.c: its marker)
const char *maxwell_cache_tag(void) {
    static char tag[17];
    if (!tag[0]) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (const char *p = k_xlat_build; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
        snprintf(tag, sizeof tag, "%016llx", (unsigned long long)h);
    }
    return tag;
}

static uint64_t xfnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// the bytes translate_impl reads: up to the end of the code as it finds it
static size_t program_len(const uint8_t *blob, size_t max_size, int stage) {
    size_t off = stage == STAGE_COMPUTE ? (max_size > 0x100 && *(const uint32_t *)blob == 0x12345679u ? 0x100 : 0) : 0x80;
    if (max_size <= off) return max_size;
    size_t n = (max_size - off) / 8, end = 0;
    if (n > 65536) n = 65536;
    for (size_t i = 0; i < n; i++) {
        uint64_t w = ((const uint64_t *)(blob + off))[i];
        if (i % 4 == 0) continue;
        if ((w & 0xfff0000000000000ull) == 0xe240000000000000ull && sbits(w, 20, 24) == -8 && end) break;
        if (w == 0 && i > 4) break;
        end = i + 1;
    }
    return off + end * 8;
}

ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    return maxwell_translate_consts(blob, max_size, NULL, 0, stage, key);
}

// What of the key a shader's translation reads, by its code (cache/xlat/<tag>/u<code hash>.bin, and in memory): the
// key describes what the draw has BOUND - integer images, depth-stencil textures, render target and vertex formats -
// and the translation reads only the slots the shader itself uses. Keyed on the full key, the warm-up's translations
// (made with an empty key) matched 1 fragment program in 192 in play: Z-A always has an integer image bound in image
// slot 1 and depth-stencil textures in texture slots 24 and 28, used or not. The cache files are named by the key
// with the unused parts cleared (key_used), so a translation serves every binding state that differs only there.
// LARECOMP_XLAT_VERIFY_MASK=1 (research): each cached lookup also translated with the full key, the two compared.
typedef struct { uint32_t tex, attr; uint8_t img, rt, all_tex, valid; } XUse;
#define XUSE_TAB 32768
static struct { uint64_t code; XUse use; } xuse_tab[XUSE_TAB];
static pthread_mutex_t xuse_lock = PTHREAD_MUTEX_INITIALIZER;

static XUse xuse_of(const ShaderInfo *info, int stage) {
    XUse u = {.valid = 1};
    for (int i = 0; i < info->ntex && i < 64; i++) {
        const ShaderTexture *t = &info->tex[i];
        int nslot = ((int)t->handle_offset - 0x20) / 8;
        if (t->bindless || nslot < 0 || nslot >= 32) u.all_tex = 1;
        else u.tex |= 1u << nslot;
    }
    for (int i = 0; i < info->nimg && i < 8; i++) if (info->img[i].slot < 8) u.img |= (uint8_t)(1u << info->img[i].slot);
    if (stage == STAGE_VERTEX) u.attr = info->attr_mask;
    if (stage == STAGE_FRAGMENT)
        for (int rt = 0; rt < 8; rt++) if ((info->out_mask >> (rt * 4)) & 0xF) u.rt |= (uint8_t)(1u << rt);
    return u;
}

static ShaderKey key_used(const ShaderKey *k, const XUse *u) {
    ShaderKey m = *k;
    if (!u->all_tex) m.tex_stencil &= u->tex;
    for (int i = 0; i < 8; i++) if (!((u->img >> i) & 1)) m.img_kind[i] = 0;
    for (int i = 0; i < 8; i++) if (!((u->rt >> i) & 1)) m.rt_kind[i] = 0;
    for (int i = 0; i < 32; i++) if (!((u->attr >> i) & 1)) m.attr_kind[i] = 0;
    return m;
}

static bool xuse_get(uint64_t code, XUse *u) {
    uint32_t sl = (uint32_t)(code ^ (code >> 31)) & (XUSE_TAB - 1);
    pthread_mutex_lock(&xuse_lock);
    for (int i = 0; i < 64; i++, sl = (sl + 1) & (XUSE_TAB - 1)) {
        if (!xuse_tab[sl].code) break;
        if (xuse_tab[sl].code == code) { *u = xuse_tab[sl].use; pthread_mutex_unlock(&xuse_lock); return true; }
    }
    pthread_mutex_unlock(&xuse_lock);
    char path[96];
    snprintf(path, sizeof path, "cache/xlat/%s/u%016llx.bin", maxwell_cache_tag(), (unsigned long long)code);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(u, sizeof *u, 1, f) == 1 && u->valid == 1;
    fclose(f);
    return ok;
}

static void xuse_put(uint64_t code, const XUse *u, bool file) {
    uint32_t sl = (uint32_t)(code ^ (code >> 31)) & (XUSE_TAB - 1);
    pthread_mutex_lock(&xuse_lock);
    for (int i = 0; i < 64; i++, sl = (sl + 1) & (XUSE_TAB - 1))
        if (!xuse_tab[sl].code || xuse_tab[sl].code == code) { xuse_tab[sl].code = code, xuse_tab[sl].use = *u; break; }
    pthread_mutex_unlock(&xuse_lock);
    if (!file) return;
    char path[96], tmp[128];
    snprintf(path, sizeof path, "cache/xlat/%s/u%016llx.bin", maxwell_cache_tag(), (unsigned long long)code);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)u);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(u, sizeof *u, 1, f) == 1;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);
}

ShaderInfo *maxwell_translate_consts(const uint8_t *blob, size_t max_size, const uint8_t *consts, size_t nconsts, int stage,
                                     const ShaderKey *key) {
    if (nconsts > 0x10000) nconsts = 0x10000;
    static int off = -1;
    if (off < 0)
        off = getenv("LARECOMP_NO_XLAT_CACHE") || getenv("LARECOMP_NO_DISCARD") || getenv("LARECOMP_VK_EXACT_H2F") ||
              getenv("LARECOMP_VK_PRECISE") || getenv("LARECOMP_SAVE_MSL") || getenv("LARECOMP_DUMP_FAILED_SHADERS") ||
              getenv("LARECOMP_XLAT_FLOW_DYN");
    if (off || maxwell_dbg_code || !key) return translate_impl(blob, max_size, consts, nconsts, stage, key);
    uint64_t code = xfnv(blob, program_len(blob, max_size, stage), 0xcbf29ce484222325ull ^ (uint64_t)stage);
    if (consts) code = xfnv(consts, nconsts, code ^ 0xc1);  // (BRX targets come from them)
    if (!code) code = 1;
    XUse use;
    bool known = xuse_get(code, &use);
    ShaderKey used = known ? key_used(key, &use) : *key;
    uint64_t h = xfnv(k_xlat_build, sizeof k_xlat_build, 0xcbf29ce484222325ull ^ (uint64_t)stage);
    h = xfnv(&used, sizeof used, h) ^ code;
    char path[96];
    snprintf(path, sizeof path, "cache/xlat/%s/%016llx.bin", maxwell_cache_tag(), (unsigned long long)h);
    FILE *f = known ? fopen(path, "rb") : NULL;
    if (f) {
        uint32_t hdr[2] = {0}, len = 0;
        ShaderInfo *info = malloc(sizeof *info);
        bool ok = fread(hdr, 4, 2, f) == 2 && hdr[0] == 0x31544c58 && hdr[1] == sizeof *info && fread(info, sizeof *info, 1, f) == 1 &&
                  fread(&len, 4, 1, f) == 1 && len < (16u << 20);
        if (ok && (info->msl = malloc(len + 1)) && fread(info->msl, 1, len, f) == len) {
            info->msl[len] = 0;
            fclose(f);
            static int verify = -1;
            if (verify < 0) verify = getenv("LARECOMP_XLAT_VERIFY_MASK") != NULL;
            if (verify) {
                static _Atomic int nv, nbad;
                ShaderInfo *full = translate_impl(blob, max_size, consts, nconsts, stage, key);
                bool same = full && full->msl && !strcmp(full->msl, info->msl);
                int v = ++nv, b = nbad += !same;
                if (!same || !(v & 255)) fprintf(stderr, "[xlat] verify: %d cached translations checked, %d differ%s\n", v, b, same ? "" : " (this one)");
                maxwell_free(full);
            }
            return info;
        }
        if (ok) free(info->msl);
        free(info);
        fclose(f);
    }
    ShaderInfo *info = translate_impl(blob, max_size, consts, nconsts, stage, key);
    if (info && info->msl && !known) {  // (what it read: the file below is named by the key's used parts)
        use = xuse_of(info, stage);
        used = key_used(key, &use);
        h = xfnv(k_xlat_build, sizeof k_xlat_build, 0xcbf29ce484222325ull ^ (uint64_t)stage);
        h = xfnv(&used, sizeof used, h) ^ code;
        snprintf(path, sizeof path, "cache/xlat/%s/%016llx.bin", maxwell_cache_tag(), (unsigned long long)h);
    }
    if (info && info->msl) {
        static int made;
        if (!made) {
            char dir[64];
            snprintf(dir, sizeof dir, "cache/xlat/%s", maxwell_cache_tag());
            made = 1, mkdir("cache", 0755), mkdir("cache/xlat", 0755), mkdir(dir, 0755);
        }
        char tmp[128];
        snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)info);
        if ((f = fopen(tmp, "wb"))) {
            uint32_t hdr[2] = {0x31544c58, sizeof *info}, len = (uint32_t)strlen(info->msl);
            ShaderInfo copy = *info;
            copy.msl = NULL;
            bool ok = fwrite(hdr, 4, 2, f) == 2 && fwrite(&copy, sizeof copy, 1, f) == 1 && fwrite(&len, 4, 1, f) == 1 &&
                      fwrite(info->msl, 1, len, f) == len;
            fclose(f);
            if (!ok || rename(tmp, path) != 0) remove(tmp);
        }
        if (!known) xuse_put(code, &use, true);
    }
    return info;
}

void maxwell_free(ShaderInfo *s) {
    if (!s) return;
    free(s->msl);
    free(s);
}

void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file) {
    (void)code; (void)max_size; (void)stage; (void)file;
}
