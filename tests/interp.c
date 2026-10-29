// Reference MIPS R4300 interpreter used only to check the recompiler.
//
// Control flow, delay slots and integer/FPU semantics are written here independently of the
// generator (tools/recomp/recomp.py); only the memory accessors and the rounding helper from
// recomp.h are shared. Runs until the function returns to RET_MAGIC, a budget runs out, or an
// unsupported situation (IO, traps, COP0) is hit.
#include "interp.h"

#include <math.h>
#include <string.h>

#define RET_MAGIC INTERP_RET_MAGIC

static InterpStatus g_status;
void (*interp_call_hook)(uint8_t *rdram, Ctx *ctx, uint32_t target);

static int64_t sx32(uint32_t v) { return (int64_t)(int32_t)v; }

static float getf(Ctx *c, int r) { float v; memcpy(&v, &c->f[r], 4); return v; }
// Arithmetic results: a NaN becomes the R4300 default NaN.
static void putf(Ctx *c, int r, float v) {
    if (isnan(v)) c->f[r] = 0x7FBFFFFFu;
    else memcpy(&c->f[r], &v, 4);
}
static double getd(Ctx *c, int r) {
    uint64_t u = (uint64_t)c->f[r] | ((uint64_t)c->f[r + 1] << 32);
    double v; memcpy(&v, &u, 8); return v;
}
static void putd(Ctx *c, int r, double v) {
    uint64_t u;
    if (isnan(v)) u = 0x7FF7FFFFFFFFFFFFull;
    else memcpy(&u, &v, 8);
    c->f[r] = (uint32_t)u; c->f[r + 1] = (uint32_t)(u >> 32);
}

static int rdram_ok(uint32_t a) { return (a & 0x1FFFFFFFu) < RDRAM_SIZE; }

static int cmp_cond(double a, double b, int cond) {
    int un = isnan(a) || isnan(b);
    return ((cond & 4) && !un && a < b) || ((cond & 2) && !un && a == b) || ((cond & 1) && un);
}

static void fpu_op(Ctx *c, uint32_t w) {
    int fmt = (w >> 21) & 31, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, fn = w & 63;
    int isd = fmt == 17;
    double a = fmt == 16 ? getf(c, fs) : fmt == 17 ? getd(c, fs) : fmt == 20 ? (double)(int32_t)c->f[fs]
             : (double)(int64_t)((uint64_t)c->f[fs] | ((uint64_t)c->f[fs + 1] << 32));
    double b = fmt == 16 ? getf(c, ft) : fmt == 17 ? getd(c, ft) : 0;
    if (fn >= 0x30) { uint32_t cond = 0;
        if (fmt == 16) cond = cmp_cond(getf(c, fs), getf(c, ft), fn & 7);
        else cond = cmp_cond(a, b, fn & 7);
        c->fcr31 = (c->fcr31 & ~(1u << 23)) | (cond << 23); return; }
    double r = 0;
    int to_int = -1, to_long = 0;
    switch (fn) {
    case 0: if (fmt == 16) { putf(c, fd, getf(c, fs) + getf(c, ft)); return; } r = a + b; break;
    case 1: if (fmt == 16) { putf(c, fd, getf(c, fs) - getf(c, ft)); return; } r = a - b; break;
    case 2: if (fmt == 16) { putf(c, fd, getf(c, fs) * getf(c, ft)); return; } r = a * b; break;
    case 3: if (fmt == 16) { putf(c, fd, getf(c, fs) / getf(c, ft)); return; } r = a / b; break;
    case 4: if (fmt == 16) { putf(c, fd, sqrtf(getf(c, fs))); return; } r = sqrt(a); break;
    // abs/neg only touch the sign bit, NaN or not.
    case 5: if (isd) c->f[fd + 1] = c->f[fs + 1] & 0x7FFFFFFFu, c->f[fd] = c->f[fs];
            else c->f[fd] = c->f[fs] & 0x7FFFFFFFu;
            return;
    case 6: c->f[fd] = c->f[fs]; if (isd) c->f[fd + 1] = c->f[fs + 1]; return;
    case 7: if (isd) c->f[fd + 1] = c->f[fs + 1] ^ 0x80000000u, c->f[fd] = c->f[fs];
            else c->f[fd] = c->f[fs] ^ 0x80000000u;
            return;
    case 8: case 9: case 10: case 11: to_long = 1; /* fallthrough */
    case 12: case 13: case 14: case 15: to_int = (int)((fn & 3) == 0 ? 0 : (fn & 3) == 1 ? 1 : (fn & 3) == 2 ? 2 : 3); break;
    case 0x20:
        if (fmt == 20) putf(c, fd, (float)(int32_t)c->f[fs]);
        else if (fmt == 21) putf(c, fd, (float)(int64_t)((uint64_t)c->f[fs] | ((uint64_t)c->f[fs + 1] << 32)));
        else putf(c, fd, (float)a);
        return;
    case 0x21: putd(c, fd, a); return;
    case 0x24: to_int = (int)(c->fcr31 & 3); break;
    case 0x25: to_int = (int)(c->fcr31 & 3); to_long = 1; break;
    default: g_status = INTERP_UNSUPPORTED; return;
    }
    if (to_int >= 0) {
        if (to_long) { uint64_t v = (uint64_t)fpu_to_l(a, to_int); c->f[fd] = (uint32_t)v; c->f[fd + 1] = (uint32_t)(v >> 32); }
        else c->f[fd] = (uint32_t)fpu_to_w(a, to_int);
        return;
    }
    putd(c, fd, r);
}

