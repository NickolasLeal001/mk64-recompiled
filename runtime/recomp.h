// CPU state and instruction semantics shared by the recompiled code and the reference interpreter.
//
// RDRAM is 8 MB stored as host-endian 32-bit words: a big-endian word at N64 address A lives at
// rdram[A & ~3] as a native uint32_t, so word accesses are direct and byte/half accesses flip the
// low address bits (XOR 3 / XOR 2).
#ifndef RECOMP_H
#define RECOMP_H

#include <stdint.h>
#include <string.h>

#define RDRAM_SIZE 0x800000u

typedef struct Ctx {
    uint64_t r[32];
    uint64_t hi, lo;
    uint32_t f[32];   // FR=0 (o32): doubles live in even/odd register pairs
    uint32_t fcr31;   // bit 23 = condition, bits 0-1 = rounding mode
} Ctx;

typedef void (*RecompFunc)(uint8_t *rdram, Ctx *ctx);

// Provided by the runtime (or the test harness).
uint32_t recomp_io_read32(uint32_t addr);
void recomp_io_write32(uint32_t addr, uint32_t value);
RecompFunc recomp_lookup(uint32_t addr);          // jalr / function pointers
void recomp_unhandled(uint32_t pc, const char *what);
uint32_t recomp_cop0_read(Ctx *ctx, int reg);
void recomp_cop0_write(Ctx *ctx, int reg, uint32_t value);

// --- addresses --------------------------------------------------------------------------------
#define PHYS(a) ((uint32_t)(a) & 0x1FFFFFFFu)
#define IS_RDRAM(a) (PHYS(a) < RDRAM_SIZE)

static inline uint32_t mem_r32(uint8_t *rdram, uint32_t a) {
    if (IS_RDRAM(a)) return *(uint32_t *)(rdram + PHYS(a));
    return recomp_io_read32(a);
}
static inline void mem_w32(uint8_t *rdram, uint32_t a, uint32_t v) {
    if (IS_RDRAM(a)) *(uint32_t *)(rdram + PHYS(a)) = v;
    else recomp_io_write32(a, v);
}
static inline uint16_t mem_r16(uint8_t *rdram, uint32_t a) {
    if (IS_RDRAM(a)) return *(uint16_t *)(rdram + (PHYS(a) ^ 2));
    return (uint16_t)(recomp_io_read32(a & ~3u) >> ((a & 2) ? 0 : 16));
}
static inline void mem_w16(uint8_t *rdram, uint32_t a, uint16_t v) {
    if (IS_RDRAM(a)) *(uint16_t *)(rdram + (PHYS(a) ^ 2)) = v;
    else recomp_unhandled(a, "16-bit IO write");
}
static inline uint8_t mem_r8(uint8_t *rdram, uint32_t a) {
    if (IS_RDRAM(a)) return rdram[PHYS(a) ^ 3];
    return (uint8_t)(recomp_io_read32(a & ~3u) >> (8 * (3 - (a & 3))));
}
static inline void mem_w8(uint8_t *rdram, uint32_t a, uint8_t v) {
    if (IS_RDRAM(a)) rdram[PHYS(a) ^ 3] = v;
    else recomp_unhandled(a, "8-bit IO write");
}
static inline uint64_t mem_r64(uint8_t *rdram, uint32_t a) {
    return ((uint64_t)mem_r32(rdram, a) << 32) | mem_r32(rdram, a + 4);
}
static inline void mem_w64(uint8_t *rdram, uint32_t a, uint64_t v) {
    mem_w32(rdram, a, (uint32_t)(v >> 32));
    mem_w32(rdram, a + 4, (uint32_t)v);
}

// Unaligned word access (big-endian semantics of LWL/LWR/SWL/SWR).
static inline uint64_t op_lwl(uint8_t *rdram, uint32_t a, uint64_t rt) {
    uint32_t sh = 8 * (a & 3);
    uint32_t w = mem_r32(rdram, a & ~3u);
    uint32_t keep = sh ? ((uint32_t)rt & ((1u << sh) - 1)) : 0;
    return (uint64_t)(int64_t)(int32_t)((w << sh) | keep);
}
static inline uint64_t op_lwr(uint8_t *rdram, uint32_t a, uint64_t rt) {
    uint32_t sh = 8 * (3 - (a & 3));
    uint32_t w = mem_r32(rdram, a & ~3u);
    uint32_t keep = sh ? ((uint32_t)rt & ~(0xFFFFFFFFu >> sh)) : 0;
    return (uint64_t)(int64_t)(int32_t)((w >> sh) | keep);
}
static inline void op_swl(uint8_t *rdram, uint32_t a, uint64_t rt) {
    uint32_t sh = 8 * (a & 3);
    uint32_t w = mem_r32(rdram, a & ~3u);
    uint32_t mask = 0xFFFFFFFFu >> sh;
    mem_w32(rdram, a & ~3u, (w & ~mask) | (((uint32_t)rt >> sh) & mask));
}
static inline void op_swr(uint8_t *rdram, uint32_t a, uint64_t rt) {
    uint32_t sh = 8 * (3 - (a & 3));
    uint32_t w = mem_r32(rdram, a & ~3u);
    uint32_t mask = 0xFFFFFFFFu << sh;
    mem_w32(rdram, a & ~3u, (w & ~mask) | (((uint32_t)rt << sh) & mask));
}

