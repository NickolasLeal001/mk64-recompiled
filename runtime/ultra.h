// Native replacement for the libultra OS layer used by the recompiled game.
#ifndef ULTRA_H
#define ULTRA_H

#include <stddef.h>
#include <stdint.h>

#include "recomp.h"

// libultra event numbers (os.h), delivered through __osEventStateTab.
enum {
    OS_EVENT_SP = 4,
    OS_EVENT_SI = 5,
    OS_EVENT_AI = 6,
    OS_EVENT_VI = 7,
    OS_EVENT_PI = 8,
    OS_EVENT_DP = 9,
};

typedef struct UltraHost {
    // Called once per vertical retrace (59.94 Hz) from the scheduler, outside any game thread.
    // Returns 0 to quit.
    int (*on_retrace)(void *user);
    // Called when the game starts an RSP task; `task` is the OSTask address in RDRAM.
    void (*on_task)(void *user, uint32_t task);
    // Audio interface: a buffer of big-endian stereo s16 in RDRAM, and bytes still queued.
    void (*on_ai_buffer)(void *user, uint32_t addr, uint32_t size, uint32_t frequency);
    uint32_t (*ai_queued)(void *user);
    void *user;
} UltraHost;

typedef struct UltraStats {
    uint64_t retraces;
    uint64_t gfx_tasks, audio_tasks, other_tasks;
    uint64_t dma_bytes, dma_count;
    uint32_t framebuffer;      // last osViSwapBuffer address
    uint32_t ai_frequency;
    int threads, threads_ready;
} UltraStats;

extern int ultra_deterministic;  // set before ultra_run: virtual time, retraces when idle
extern uint8_t *ultra_rdram;
extern const uint8_t *ultra_rom;
extern size_t ultra_rom_size;
extern UltraStats ultra_stats;

// Loads the boot segment like IPL3, runs the game's entry point and never returns.
void ultra_run(uint8_t *rdram, const uint8_t *rom, size_t rom_size, const UltraHost *host);

// Posts a libultra event (message from __osEventStateTab) at the next scheduling point.
void ultra_post_event(int event);

// Controller state for osContGetReadData, filled by the host (runtime/input.c).
typedef struct UltraPad {
    int connected;
    uint16_t buttons;
    int8_t stick_x, stick_y;
} UltraPad;
extern UltraPad ultra_pads[4];

void ultra_log(const char *fmt, ...);

#endif