// Executes one non-branch instruction. Returns 0 if it was a branch/jump (handled by caller).
static int exec(uint8_t *m, Ctx *c, uint32_t w, uint32_t pc) {
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
    int64_t simm = (int16_t)(w & 0xFFFF);
    uint64_t zimm = w & 0xFFFF;
    uint64_t *R = c->r;
    uint64_t S = R[rs], T = R[rt];
    uint32_t ea = (uint32_t)S + (uint32_t)simm;
    uint64_t v = 0;
    int wr = -1;  // register written
    if (op >= 0x20 && op != 0x2F && !rdram_ok(ea)) { g_status = INTERP_IO; return 1; }
    switch (op) {
    case 0x00:
        switch (w & 63) {
        case 0x00: v = sx32((uint32_t)T << sa); wr = rd; break;
        case 0x02: v = sx32((uint32_t)T >> sa); wr = rd; break;
        case 0x03: v = sx32((uint32_t)((int32_t)(uint32_t)T >> sa)); wr = rd; break;
        case 0x04: v = sx32((uint32_t)T << (S & 31)); wr = rd; break;
        case 0x06: v = sx32((uint32_t)T >> (S & 31)); wr = rd; break;
        case 0x07: v = sx32((uint32_t)((int32_t)(uint32_t)T >> (S & 31))); wr = rd; break;
        case 0x08: case 0x09: return 0;
        case 0x0D: g_status = INTERP_TRAP; return 1;
        case 0x0F: return 1;
        case 0x10: v = c->hi; wr = rd; break;
        case 0x11: c->hi = S; return 1;
        case 0x12: v = c->lo; wr = rd; break;
        case 0x13: c->lo = S; return 1;
        case 0x14: v = T << (S & 63); wr = rd; break;
        case 0x16: v = T >> (S & 63); wr = rd; break;
        case 0x17: v = (uint64_t)((int64_t)T >> (S & 63)); wr = rd; break;
        case 0x18: { int64_t p = (int64_t)(int32_t)S * (int32_t)T; c->lo = sx32((uint32_t)p); c->hi = sx32((uint32_t)((uint64_t)p >> 32)); return 1; }
        case 0x19: { uint64_t p = (uint64_t)(uint32_t)S * (uint32_t)T; c->lo = sx32((uint32_t)p); c->hi = sx32((uint32_t)(p >> 32)); return 1; }
        case 0x1A: op_div(c, S, T); return 1;
        case 0x1B: op_divu(c, S, T); return 1;
        case 0x1C: op_dmult(c, S, T); return 1;
        case 0x1D: op_dmultu(c, S, T); return 1;
        case 0x1E: op_ddiv(c, S, T); return 1;
        case 0x1F: op_ddivu(c, S, T); return 1;
        case 0x20: case 0x21: v = sx32((uint32_t)S + (uint32_t)T); wr = rd; break;
        case 0x22: case 0x23: v = sx32((uint32_t)S - (uint32_t)T); wr = rd; break;
        case 0x24: v = S & T; wr = rd; break;
        case 0x25: v = S | T; wr = rd; break;
        case 0x26: v = S ^ T; wr = rd; break;
        case 0x27: v = ~(S | T); wr = rd; break;
        case 0x2A: v = (int64_t)S < (int64_t)T; wr = rd; break;
        case 0x2B: v = S < T; wr = rd; break;
        case 0x2C: case 0x2D: v = S + T; wr = rd; break;
        case 0x2E: case 0x2F: v = S - T; wr = rd; break;
        case 0x34: if (S == T) { g_status = INTERP_TRAP; } return 1;
        case 0x36: if (S != T) { g_status = INTERP_TRAP; } return 1;
        case 0x38: v = T << sa; wr = rd; break;
        case 0x3A: v = T >> sa; wr = rd; break;
        case 0x3B: v = (uint64_t)((int64_t)T >> sa); wr = rd; break;
        case 0x3C: v = T << (sa + 32); wr = rd; break;
        case 0x3E: v = T >> (sa + 32); wr = rd; break;
        case 0x3F: v = (uint64_t)((int64_t)T >> (sa + 32)); wr = rd; break;
        default: g_status = INTERP_UNSUPPORTED; return 1;
        }
        break;
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x14: case 0x15: case 0x16: case 0x17: return 0;
    case 0x08: case 0x09: v = sx32((uint32_t)S + (uint32_t)simm); wr = rt; break;
    case 0x0A: v = (int64_t)S < simm; wr = rt; break;
    case 0x0B: v = S < (uint64_t)simm; wr = rt; break;
    case 0x0C: v = S & zimm; wr = rt; break;
    case 0x0D: v = S | zimm; wr = rt; break;
    case 0x0E: v = S ^ zimm; wr = rt; break;
    case 0x0F: v = sx32((uint32_t)(zimm << 16)); wr = rt; break;
    case 0x10: g_status = INTERP_UNSUPPORTED; return 1;
    case 0x11:
        if (rs == 8) return 0;
        switch (rs) {
        case 0: v = sx32(c->f[rd]); wr = rt; break;
        case 1: v = (uint64_t)c->f[rd] | ((uint64_t)c->f[rd + 1] << 32); wr = rt; break;
        case 2: v = sx32(rd == 31 ? c->fcr31 : 0xA00); wr = rt; break;
        case 4: c->f[rd] = (uint32_t)T; return 1;
        case 5: c->f[rd] = (uint32_t)T; c->f[rd + 1] = (uint32_t)(T >> 32); return 1;
        case 6: if (rd == 31) c->fcr31 = (uint32_t)T; return 1;
        default: fpu_op(c, w); return 1;
        }
        break;
    case 0x18: case 0x19: v = S + (uint64_t)simm; wr = rt; break;
    case 0x20: v = (uint64_t)(int64_t)(int8_t)mem_r8(m, ea); wr = rt; break;
    case 0x21: v = (uint64_t)(int64_t)(int16_t)mem_r16(m, ea); wr = rt; break;
    case 0x22: v = op_lwl(m, ea, T); wr = rt; break;
    case 0x23: case 0x30: v = sx32(mem_r32(m, ea)); wr = rt; break;
    case 0x24: v = mem_r8(m, ea); wr = rt; break;
    case 0x25: v = mem_r16(m, ea); wr = rt; break;
    case 0x26: v = op_lwr(m, ea, T); wr = rt; break;
    case 0x27: v = mem_r32(m, ea); wr = rt; break;
    case 0x28: mem_w8(m, ea, (uint8_t)T); return 1;
    case 0x29: mem_w16(m, ea, (uint16_t)T); return 1;
    case 0x2A: op_swl(m, ea, T); return 1;
    case 0x2B: mem_w32(m, ea, (uint32_t)T); return 1;
    case 0x2E: op_swr(m, ea, T); return 1;
    case 0x2F: return 1;
    case 0x31: c->f[rt] = mem_r32(m, ea); return 1;
    case 0x35: { uint64_t d = mem_r64(m, ea); c->f[rt] = (uint32_t)d; c->f[rt + 1] = (uint32_t)(d >> 32); return 1; }
    case 0x37: v = mem_r64(m, ea); wr = rt; break;
    case 0x38: mem_w32(m, ea, (uint32_t)T); v = 1; wr = rt; break;
    case 0x39: mem_w32(m, ea, c->f[rt]); return 1;
    case 0x3D: mem_w64(m, ea, (uint64_t)c->f[rt] | ((uint64_t)c->f[rt + 1] << 32)); return 1;
    case 0x3F: mem_w64(m, ea, T); return 1;
    default: g_status = INTERP_UNSUPPORTED; return 1;
    }
    (void)pc;
    if (wr > 0) R[wr] = v;
    return 1;
}

