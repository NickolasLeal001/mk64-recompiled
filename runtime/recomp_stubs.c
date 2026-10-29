// Phase 3 placeholders for the hardware-facing runtime hooks. Phase 4 replaces this file with the
// real implementations (PI/SI/VI/AI/SP registers, COP0, libultra replacements).
#include <stdio.h>
#include <stdlib.h>

#include "recomp.h"

void recomp_unhandled(uint32_t pc, const char *what) {
    fprintf(stderr, "recomp: %s at 0x%08x\n", what, pc);
    abort();
}

uint32_t recomp_io_read32(uint32_t addr) {
    recomp_unhandled(addr, "IO read");
    return 0;
}

void recomp_io_write32(uint32_t addr, uint32_t value) {
    (void)value;
    recomp_unhandled(addr, "IO write");
}

uint32_t recomp_cop0_read(Ctx *ctx, int reg) {
    (void)ctx;
    recomp_unhandled((uint32_t)reg, "COP0 read");
    return 0;
}

void recomp_cop0_write(Ctx *ctx, int reg, uint32_t value) {
    (void)ctx;
    (void)value;
    recomp_unhandled((uint32_t)reg, "COP0 write");
}
