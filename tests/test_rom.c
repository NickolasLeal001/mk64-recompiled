// SHA-1 known-answer tests (FIPS 180-4 examples) plus ROM rejection cases.
#include <stdio.h>
#include <string.h>

#include "rom.h"
#include "sha1.h"

static int failures = 0;

static void check_sha1(const char *input, const char *expected) {
    uint8_t d[20];
    char hex[41];
    sha1((const uint8_t *)input, strlen(input), d);
    sha1_hex(d, hex);
    if (strcmp(hex, expected) != 0) {
        printf("FAIL sha1(\"%.20s...\") = %s, esperado %s\n", input, hex, expected);
        failures++;
    }
}

int main(int argc, char **argv) {
    check_sha1("", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    check_sha1("abc", "a9993e364706816aba3e25717850c26c9cd0d89d");
    check_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
               "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    // 55 and 56 bytes straddle the one-block / two-block padding boundary.
    check_sha1("0123456789012345678901234567890123456789012345678901234",
               "9f3a4ce7f66b1b74c34da2c5d732c39f81e0f8df");

    Rom rom;
    if (rom_load("arquivo-que-nao-existe.z64", &rom) != ROM_ERR_OPEN) {
        printf("FAIL arquivo inexistente nao foi rejeitado\n");
        failures++;
    }

    // Optional: a real ROM path verifies the full happy path.
    if (argc > 1) {
        RomStatus s = rom_load(argv[1], &rom);
        printf("%s -> %s (sha1 %s)\n", argv[1], rom_status_message(s), rom.data ? rom.sha1 : "-");
        if (s != ROM_OK) {
            failures++;
        }
        rom_free(&rom);
    }

    printf(failures ? "%d falha(s)\n" : "ok\n", failures);
    return failures != 0;
}