// Branch decision for the instruction at pc; *likely set for branch-likely forms.
static int decide(Ctx *c, uint32_t w, uint32_t pc, uint32_t *target, int *likely, int *link, int *is_jump) {
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
    int64_t S = (int64_t)c->r[rs], T = (int64_t)c->r[rt];
    uint32_t bt = pc + 4 + (uint32_t)((int32_t)(int16_t)(w & 0xFFFF) << 2);
    *likely = 0; *link = 0; *is_jump = 0; *target = bt;
    switch (op) {
    case 0x00: *is_jump = 1; *target = (uint32_t)S; *link = (w & 63) == 9 ? (int)((w >> 11) & 31) : 0; return 1;
    case 0x02: case 0x03: *is_jump = 1; *target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFF) << 2);
        *link = op == 3 ? 31 : 0; return 1;
    case 0x01:
        *likely = (rt & 2) != 0;
        *link = (rt & 0x10) ? 31 : 0;
        return (rt & 1) ? S >= 0 : S < 0;
    case 0x04: case 0x14: *likely = op == 0x14; return S == T;
    case 0x05: case 0x15: *likely = op == 0x15; return S != T;
    case 0x06: case 0x16: *likely = op == 0x16; return S <= 0;
    case 0x07: case 0x17: *likely = op == 0x17; return S > 0;
    case 0x11: { int cond = (c->fcr31 >> 23) & 1; *likely = (rt & 2) != 0; return (rt & 1) ? cond : !cond; }
    }
    return 0;
}

