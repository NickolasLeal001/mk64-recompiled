// Differential test: every recompiled game function against the reference interpreter.
//
// For each function and several random initial states (argument registers pointing into a random
// scratch area, small integers or random words; random FPU arguments), the interpreter runs first.
// If it finishes cleanly (no IO, trap, loop budget), the recompiled function runs from the same
// state and all registers plus the full 8 MB of RDRAM must match.
//
// Usage: recomp_diff <rom.z64> [trials] [first_addr]
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "interp.h"
#include "replaced_stubs.c.inc"  // runtime-replaced libultra functions escape like IO does
#include "rom.h"

extern const struct {
    uint32_t addr;
    RecompFunc fn;
} recomp_func_table[];
extern const unsigned recomp_func_count;

#define LIBULTRA_START 0x800CBF70u
#define LIBULTRA_END 0x800D8F70u
#define SCRATCH 0x80300000u
#define SCRATCH_SIZE 0xE0000u
#define STACK_TOP 0x803F0000u

static jmp_buf g_escape;
static const char *g_escape_why;
static uint32_t g_escape_pc;

void recomp_unhandled(uint32_t pc, const char *what) {
    g_escape_pc = pc;
    g_escape_why = what;
    longjmp(g_escape, 1);
}
// Never return: recomp_unhandled jumps back to the test loop.
uint32_t recomp_io_read32(uint32_t addr) { recomp_unhandled(addr, "IO read"); return addr; }
void recomp_io_write32(uint32_t addr, uint32_t value) { (void)value; recomp_unhandled(addr, "IO write"); }
uint32_t recomp_cop0_read(Ctx *ctx, int reg) { (void)ctx; recomp_unhandled((uint32_t)reg, "COP0"); return (uint32_t)reg; }
void recomp_cop0_write(Ctx *ctx, int reg, uint32_t v) { (void)ctx; (void)v; recomp_unhandled((uint32_t)reg, "COP0"); }

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static void load_segment(uint8_t *rdram, const uint8_t *rom, uint32_t rom_off, uint32_t vaddr, uint32_t size) {
    for (uint32_t k = 0; k < size; k += 4) {
        const uint8_t *p = rom + rom_off + k;
        *(uint32_t *)(rdram + PHYS(vaddr + k)) = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
}

static uint64_t rand_arg(void) {
    switch (rnd() % 3) {
    case 0: return SX32(SCRATCH + (rnd() % (SCRATCH_SIZE / 2) & ~15u));
    case 1: return rnd() % 8;
    default: return SX32(rnd());
    }
}

static void random_state(Ctx *c, uint8_t *rdram) {
    memset(c, 0, sizeof(*c));
    for (int r = 4; r <= 7; r++) c->r[r] = rand_arg();
    for (int r = 16; r <= 23; r++) c->r[r] = rand_arg();
    c->r[29] = SX32(STACK_TOP);
    for (int f = 12; f <= 15; f++) {
        float v = (float)((int)(rnd() % 20001) - 10000) / 100.0f;
        memcpy(&c->f[f], &v, 4);
    }
    // Scratch: random words, a quarter of them pointers back into the scratch area.
    for (uint32_t k = 0; k < SCRATCH_SIZE; k += 4) {
        uint32_t v = (rnd() & 3) == 0 ? SCRATCH + (rnd() % SCRATCH_SIZE & ~3u) : rnd();
        *(uint32_t *)(rdram + PHYS(SCRATCH + k)) = v;
    }
}

#include "rewritten_list.c.inc"

// 0: generated, 1: rewritten returning a value, 2: rewritten returning nothing.
static int is_rewritten(uint32_t addr) {
    for (size_t k = 0; k < sizeof rewritten_addrs / sizeof rewritten_addrs[0]; k++)
        if (rewritten_addrs[k] == addr) return rewritten_void[k] ? 2 : 1;
    return 0;
}

// o32 ABI-visible state: result v0 (the rewrites return at most 32 bits, so v1 is scratch), f0/f2,
// callee-saved s0-s7/fp, f20-f31, sp and ra. A function returning nothing leaves v0 and f0-f3 as
// scratch too.
static int g_abi_only, g_abi_void;
static int abi_reg(int r) { return (r == 2 && !g_abi_void) || (r >= 16 && r <= 23) || r == 29 || r == 30 || r == 31; }
static int abi_freg(int f) { return (f <= 3 && !g_abi_void) || f >= 20; }

static int compare(const Ctx *a, const Ctx *b, const uint8_t *ma, const uint8_t *mb, char *why, size_t n) {
    for (int r = 1; r < 32; r++) {
        if (r == 26 || r == 27) continue;
        if (g_abi_only && !abi_reg(r)) continue;
        if (a->r[r] != b->r[r]) {
            snprintf(why, n, "r%d interp=%016llx recomp=%016llx", r, (unsigned long long)a->r[r], (unsigned long long)b->r[r]);
            return 0;
        }
    }
    if (!g_abi_only && (a->hi != b->hi || a->lo != b->lo)) { snprintf(why, n, "hi/lo"); return 0; }
    for (int f = 0; f < 32; f++) {
        if (g_abi_only && !abi_freg(f)) continue;
        if (a->f[f] != b->f[f]) {
            snprintf(why, n, "f%d interp=%08x recomp=%08x", f, a->f[f], b->f[f]);
            return 0;
        }
    }
    if (a->fcr31 != b->fcr31) { snprintf(why, n, "fcr31"); return 0; }
    if (memcmp(ma, mb, RDRAM_SIZE) != 0) {
        uint32_t k = 0;
        while (ma[k] == mb[k]) k++;
        k &= ~3u;
        unsigned count = 0;
        for (uint32_t j = 0; j < RDRAM_SIZE; j += 4) count += *(const uint32_t *)(ma + j) != *(const uint32_t *)(mb + j);
        snprintf(why, n, "RDRAM word 0x%08x: interp=%08x recomp=%08x (%u words differ)", 0x80000000u + k,
                 *(const uint32_t *)(ma + k), *(const uint32_t *)(mb + k), count);
        return 0;
    }
    return 1;
}

static void call_recompiled(uint8_t *rdram, Ctx *ctx, uint32_t target) {
    recomp_lookup(target)(rdram, ctx);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "uso: recomp_diff <rom.z64> [tentativas] [primeiro_endereco] [flags: h=hibrido o=so essa funcao]\n");
        return 2;
    }
    // Hybrid mode: the interpreter runs only each tested function's own body and calls the
    // recompiled callees, so a divergence points at that body rather than at something it calls.
    // Recompiled callees have no instruction budget, so use it together with "o" on one function.
    const char *flags = argc > 4 ? argv[4] : "";
    if (strchr(flags, 'h')) interp_call_hook = call_recompiled;
    int only = strchr(flags, 'o') != NULL;
    Rom rom;
    if (rom_load(argv[1], &rom) != ROM_OK) {
        fprintf(stderr, "ROM invalida\n");
        return 2;
    }
    int trials = argc > 2 ? atoi(argv[2]) : 6;
    uint32_t first = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 16) : 0;

    uint8_t *base = calloc(RDRAM_SIZE, 1), *ma = malloc(RDRAM_SIZE), *mb = malloc(RDRAM_SIZE);
    load_segment(base, rom.data, 0x1000, 0x80000400u, 0xF6510);
    load_segment(base, rom.data, 0xF7510, 0x8028DF00u, 0x2C130);
    load_segment(base, rom.data, 0x123640, 0x80280000u, 0x74A0);
    interp_code_image = base;

    unsigned tested = 0, passed = 0, failed = 0, skipped = 0, runs_ok = 0;
    unsigned skip_reason[5] = {0};
    for (unsigned k = 0; k < recomp_func_count; k++) {
        uint32_t addr = recomp_func_table[k].addr;
        if (addr < first || (only && addr != first) || (addr >= LIBULTRA_START && addr < LIBULTRA_END)) continue;
        int any = 0, bad = 0;
        char why[160] = "";
        g_abi_only = is_rewritten(addr) != 0;  // hand-written C: only the ABI contract must match
        g_abi_void = is_rewritten(addr) == 2;
        for (int t = 0; t < trials && !bad; t++) {
            Ctx c0, ca, cb;
            memcpy(ma, base, RDRAM_SIZE);
            random_state(&c0, ma);
            memcpy(mb, ma, RDRAM_SIZE);
            ca = c0;
            cb = c0;
            long budget = 2000000;
            InterpStatus st;
            if (setjmp(g_escape)) {
                st = INTERP_IO;  // a recompiled callee (hybrid mode) left RDRAM
            } else {
                st = interp_call(ma, &ca, addr, &budget);
            }
            if (st != INTERP_OK) { skip_reason[st]++; continue; }
            any = 1;
            cb.r[31] = SX32(INTERP_RET_MAGIC);  // same ra the interpreter started with
            if (setjmp(g_escape)) {
                // Calls into runtime-replaced libultra functions are not comparable: the
                // interpreter ran the original MIPS while the recompiled code calls the runtime.
                if (strstr(g_escape_why, "(runtime)")) { skip_reason[INTERP_UNSUPPORTED]++; continue; }
                snprintf(why, sizeof why, "recomp escaped: %s at 0x%08x", g_escape_why, g_escape_pc);
                bad = 1;
                break;
            }
            recomp_func_table[k].fn(mb, &cb);
            if (!compare(&ca, &cb, ma, mb, why, sizeof why)) bad = 1;
            else runs_ok++;
        }
        if (!any) { skipped++; continue; }
        tested++;
        if (bad) {
            failed++;
            if (failed <= 25) printf("DIVERGE 0x%08x: %s\n", addr, why);
        } else {
            passed++;
        }
    }
    printf("funcoes testadas: %u | iguais: %u | divergentes: %u | puladas: %u | execucoes comparadas: %u\n",
           tested, passed, failed, skipped, runs_ok);
    printf("tentativas puladas por motivo: budget=%u io=%u trap=%u nao-suportado=%u\n",
           skip_reason[INTERP_BUDGET], skip_reason[INTERP_IO], skip_reason[INTERP_TRAP], skip_reason[INTERP_UNSUPPORTED]);
    rom_free(&rom);
    return failed != 0;
}
