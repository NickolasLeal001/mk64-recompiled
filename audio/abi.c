// High-level emulation of the game's audio microcode. Its command lists use the later libultra
// audio ABI (the one with ENVSETUP1/ENVSETUP2/ENVMIXER at 0x12/0x16/0x13 and LOADBUFF/SAVEBUFF at
// 0x14/0x15), identified from the lists the game builds; DMEM addresses are absolute. MK64's
// variant of it (as in Mupen64Plus' HLE "nead_mk") takes INTERLEAVE's output and size from
// SETBUFF and has no wet ramp. DMEM is kept as big-endian bytes like the RSP's data memory.
#include "abi.h"

#include <string.h>

#include "recomp.h"

static uint8_t g_dmem[0x10000];
static uint8_t *g_ram;
static int16_t g_lut[64 * 4];
static int g_lut_ok;

static struct {
    uint32_t segments[16];
    uint16_t in, out, count;
    uint16_t vol[2], rate[2];
    uint16_t vol_wet;
    int16_t table[16 * 8];
    uint32_t loop;
} A;

uint64_t abi_stats_unknown;
uint32_t abi_unknown_ops;  // bit per opcode seen but not handled

// --- helpers ---------------------------------------------------------------------------------
static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
static int16_t dm16(uint32_t a) { a &= 0xFFFE; return (int16_t)(g_dmem[a] << 8 | g_dmem[a + 1]); }
static void dw16(uint32_t a, int16_t v) { a &= 0xFFFE; g_dmem[a] = (uint8_t)((uint16_t)v >> 8); g_dmem[a + 1] = (uint8_t)v; }
static uint32_t addr(uint32_t so) { return (A.segments[(so >> 24) & 15] + (so & 0x00FFFFFFu)) & 0x00FFFFFFu; }
static int16_t ram16(uint32_t a) { return (int16_t)mem_r16(g_ram, 0x80000000u | a); }
static void ram16w(uint32_t a, int16_t v) { mem_w16(g_ram, 0x80000000u | a, (uint16_t)v); }

int abi_init(const uint8_t *rom, size_t rom_size, uint32_t lut_offset) {
    if (lut_offset + 512 > rom_size) return 0;
    long sum = 0;
    for (int k = 0; k < 256; k++) {
        g_lut[k] = (int16_t)(rom[lut_offset + 2 * k] << 8 | rom[lut_offset + 2 * k + 1]);
        if (k < 4) sum += g_lut[k];
    }
    // Each row of the resampling filter sums to about unity gain (1 << 15).
    g_lut_ok = sum > 32000 && sum < 33500;
    return g_lut_ok;
}

// --- ADPCM (VADPCM, 4 or, with flag 0x04, 2 bits per sample) -------------------------------------
static void adpcm(uint8_t flags, uint32_t state) {
    int16_t last[16];
    if (flags & 0x01) {
        memset(last, 0, sizeof last);
    } else {
        uint32_t src = (flags & 0x02) ? A.loop : state;
        for (int k = 0; k < 16; k++) last[k] = ram16(src + 2 * k);
    }
    uint32_t in = A.in, out = A.out;
    for (int k = 0; k < 16; k++, out += 2) dw16(out, last[k]);
    int count = (A.count + 31) & ~31;
    while (count > 0) {
        uint8_t code = g_dmem[in++ & 0xFFFF];
        int scale = code >> 4;
        const int16_t *book1 = A.table + ((code & 15) << 4), *book2 = book1 + 8;
        int16_t frame[16];
        if (flags & 0x04) {
            int shift = scale < 14 ? 14 - scale : 0;
            for (int k = 0; k < 4; k++) {
                uint8_t b = g_dmem[in++ & 0xFFFF];
                for (int q = 0; q < 4; q++)
                    frame[4 * k + q] = (int16_t)((int16_t)((uint16_t)((b >> (6 - 2 * q)) & 3) << 14) >> shift);
            }
        } else {
            int shift = scale < 12 ? 12 - scale : 0;
            for (int k = 0; k < 8; k++) {
                uint8_t b = g_dmem[in++ & 0xFFFF];
                frame[2 * k] = (int16_t)((int16_t)((uint16_t)(b >> 4) << 12) >> shift);
                frame[2 * k + 1] = (int16_t)((int16_t)((uint16_t)(b & 15) << 12) >> shift);
            }
        }
        for (int half = 0; half < 2; half++) {
            const int16_t *src = frame + 8 * half;
            int16_t l1 = half ? last[6] : last[14], l2 = half ? last[7] : last[15];
            int16_t *dst = last + 8 * half;
            for (int i = 0; i < 8; i++) {
                int32_t acc = (int32_t)src[i] << 11;
                acc += book1[i] * l1 + book2[i] * l2;
                for (int j = 0; j < i; j++) acc += book2[j] * src[i - j - 1];
                dst[i] = clamp16(acc >> 11);
            }
        }
        for (int k = 0; k < 16; k++, out += 2) dw16(out, last[k]);
        count -= 32;
    }
    for (int k = 0; k < 16; k++) ram16w(state + 2 * k, last[k]);
}