// Instructions come from a pristine code image when one is given: recompiled code cannot see
// stores into code, so random test data overwriting code must not change what is executed.
uint8_t *interp_code_image;
static uint32_t fetch(uint8_t *m, uint32_t pc) { return mem_r32(interp_code_image ? interp_code_image : m, pc); }

InterpStatus interp_call(uint8_t *m, Ctx *c, uint32_t entry, long *budget) {
    g_status = INTERP_OK;
    c->r[31] = (uint64_t)(int64_t)(int32_t)RET_MAGIC;
    uint32_t pc = entry;
    while (pc != RET_MAGIC) {
        if (--*budget < 0) return INTERP_BUDGET;
        uint32_t w = fetch(m, pc);
        if (exec(m, c, w, pc)) {
            if (g_status != INTERP_OK) return g_status;
            c->r[0] = 0;
            pc += 4;
            continue;
        }
        uint32_t target; int likely, link, is_jump;
        int taken = decide(c, w, pc, &target, &likely, &link, &is_jump);
        uint32_t ds = fetch(m, pc + 4);
        if (link) c->r[link] = (uint64_t)(int64_t)(int32_t)(pc + 8);
        if (taken || !likely) {
            if (!exec(m, c, ds, pc + 4)) return INTERP_UNSUPPORTED;  // branch in delay slot
            if (g_status != INTERP_OK) return g_status;
            c->r[0] = 0;
        }
        if (taken && link && interp_call_hook) {
            interp_call_hook(m, c, target);
            pc += 8;
            continue;
        }
        pc = taken ? target : pc + 8;
    }
    return INTERP_OK;
}
