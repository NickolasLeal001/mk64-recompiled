// Checks the behaviour documented in mk64-analise/NOTAS.md against the recompiled game code.
// Each check calls the real (recompiled) function with RDRAM loaded from the user's ROM.
//
// Usage: behavior_test <rom.z64>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "replaced_stubs.c.inc"
#include "rom.h"

void recomp_unhandled(uint32_t pc, const char *what) {
    fprintf(stderr, "unhandled: %s at 0x%08x\n", what, pc);
    exit(2);
}
uint32_t recomp_io_read32(uint32_t addr) { recomp_unhandled(addr, "IO read"); return 0; }
void recomp_io_write32(uint32_t addr, uint32_t v) { (void)v; recomp_unhandled(addr, "IO write"); }
uint32_t recomp_cop0_read(Ctx *c, int r) { (void)c; return (uint32_t)r; }
void recomp_cop0_write(Ctx *c, int r, uint32_t v) { (void)c; (void)r; (void)v; }

static uint8_t *ram;
static int failures;

static void check(int cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    failures += !cond;
}

static void load(const Rom *rom, uint32_t off, uint32_t vaddr, uint32_t size) {
    for (uint32_t k = 0; k < size; k++) mem_w8(ram, vaddr + k, rom->data[off + k]);
}

// FUN_80038534 / FUN_800388b0 take a controller struct (s16 x at +0, s16 y at +2).
static int stick(uint32_t fn_addr, int x, int y) {
    Ctx c;
    memset(&c, 0, sizeof c);
    mem_w16(ram, 0x80300000u, (uint16_t)x);
    mem_w16(ram, 0x80300002u, (uint16_t)y);
    c.r[4] = SX32(0x80300000u);
    c.r[29] = SX32(0x803F0000u);
    recomp_lookup(fn_addr)(ram, &c);
    return (int)(int32_t)c.r[2];
}

int main(int argc, char **argv) {
    Rom rom;
    if (argc < 2 || rom_load(argv[1], &rom) != ROM_OK) {
        fprintf(stderr, "uso: behavior_test <rom.z64>\n");
        return 2;
    }
    ram = calloc(RDRAM_SIZE, 1);
    load(&rom, 0x1000, 0x80000400u, 0xF6510);
    load(&rom, 0xF7510, 0x8028DF00u, 0x2C130);

    // Stick: dead zone of +-12 per axis, then clamped to +-53 (NOTAS.md, "Leitura do analogico").
    int dead = 1;
    for (int v = -12; v <= 12; v++) dead &= stick(0x800388B0u, 0, v) == 0 && stick(0x80038534u, v, 0) == 0;
    check(dead, "analogico: |valor| <= 12 vira 0 (zona morta), nos dois eixos");
    check(stick(0x800388B0u, 0, 80) == 53 && stick(0x800388B0u, 0, -80) == -53, "analogico Y: deflexao total limitada a +-53");
    check(stick(0x80038534u, 80, 0) == 53 && stick(0x80038534u, -80, 0) == -53, "analogico X: deflexao total limitada a +-53");
    check(stick(0x800388B0u, 0, 40) == 28, "analogico Y: 40 -> 28 (desconta a zona morta de 12)");

    // C-down sets Z (FUN_80000934 reads the raw pad at 0x8014F0F0, 6 bytes per controller).
    {
        Ctx c;
        memset(&c, 0, sizeof c);
        mem_w16(ram, 0x801625EAu, 0);           // not paused/ignored
        mem_w16(ram, 0x8014F0F0u, 0x0004);      // raw buttons: C-down only
        mem_w16(ram, 0x800F6914u, 0);
        c.r[4] = 0;
        c.r[29] = SX32(0x803F0000u);
        recomp_lookup(0x80000934u)(ram, &c);
        uint16_t held = mem_r16(ram, 0x800F6914u);
        check((held & 0x2000) && (held & 0x0004), "C-baixo liga tambem o bit do Z (0x2000)");
    }

    // Acceleration table per character class (0x800E3010 -> 8 pointers to 10 floats).
    {
        float row[8][10];
        for (int ch = 0; ch < 8; ch++) {
            uint32_t p = mem_r32(ram, 0x800E3010u + 4 * ch);
            for (int k = 0; k < 10; k++) {
                uint32_t bits = mem_r32(ram, p + 4 * k);
                memcpy(&row[ch][k], &bits, 4);
            }
        }
        int light = row[2][3] == 2.6f && row[3][3] == 2.6f && row[6][3] == 2.6f;
        int heavy = row[4][7] == 1.8f && row[5][7] == 1.8f && row[7][7] == 1.8f;
        int medium = row[0][9] == 0.4f && row[1][9] == 0.4f;
        check(light && heavy && medium, "aceleracao: leves 2.6 a 30-40%, pesados 1.8 a 70-80%, medios 0.4 no fim");
    }

    printf(failures ? "%d falha(s)\n" : "tudo ok\n", failures);
    rom_free(&rom);
    return failures != 0;
}