// --- RESAMPLE (4-tap filter from the microcode's table) ----------------------------------------
static void resample(uint8_t flags, uint16_t pitch16, uint32_t state) {
    uint32_t pitch = (uint32_t)pitch16 << 1;
    uint32_t ipos = A.in - 8, opos = A.out;  // 4 history samples sit before the input
    uint32_t accu;
    if (flags & 0x01) {
        for (int k = 0; k < 4; k++) dw16(ipos + 2 * k, 0);
        accu = 0;
    } else {
        for (int k = 0; k < 4; k++) dw16(ipos + 2 * k, ram16(state + 2 * k));
        accu = (uint16_t)ram16(state + 8);
    }
    int count = (A.count + 15) & ~15;
    for (int n = count / 2; n > 0; n--) {
        const int16_t *lut = g_lut + ((accu & 0xFC00) >> 8);
        int32_t s = 0;
        if (g_lut_ok) {
            for (int k = 0; k < 4; k++) s += dm16(ipos + 2 * k) * lut[k];
            s >>= 15;
        } else {  // no table: linear interpolation between the two middle samples
            float f = (accu & 0xFFFF) / 65536.0f;
            s = (int32_t)(dm16(ipos + 2) * (1 - f) + dm16(ipos + 4) * f);
        }
        dw16(opos, clamp16(s));
        opos += 2;
        accu += pitch;
        ipos += (accu >> 16) * 2;
        accu &= 0xFFFF;
    }
    for (int k = 0; k < 4; k++) ram16w(state + 2 * k, dm16(ipos + 2 * k));
    ram16w(state + 8, (int16_t)accu);
}

// --- ENVMIXER: volumes and ramps set by ENVSETUP1/2, mixed into dry and wet (reverb) buses ----
static void envmixer(uint32_t w0, uint32_t w1) {
    uint32_t in = ((w0 >> 16) & 0xFF) << 4;
    int n = (int)((w0 >> 8) & 0xFF);
    int16_t negs[2] = {(int16_t)((w0 >> 1) & 1 ? -1 : 0), (int16_t)(w0 & 1 ? -1 : 0)};
    uint32_t dry[2] = {((w1 >> 24) & 0xFF) << 4, ((w1 >> 16) & 0xFF) << 4};
    uint32_t wet[2] = {((w1 >> 8) & 0xFF) << 4, (w1 & 0xFF) << 4};
    uint16_t vols[2] = {A.vol[0], A.vol[1]};
    uint16_t vol_wet = A.vol_wet;
    n = (n + 7) & ~7;
    while (n > 0) {
        for (int i = 0; i < 8; i++) {
            int16_t s = dm16(in);
            in += 2;
            int16_t sides[2];
            for (int j = 0; j < 2; j++) sides[j] = (int16_t)(((s * vols[j]) >> 16) ^ negs[j]);
            for (int j = 0; j < 2; j++) {
                dw16(dry[j], clamp16(dm16(dry[j]) + sides[j]));
                dw16(wet[j], clamp16(dm16(wet[j]) + ((sides[j] * vol_wet) >> 16)));
                dry[j] += 2;
                wet[j] += 2;
            }
        }
        vols[0] = (uint16_t)(vols[0] + A.rate[0]);
        vols[1] = (uint16_t)(vols[1] + A.rate[1]);
        n -= 8;
    }
}

