#ifndef INTERP_H
#define INTERP_H

#include "recomp.h"

typedef enum {
    INTERP_OK = 0,
    INTERP_BUDGET,       // instruction budget exhausted (loop or very long function)
    INTERP_IO,           // touched memory outside RDRAM
    INTERP_TRAP,         // break / trap taken
    INTERP_UNSUPPORTED,  // COP0, branch in delay slot, unknown encoding
} InterpStatus;

// Return address the interpreter plants in ra; the function is done when it jumps there.
#define INTERP_RET_MAGIC 0x80DEAD00u

// Optional: when set, calls (jal/jalr/bal) made by the interpreted code run through this hook
// instead of being interpreted (used to bisect divergences between interpreter and recompiler).
extern void (*interp_call_hook)(uint8_t *rdram, Ctx *ctx, uint32_t target);

// Optional: RDRAM image to fetch instructions from (instead of the live, writable RDRAM).
extern uint8_t *interp_code_image;

// Runs the function at `entry` until it returns. *budget is decremented per instruction.
InterpStatus interp_call(uint8_t *rdram, Ctx *ctx, uint32_t entry, long *budget);

#endif
