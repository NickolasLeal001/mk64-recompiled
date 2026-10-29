#include "rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha1.h"

// First word of every retail N64 ROM header, in each dump byte order.
static int detect_order(const uint8_t *p, RomOrder *order) {
    if (p[0] == 0x80 && p[1] == 0x37 && p[2] == 0x12 && p[3] == 0x40) {
        *order = ROM_ORDER_Z64;
    } else if (p[0] == 0x37 && p[1] == 0x80 && p[2] == 0x40 && p[3] == 0x12) {
        *order = ROM_ORDER_V64;
    } else if (p[0] == 0x40 && p[1] == 0x12 && p[2] == 0x37 && p[3] == 0x80) {
        *order = ROM_ORDER_N64;
    } else {
        return 0;
    }
    return 1;
}

static void to_z64(uint8_t *p, size_t size, RomOrder order) {
    for (size_t i = 0; i + 3 < size; i += 4) {
        uint8_t a = p[i], b = p[i + 1], c = p[i + 2], d = p[i + 3];
        if (order == ROM_ORDER_V64) {
            p[i] = b, p[i + 1] = a, p[i + 2] = d, p[i + 3] = c;
        } else if (order == ROM_ORDER_N64) {
            p[i] = d, p[i + 1] = c, p[i + 2] = b, p[i + 3] = a;
        }
    }
}

static void copy_trimmed(char *dst, const uint8_t *src, size_t n) {
    memcpy(dst, src, n);
    dst[n] = '\0';
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\0')) {
        dst[--n] = '\0';
    }
}

RomStatus rom_load(const char *path, Rom *rom) {
    memset(rom, 0, sizeof(*rom));
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ROM_ERR_OPEN;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size != MK64_US_SIZE) {
        fclose(f);
        return ROM_ERR_SIZE;
    }
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return ROM_ERR_OPEN;
    }
    fclose(f);

    RomOrder order;
    if (!detect_order(data, &order)) {
        free(data);
        return ROM_ERR_FORMAT;
    }
    to_z64(data, (size_t)size, order);

    uint8_t digest[20];
    sha1(data, (size_t)size, digest);
    sha1_hex(digest, rom->sha1);

    rom->data = data;
    rom->size = (size_t)size;
    rom->source_order = order;
    copy_trimmed(rom->title, data + 0x20, 20);
    copy_trimmed(rom->game_code, data + 0x3B, 4);

    if (strcmp(rom->sha1, MK64_US_SHA1) != 0) {
        return ROM_ERR_HASH; // caller can still report what was loaded, then rom_free
    }
    return ROM_OK;
}

void rom_free(Rom *rom) {
    free(rom->data);
    rom->data = NULL;
}

const char *rom_status_message(RomStatus status) {
    switch (status) {
    case ROM_OK:
        return "ROM verificada: Mario Kart 64 (US).";
    case ROM_ERR_OPEN:
        return "Nao foi possivel abrir ou ler o arquivo da ROM.";
    case ROM_ERR_SIZE:
        return "Tamanho errado: a ROM do Mario Kart 64 (US) tem 12 MB.";
    case ROM_ERR_FORMAT:
        return "O arquivo nao parece uma ROM de N64 (.z64/.v64/.n64).";
    case ROM_ERR_HASH:
        return "ROM diferente da esperada (SHA-1 nao confere). Use um dump do Mario Kart 64 US.";
    }
    return "Erro desconhecido.";
}
