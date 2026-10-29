#ifndef ROM_H
#define ROM_H

#include <stddef.h>
#include <stdint.h>

// The only ROM this port accepts: Mario Kart 64 (US, NKTE), big-endian .z64 order.
#define MK64_US_SIZE 0xC00000
#define MK64_US_SHA1 "579c48e211ae952530ffc8738709f078d5dd215e"

typedef enum {
    ROM_OK = 0,
    ROM_ERR_OPEN,
    ROM_ERR_SIZE,
    ROM_ERR_FORMAT,
    ROM_ERR_HASH,
} RomStatus;

typedef enum {
    ROM_ORDER_Z64, // big-endian, native N64 order
    ROM_ORDER_V64, // byte-swapped 16-bit
    ROM_ORDER_N64, // little-endian 32-bit
} RomOrder;

typedef struct {
    uint8_t *data;   // always converted to .z64 order
    size_t size;
    RomOrder source_order;
    char sha1[41];   // of the .z64-ordered data
    char title[21];  // header 0x20, trimmed
    char game_code[5]; // header 0x3B, e.g. "NKTE"
} Rom;

RomStatus rom_load(const char *path, Rom *rom);
void rom_free(Rom *rom);
const char *rom_status_message(RomStatus status);

#endif
