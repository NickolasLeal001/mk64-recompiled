// Accesses the recompiled code makes outside RDRAM. The OS functions that drive the hardware are
// replaced natively, so anything reaching here is logged once per address and answered with 0.
#include <stdio.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ultra.h"

#define MAX_SEEN 256
static uint32_t g_seen[MAX_SEEN];
static int g_seen_count;

static void note(uint32_t addr, const char *what) {
    for (int k = 0; k < g_seen_count; k++)
        if (g_seen[k] == addr) return;
    if (g_seen_count < MAX_SEEN) g_seen[g_seen_count++] = addr;
    ultra_log("IO %s 0x%08x (unhandled, ignored)", what, addr);
}

uint32_t recomp_io_read32(uint32_t addr) {
    note(addr, "read");
    return 0;
}

void recomp_io_write32(uint32_t addr, uint32_t value) {
    (void)value;
    note(addr, "write");
}

void recomp_unhandled(uint32_t pc, const char *what) {
    char msg[256];
    snprintf(msg, sizeof msg, "Erro no codigo recompilado: %s em 0x%08x", what, pc);
    ultra_log("FATAL: %s", msg);
    fprintf(stderr, "%s\n", msg);
    MessageBoxA(NULL, msg, "MK64 Port", MB_ICONERROR);
    exit(1);
}