// --- integer helpers ----------------------------------------------------------------------------
// Function entry hook of the generated code: a call count per function when built with
// RECOMP_PROFILE (MK64_PROFILE=a,b then logs the functions called in retraces [a, b)).
#ifdef RECOMP_PROFILE
void recomp_enter(uint32_t addr);
#define RECOMP_ENTER(a) recomp_enter(a)
#else
#define RECOMP_ENTER(a) ((void)0)
#endif

#define SX32(v) ((uint64_t)(int64_t)(int32_t)(uint32_t)(v))

static inline void op_mult(Ctx *c, uint64_t a, uint64_t b) {
    int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    c->lo = SX32(p);
    c->hi = SX32((uint64_t)p >> 32);
}
static inline void op_multu(Ctx *c, uint64_t a, uint64_t b) {
    uint64_t p = (uint64_t)(uint32_t)a * (uint32_t)b;
    c->lo = SX32(p);
    c->hi = SX32(p >> 32);
}
static inline void op_div(Ctx *c, uint64_t a, uint64_t b) {
    int32_t n = (int32_t)a, d = (int32_t)b;
    if (d == 0) {  // R4300 result for division by zero
        c->lo = n >= 0 ? (uint64_t)-1 : 1;
        c->hi = SX32(n);
    } else if (n == INT32_MIN && d == -1) {
        c->lo = SX32(n);
        c->hi = 0;
    } else {
        c->lo = SX32(n / d);
        c->hi = SX32(n % d);
    }
}
static inline void op_divu(Ctx *c, uint64_t a, uint64_t b) {
    uint32_t n = (uint32_t)a, d = (uint32_t)b;
    if (d == 0) {
        c->lo = (uint64_t)-1;
        c->hi = SX32(n);
    } else {
        c->lo = SX32(n / d);
        c->hi = SX32(n % d);
    }
}
void op_dmult(Ctx *c, uint64_t a, uint64_t b);
void op_dmultu(Ctx *c, uint64_t a, uint64_t b);
void op_ddiv(Ctx *c, uint64_t a, uint64_t b);
void op_ddivu(Ctx *c, uint64_t a, uint64_t b);

// --- FPU (FR=0) ----------------------------------------------------------------------------------
static inline float fs_get(Ctx *c, int r) { float v; memcpy(&v, &c->f[r], 4); return v; }
static inline void fs_set(Ctx *c, int r, float v) { memcpy(&c->f[r], &v, 4); }
static inline double fd_get(Ctx *c, int r) {
    uint64_t u = ((uint64_t)c->f[r + 1] << 32) | c->f[r];
    double v; memcpy(&v, &u, 8); return v;
}
static inline void fd_set(Ctx *c, int r, double v) {
    uint64_t u; memcpy(&u, &v, 8);
    c->f[r] = (uint32_t)u;
    c->f[r + 1] = (uint32_t)(u >> 32);
}
static inline uint64_t fl_get(Ctx *c, int r) { return ((uint64_t)c->f[r + 1] << 32) | c->f[r]; }
static inline void fl_set(Ctx *c, int r, uint64_t u) { c->f[r] = (uint32_t)u; c->f[r + 1] = (uint32_t)(u >> 32); }
// An invalid FPU operation on the R4300 yields its default quiet NaN (legacy MIPS encoding, quiet
// bit clear), not the host's 0xffc00000 / 0xfff8000000000000.
static inline float fnan_s(float v) {
    if (v != v) { uint32_t u = 0x7FBFFFFFu; memcpy(&v, &u, 4); }
    return v;
}
static inline double fnan_d(double v) {
    if (v != v) { uint64_t u = 0x7FF7FFFFFFFFFFFFull; memcpy(&v, &u, 8); }
    return v;
}

#define FCOND(c) (((c)->fcr31 >> 23) & 1)
static inline void fcond_set(Ctx *c, int v) { c->fcr31 = (c->fcr31 & ~(1u << 23)) | ((uint32_t)(v != 0) << 23); }

// Conversions to integer honour the rounding mode in fcr31 (cvt.*) or a fixed one (round/trunc/...).
int32_t fpu_to_w(double v, int mode);   // mode: 0 nearest, 1 zero, 2 +inf, 3 -inf
int64_t fpu_to_l(double v, int mode);
#define RM_CUR(c) ((int)((c)->fcr31 & 3))

#endif