void abi_run(uint8_t *rdram, uint32_t list, uint32_t size) {
    g_ram = rdram;
    for (uint32_t k = 0; k + 8 <= size; k += 8) {
        uint32_t w0 = mem_r32(rdram, 0x80000000u | ((list + k) & 0x00FFFFFFu));
        uint32_t w1 = mem_r32(rdram, 0x80000000u | ((list + k + 4) & 0x00FFFFFFu));
        uint8_t op = (uint8_t)(w0 >> 24), flags = (uint8_t)(w0 >> 16);
        uint32_t packed = ((w0 >> 16) & 0xFF) << 4;  // size field stored as count >> 4
        switch (op) {
        case 0x00: break;  // SPNOOP
        case 0x01: adpcm(flags, addr(w1)); break;
        case 0x02:         // CLEARBUFF(dmem, count)
            memset(&g_dmem[w0 & 0xFFFF], 0, (w1 & 0xFFF) > 0x10000 - (w0 & 0xFFFF) ? 0 : (w1 & 0xFFF));
            break;
        case 0x05: resample(flags, (uint16_t)w0, addr(w1)); break;
        case 0x07: if (w1) A.segments[(w1 >> 24) & 15] = w1 & 0x00FFFFFFu; break;  // SEGMENT
        case 0x08:         // SETBUFF(in, out, count)
            A.in = (uint16_t)w0;
            A.out = (uint16_t)(w1 >> 16);
            A.count = (uint16_t)w1;
            break;
        case 0x0A: {       // DMEMMOVE(in, out, count)
            uint16_t i = (uint16_t)w0, o = (uint16_t)(w1 >> 16), c = (uint16_t)((w1 + 3) & ~3u);
            memmove(&g_dmem[o], &g_dmem[i], (size_t)c > (size_t)(0x10000 - (o > i ? o : i)) ? 0 : c);
            break;
        }
        case 0x0B: {       // LOADADPCM(count, addr)
            uint32_t a = addr(w1), n = (w0 & 0xFFFFFF) / 2;
            for (uint32_t j = 0; j < n && j < 16 * 8; j++) A.table[j] = ram16(a + 2 * j);
            break;
        }
        case 0x0C: {       // MIXER(count >> 4, gain, in, out)
            int16_t gain = (int16_t)w0;
            uint16_t i = (uint16_t)(w1 >> 16), o = (uint16_t)w1;
            for (uint32_t n = 0; n < packed; n += 2) {
                int32_t s = gain == -0x8000 ? dm16(o + n) - dm16(i + n)
                                            : ((dm16(o + n) * 0x7FFF + dm16(i + n) * gain) + 0x4000) >> 15;
                dw16(o + n, clamp16(s));
            }
            break;
        }
        case 0x0D: {       // INTERLEAVE(left, right): to SETBUFF's out, SETBUFF's count bytes per channel
            uint16_t o = A.out, l = (uint16_t)(w1 >> 16), r = (uint16_t)w1;
            int16_t tmp[4096];
            uint32_t frames = A.count / 2u;
            if (frames > 2048) frames = 2048;
            for (uint32_t j = 0; j < frames; j++) { tmp[2 * j] = dm16(l + 2 * j); tmp[2 * j + 1] = dm16(r + 2 * j); }
            for (uint32_t j = 0; j < 2 * frames; j++) dw16(o + 2 * j, tmp[j]);
            break;
        }
        case 0x0F: A.loop = addr(w1); break;  // SETLOOP
        case 0x11: {       // INTERL(count, in, out): every other sample (2:1 decimation)
            uint16_t c = (uint16_t)w0, i = (uint16_t)(w1 >> 16), o = (uint16_t)w1;
            for (int j = 0; j < c; j++) dw16(o + 2 * j, dm16(i + 4 * j));
            break;
        }
        case 0x12:         // ENVSETUP1(wet volume, unused, left ramp, right ramp): the wet volume stays
            A.vol_wet = (uint16_t)(flags << 8);
            A.rate[0] = (uint16_t)(w1 >> 16);
            A.rate[1] = (uint16_t)w1;
            break;
        case 0x13: envmixer(w0, w1); break;
        case 0x14: {       // LOADBUFF(count, dmem, addr): a DMA, so dmem/addr aligned to 4/8 and count to 8
            uint32_t a = addr(w1) & ~7u, d = w0 & 0xFFC, c = (((w0 >> 12) & 0xFFF) + 7) & ~7u;
            for (uint32_t n = 0; n < c; n++) g_dmem[(d + n) & 0xFFFF] = mem_r8(rdram, 0x80000000u | (a + n));
            break;
        }
        case 0x15: {       // SAVEBUFF(count, dmem, addr), aligned like LOADBUFF
            uint32_t a = addr(w1) & ~7u, d = w0 & 0xFFC, c = (((w0 >> 12) & 0xFFF) + 7) & ~7u;
            for (uint32_t n = 0; n < c; n++) mem_w8(rdram, 0x80000000u | (a + n), g_dmem[(d + n) & 0xFFFF]);
            break;
        }
        case 0x16:         // ENVSETUP2(initial left volume, initial right volume)
            A.vol[0] = (uint16_t)(w1 >> 16);
            A.vol[1] = (uint16_t)w1;
            break;
        default:
            abi_stats_unknown++;
            abi_unknown_ops |= 1u << (op & 31);
            break;
        }
    }
}
