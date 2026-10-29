#ifndef ABI_H
#define ABI_H

#include <stddef.h>
#include <stdint.h>

// Reads the resampling filter (64 x 4 s16) from the audio microcode data in the user's ROM.
// Returns 0 if the table does not look right (resampling then falls back to linear).
int abi_init(const uint8_t *rom, size_t rom_size, uint32_t lut_offset);
// Runs an audio task's command list (OSTask data_ptr / data_size).
void abi_run(uint8_t *rdram, uint32_t list, uint32_t size);
extern uint64_t abi_stats_unknown;
extern uint32_t abi_unknown_ops;  // bit per unhandled opcode (0x00-0x1F)

#endif
