// Software RDP. Draws into the color image in RDRAM (RGBA16) exactly where the game points it,
// so CPU reads of the framebuffer and the VI see the same pixels.
//
// Simplifications kept on purpose: no coverage/anti-aliasing (alpha-to-coverage modes use an alpha
// threshold), no dithering, bilinear instead of 3-point filtering, a float Z buffer per Z image.
#include "rdp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "recomp.h"
#include "rsp.h"

static uint8_t *g_ram;
#define RAM(a) (0x80000000u | ((a) & 0x00FFFFFFu))

// Image formats (G_IM_FMT_*) and sizes (G_IM_SIZ_*)
enum { FMT_RGBA, FMT_YUV, FMT_CI, FMT_IA, FMT_I };
enum { SIZ_4, SIZ_8, SIZ_16, SIZ_32 };

typedef struct Tile {
    int fmt, siz, line, tmem, palette;
    int cms, cmt, masks, maskt, shifts, shiftt;
    float uls, ult, lrs, lrt;  // texels
} Tile;

typedef struct Color { float r, g, b, a; } Color;

static struct {
    uint32_t om_hi, om_lo;
    uint32_t cimg, cimg_width, cimg_siz, zimg, timg, timg_width, timg_siz, timg_fmt;
    Tile tiles[8];
    uint8_t tmem[4096];
    uint32_t combine_hi, combine_lo;
    Color prim, env, blend, fog;
    float prim_lod;
    uint32_t fill;
    float prim_depth;
    int sc_x0, sc_y0, sc_x1, sc_y1;  // scissor, pixels, exclusive end
} R;

// One float Z buffer per Z image address (the game uses a single one).
static struct { uint32_t addr; int w, h; float *z; } g_zbuf;

// ---------------------------------------------------------------------------------------------
// Othermode fields
#define CYCLE_TYPE() ((R.om_hi >> 20) & 3)
enum { CYC_1, CYC_2, CYC_COPY, CYC_FILL };
#define TEX_PERSP() ((R.om_hi >> 19) & 1)
#define TEX_FILTER() ((R.om_hi >> 12) & 3)
#define TLUT_TYPE() ((R.om_hi >> 14) & 3)  // 2 = RGBA16, 3 = IA16
#define ALPHA_COMPARE() (R.om_lo & 3)
#define Z_SRC_PRIM() ((R.om_lo >> 2) & 1)
#define Z_CMP() ((R.om_lo >> 4) & 1)
#define Z_UPD() ((R.om_lo >> 5) & 1)
#define Z_MODE() ((R.om_lo >> 10) & 3)
#define CVG_X_ALPHA() ((R.om_lo >> 12) & 1)
#define ALPHA_CVG_SEL() ((R.om_lo >> 13) & 1)
#define FORCE_BL() ((R.om_lo >> 14) & 1)

void rdp_set_othermode(uint32_t hi, uint32_t lo) { R.om_hi = hi; R.om_lo = lo; }
void rdp_get_othermode(uint32_t *hi, uint32_t *lo) { *hi = R.om_hi; *lo = R.om_lo; }
int rdp_cimg_width(void) { return (int)R.cimg_width; }

// Per-primitive work skipped when the current state does not need it (same results either way).
static struct { int tex0, tex1, mem; } g_need;

static int combiner_uses(int sel_a, int sel_b, int sel_c, int sel_d, int aa, int ab, int ac, int ad, int texel) {
    // Color A/B/D and alpha A/B/D: 1 = TEXEL0, 2 = TEXEL1; color C adds 8/9 (texel alphas); alpha C 1/2.
    return sel_a == texel || sel_b == texel || sel_d == texel || sel_c == texel || sel_c == texel + 7 ||
           aa == texel || ab == texel || ad == texel || ac == texel;
}

static void update_needs(void) {
    uint32_t h = R.combine_hi, l = R.combine_lo;
    int two = CYCLE_TYPE() == CYC_2;
    for (int texel = 1; texel <= 2; texel++) {
        int u = combiner_uses((h >> 20) & 15, (l >> 28) & 15, (h >> 15) & 31, (l >> 15) & 7,
                              (h >> 12) & 7, (l >> 12) & 7, (h >> 9) & 7, (l >> 9) & 7, texel);
        if (two)
            u |= combiner_uses((h >> 5) & 15, (l >> 24) & 15, h & 31, (l >> 6) & 7,
                               (l >> 21) & 7, (l >> 3) & 7, (l >> 18) & 7, l & 7, texel);
        if (texel == 1) g_need.tex0 = u; else g_need.tex1 = u;
    }
    // Memory color: input 1 of P/M (memory color) or of B (memory alpha). In 2-cycle mode any use in
    // either cycle counts; in 1-cycle mode cycle 0 is final and, without FORCE_BL, only P is output.
    int p0 = (R.om_lo >> 30) & 3, m0 = (R.om_lo >> 22) & 3, b0 = (R.om_lo >> 18) & 3;
    int p1 = (R.om_lo >> 28) & 3, m1 = (R.om_lo >> 20) & 3, b1 = (R.om_lo >> 16) & 3;
    if (two)
        g_need.mem = p0 == 1 || m0 == 1 || b0 == 1 || p1 == 1 || m1 == 1 || b1 == 1;
    else
        g_need.mem = p0 == 1 || (FORCE_BL() && (m0 == 1 || b0 == 1));
}


static Color unpack32(uint32_t c) {
    Color o = {((c >> 24) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f};
    return o;
}

static Color rgba16(uint16_t c) {
    Color o = {((c >> 11) & 31) / 31.0f, ((c >> 6) & 31) / 31.0f, ((c >> 1) & 31) / 31.0f, (float)(c & 1)};
    return o;
}

static uint16_t pack16(Color c) {
    int r = (int)(c.r * 31.0f + 0.5f), g = (int)(c.g * 31.0f + 0.5f), b = (int)(c.b * 31.0f + 0.5f);
    return (uint16_t)((r << 11) | (g << 6) | (b << 1) | (c.a >= 0.5f ? 1 : 0));
}

// ---------------------------------------------------------------------------------------------
// TMEM. Texels are stored big-endian; odd rows have their 32-bit halves swapped (bytes ^ 4),
// and 32-bit texels keep RG in the low 2 KB and BA in the high 2 KB.
static uint8_t tmem_byte(uint32_t addr) { return R.tmem[addr & 0xFFF]; }

static void load_block(int tile, int uls, int ult, int lrs, int dxt) {
    Tile *t = &R.tiles[tile];
    int bytes_per_texel_x2 = R.timg_siz == SIZ_4 ? 1 : R.timg_siz == SIZ_8 ? 2 : R.timg_siz == SIZ_16 ? 4 : 8;
    uint32_t src = R.timg + (uint32_t)((ult * (int)R.timg_width + uls) * bytes_per_texel_x2 / 2);
    uint32_t nbytes = (uint32_t)((lrs - uls + 1) * bytes_per_texel_x2 / 2);
    nbytes = (nbytes + 7) & ~7u;
    uint32_t base = (uint32_t)t->tmem * 8;
    uint32_t words = nbytes / 8;
    uint32_t counter = 0;  // dxt accumulates per 64-bit word; bit 11 marks odd lines
    for (uint32_t w = 0; w < words; w++) {
        int odd = dxt ? (counter >> 11) & 1 : 0;
        counter += (uint32_t)dxt;
        for (int k = 0; k < 8; k++) {
            uint8_t b = mem_r8(g_ram, RAM(src + w * 8 + k));
            if (R.timg_siz == SIZ_32) {
                // RG halves to low TMEM, BA halves to high TMEM.
                uint32_t texel = (w * 8 + k) / 4, part = (w * 8 + k) % 4;
                uint32_t dst = base + texel * 2 + (part & 1);
                if (part >= 2) dst += 2048;
                if (odd) dst ^= 4;
                R.tmem[dst & 0xFFF] = b;
            } else {
                uint32_t dst = base + w * 8 + k;
                if (odd) dst ^= 4;
                R.tmem[dst & 0xFFF] = b;
            }
        }
    }
}

static void load_tile(int tile, int uls, int ult, int lrs, int lrt) {
    Tile *t = &R.tiles[tile];
    int bpp2 = R.timg_siz == SIZ_4 ? 1 : R.timg_siz == SIZ_8 ? 2 : R.timg_siz == SIZ_16 ? 4 : 8;  // half-bytes
    int width = lrs - uls + 1;
    for (int y = ult; y <= lrt; y++) {
        int row = y - ult;
        uint32_t src = R.timg + (uint32_t)((y * (int)R.timg_width + uls) * bpp2 / 2);
        int nbytes = width * bpp2 / 2;
        for (int k = 0; k < nbytes; k++) {
            uint8_t b = mem_r8(g_ram, RAM(src + (uint32_t)k));
            if (R.timg_siz == SIZ_32) {
                int texel = k / 4, part = k % 4;
                uint32_t dst = (uint32_t)t->tmem * 8 + (uint32_t)(row * t->line * 8) + (uint32_t)(texel * 2 + (part & 1));
                if (part >= 2) dst += 2048;
                if (row & 1) dst ^= 4;
                R.tmem[dst & 0xFFF] = b;
            } else {
                uint32_t dst = (uint32_t)t->tmem * 8 + (uint32_t)(row * t->line * 8 + k);
                if (row & 1) dst ^= 4;
                R.tmem[dst & 0xFFF] = b;
            }
        }
    }
    t->uls = (float)uls; t->ult = (float)ult;
}

static void load_tlut(int tile, int uls, int lrs) {
    Tile *t = &R.tiles[tile];
    // Palette entries are 16-bit; TMEM quadruples each one into a 64-bit word.
    for (int k = uls; k <= lrs; k++) {
        uint16_t c = mem_r16(g_ram, RAM(R.timg + (uint32_t)k * 2));
        uint32_t dst = (uint32_t)t->tmem * 8 + (uint32_t)(k - uls) * 8;
        for (int q = 0; q < 4; q++) {
            R.tmem[(dst + q * 2) & 0xFFF] = (uint8_t)(c >> 8);
            R.tmem[(dst + q * 2 + 1) & 0xFFF] = (uint8_t)c;
        }
    }
}

static Color tlut_color(int index) {
    uint32_t a = 0x800 + (uint32_t)index * 8;
    uint16_t c = (uint16_t)(tmem_byte(a) << 8 | tmem_byte(a + 1));
    if (TLUT_TYPE() == 3) {  // IA16
        float i = (c >> 8) / 255.0f;
        Color o = {i, i, i, (c & 0xFF) / 255.0f};
        return o;
    }
    return rgba16(c);
}

static Color fetch_texel(const Tile *t, int s, int tt) {
    uint32_t row = (uint32_t)t->tmem * 8 + (uint32_t)(tt * t->line * 8);
    uint32_t odd = (tt & 1) ? 4 : 0;
    Color o = {0, 0, 0, 0};
    switch (t->siz) {
    case SIZ_4: {
        uint8_t b = tmem_byte((row + (uint32_t)(s >> 1)) ^ odd);
        int v = (s & 1) ? (b & 15) : (b >> 4);
        if (t->fmt == FMT_CI || TLUT_TYPE() >= 2) return tlut_color((t->palette << 4) | v);
        if (t->fmt == FMT_IA) {
            float i = (v >> 1) / 7.0f;
            o.r = o.g = o.b = i; o.a = (float)(v & 1);
        } else {
            o.r = o.g = o.b = o.a = v / 15.0f;
        }
        return o;
    }
    case SIZ_8: {
        uint8_t b = tmem_byte((row + (uint32_t)s) ^ odd);
        if (t->fmt == FMT_CI || TLUT_TYPE() >= 2) return tlut_color(b);
        if (t->fmt == FMT_IA) {
            o.r = o.g = o.b = (b >> 4) / 15.0f; o.a = (b & 15) / 15.0f;
        } else {
            o.r = o.g = o.b = o.a = b / 255.0f;
        }
        return o;
    }
    case SIZ_16: {
        uint32_t a = (row + (uint32_t)s * 2) ^ odd;
        uint16_t c = (uint16_t)(tmem_byte(a) << 8 | tmem_byte(a + 1));
        if (t->fmt == FMT_IA) {
            o.r = o.g = o.b = (c >> 8) / 255.0f; o.a = (c & 0xFF) / 255.0f;
            return o;
        }
        if (t->fmt == FMT_CI) return tlut_color(c >> 8);
        return rgba16(c);
    }
    default: {  // 32-bit RGBA: RG in low TMEM, BA in high TMEM
        uint32_t a = ((uint32_t)t->tmem * 8 + (uint32_t)(tt * t->line * 8) + (uint32_t)s * 2) ^ odd;
        o.r = tmem_byte(a) / 255.0f; o.g = tmem_byte(a + 1) / 255.0f;
        o.b = tmem_byte(a + 2048) / 255.0f; o.a = tmem_byte(a + 2049) / 255.0f;
        return o;
    }
    }
}

// Applies shift, clamp, mirror and mask to one texel coordinate (integer texels).
static int wrap_coord(int c, int mask, int cm, int max) {
    int clamp = (cm & 2) || mask == 0;
    if (clamp) c = c < 0 ? 0 : c > max ? max : c;
    if (mask) {
        int size = 1 << mask;
        if (cm & 1) {  // mirror
            int m = c & ((size << 1) - 1);
            c = m >= size ? (size << 1) - 1 - m : m;
        } else {
            c &= size - 1;
        }
    }
    return c;
}

static float shift_coord(float c, int shift) {
    if (shift == 0) return c;
    if (shift < 11) return c / (float)(1 << shift);
    return c * (float)(1 << (16 - shift));
}

static Color sample(int tile_index, float s, float t) {
    const Tile *tl = &R.tiles[tile_index & 7];
    s = shift_coord(s, tl->shifts) - tl->uls;
    t = shift_coord(t, tl->shiftt) - tl->ult;
    int maxs = (int)(tl->lrs - tl->uls), maxt = (int)(tl->lrt - tl->ult);
    if (TEX_FILTER() == 0 || CYCLE_TYPE() == CYC_COPY) {
        int is = wrap_coord((int)floorf(s), tl->masks, tl->cms, maxs);
        int it = wrap_coord((int)floorf(t), tl->maskt, tl->cmt, maxt);
        return fetch_texel(tl, is, it);
    }
    s -= 0.5f; t -= 0.5f;
    float fs = floorf(s), ft = floorf(t);
    float a = s - fs, b = t - ft;
    int s0 = (int)fs, t0 = (int)ft;
    int s0w = wrap_coord(s0, tl->masks, tl->cms, maxs), s1w = wrap_coord(s0 + 1, tl->masks, tl->cms, maxs);
    int t0w = wrap_coord(t0, tl->maskt, tl->cmt, maxt), t1w = wrap_coord(t0 + 1, tl->maskt, tl->cmt, maxt);
    Color c00 = fetch_texel(tl, s0w, t0w), c10 = fetch_texel(tl, s1w, t0w);
    Color c01 = fetch_texel(tl, s0w, t1w), c11 = fetch_texel(tl, s1w, t1w);
    Color o;
    o.r = (c00.r * (1 - a) + c10.r * a) * (1 - b) + (c01.r * (1 - a) + c11.r * a) * b;
    o.g = (c00.g * (1 - a) + c10.g * a) * (1 - b) + (c01.g * (1 - a) + c11.g * a) * b;
    o.b = (c00.b * (1 - a) + c10.b * a) * (1 - b) + (c01.b * (1 - a) + c11.b * a) * b;
    o.a = (c00.a * (1 - a) + c10.a * a) * (1 - b) + (c01.a * (1 - a) + c11.a * a) * b;
    return o;
}

// ---------------------------------------------------------------------------------------------
// Color combiner: (A - B) * C + D per cycle, separately for color and alpha.
typedef struct Inputs { Color comb, t0, t1, shade; } Inputs;

static float sel_rgb(int which, int sel, const Inputs *in, int ch) {
    #define CH(col) ((&(col).r)[ch])
    switch (which) {
    case 0:  // A
        switch (sel) { case 0: return CH(in->comb); case 1: return CH(in->t0); case 2: return CH(in->t1);
                       case 3: return CH(R.prim); case 4: return CH(in->shade); case 5: return CH(R.env);
                       case 6: return 1.0f; default: return 0.0f; }
    case 1:  // B
        switch (sel) { case 0: return CH(in->comb); case 1: return CH(in->t0); case 2: return CH(in->t1);
                       case 3: return CH(R.prim); case 4: return CH(in->shade); case 5: return CH(R.env);
                       default: return 0.0f; }
    case 2:  // C
        switch (sel) { case 0: return CH(in->comb); case 1: return CH(in->t0); case 2: return CH(in->t1);
                       case 3: return CH(R.prim); case 4: return CH(in->shade); case 5: return CH(R.env);
                       case 7: return in->comb.a; case 8: return in->t0.a; case 9: return in->t1.a;
                       case 10: return R.prim.a; case 11: return in->shade.a; case 12: return R.env.a;
                       case 13: return 1.0f; case 14: return R.prim_lod; default: return 0.0f; }
    default:  // D
        switch (sel) { case 0: return CH(in->comb); case 1: return CH(in->t0); case 2: return CH(in->t1);
                       case 3: return CH(R.prim); case 4: return CH(in->shade); case 5: return CH(R.env);
                       case 6: return 1.0f; default: return 0.0f; }
    }
    #undef CH
}

static float sel_alpha(int which, int sel, const Inputs *in) {
    if (which == 2) {  // C
        switch (sel) { case 0: return 1.0f; case 1: return in->t0.a; case 2: return in->t1.a; case 3: return R.prim.a;
                       case 4: return in->shade.a; case 5: return R.env.a; case 6: return R.prim_lod; default: return 0.0f; }
    }
    switch (sel) { case 0: return in->comb.a; case 1: return in->t0.a; case 2: return in->t1.a; case 3: return R.prim.a;
                   case 4: return in->shade.a; case 5: return R.env.a; case 6: return 1.0f; default: return 0.0f; }
}

static float clampf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static Color combine_cycle(int cycle, const Inputs *in) {
    uint32_t h = R.combine_hi, l = R.combine_lo;
    int a, b, c, d, aa, ab, ac, ad;
    if (cycle == 0) {
        a = (h >> 20) & 15; c = (h >> 15) & 31; aa = (h >> 12) & 7; ac = (h >> 9) & 7;
        b = (l >> 28) & 15; d = (l >> 15) & 7; ab = (l >> 12) & 7; ad = (l >> 9) & 7;
    } else {
        a = (h >> 5) & 15; c = h & 31; aa = (l >> 21) & 7; ac = (l >> 18) & 7;
        b = (l >> 24) & 15; d = (l >> 6) & 7; ab = (l >> 3) & 7; ad = l & 7;
    }
    Color o;
    for (int ch = 0; ch < 3; ch++)
        (&o.r)[ch] = clampf((sel_rgb(0, a, in, ch) - sel_rgb(1, b, in, ch)) * sel_rgb(2, c, in, ch) + sel_rgb(3, d, in, ch));
    o.a = clampf((sel_alpha(0, aa, in) - sel_alpha(1, ab, in)) * sel_alpha(2, ac, in) + sel_alpha(3, ad, in));
    return o;
}

// ---------------------------------------------------------------------------------------------
// Blender: (P * A + M * B) / (A + B). Blending applies only with FORCE_BL; otherwise the final
// cycle passes its first input through (coverage-driven edge blending is not modelled).
static Color blend_cycle(int cycle, Color pixel, Color mem, float shade_alpha, int final) {
    int sh = cycle == 0 ? 30 : 28;
    int p = (R.om_lo >> sh) & 3, a = (R.om_lo >> (sh - 4)) & 3, m = (R.om_lo >> (sh - 8)) & 3, b = (R.om_lo >> (sh - 12)) & 3;
    Color P = p == 0 ? pixel : p == 1 ? mem : p == 2 ? R.blend : R.fog;
    Color M = m == 0 ? pixel : m == 1 ? mem : m == 2 ? R.blend : R.fog;
    float A = a == 0 ? pixel.a : a == 1 ? R.fog.a : a == 2 ? shade_alpha : 0.0f;
    float B = b == 0 ? 1.0f - A : b == 1 ? mem.a : b == 2 ? 1.0f : 0.0f;
    if (final && !FORCE_BL()) {
        P.a = pixel.a;
        return P;
    }
    float sum = A + B;
    if (sum <= 0) sum = 1;
    Color o = {(P.r * A + M.r * B) / sum, (P.g * A + M.g * B) / sum, (P.b * A + M.b * B) / sum, pixel.a};
    return o;
}

// ---------------------------------------------------------------------------------------------
static float *zbuffer(void) {
    if (!R.zimg || !R.cimg_width) return NULL;
    int w = (int)R.cimg_width, h = 240;
    if (!g_zbuf.z || g_zbuf.addr != R.zimg || g_zbuf.w != w) {
        free(g_zbuf.z);
        g_zbuf.z = malloc(sizeof(float) * (size_t)w * h);
        for (int k = 0; k < w * h; k++) g_zbuf.z[k] = 1.0f;
        g_zbuf.addr = R.zimg; g_zbuf.w = w; g_zbuf.h = h;
    }
    return g_zbuf.z;
}

static void write_pixel(int x, int y, Color comb, float shade_alpha, float z) {
    if (ALPHA_COMPARE() == 1 && comb.a < R.blend.a) return;
    if ((CVG_X_ALPHA() || ALPHA_CVG_SEL()) && comb.a < 0.5f && !FORCE_BL()) return;
    float *zb = NULL;
    int zi = 0;
    if (Z_CMP() || Z_UPD()) {
        zb = zbuffer();
        if (zb && y < g_zbuf.h) zi = y * g_zbuf.w + x; else zb = NULL;
    }
    if (Z_SRC_PRIM()) z = R.prim_depth;
    if (zb && Z_CMP()) {
        float cur = zb[zi];
        if (Z_MODE() == 3) { if (z > cur + 0.002f) return; }  // decal: on top of what is there
        else if (z > cur) return;
    }
    uint32_t addr = R.cimg + (uint32_t)(y * (int)R.cimg_width + x) * 2;
    Color mem = {0, 0, 0, 0};
    if (g_need.mem) mem = rgba16(mem_r16(g_ram, RAM(addr)));
    Color out;
    if (CYCLE_TYPE() == CYC_2) {
        Color c0 = blend_cycle(0, comb, mem, shade_alpha, 0);
        out = blend_cycle(1, c0, mem, shade_alpha, 1);
    } else {
        out = blend_cycle(0, comb, mem, shade_alpha, 1);
    }
    mem_w16(g_ram, RAM(addr), pack16(out));
    if (zb && Z_UPD()) zb[zi] = z;
}

static Color shade_pixel(Color shade, float s, float t) {
    Inputs in;
    in.shade = shade;
    int tile = rsp_current_tile();
    Color zero = {0, 0, 0, 0};
    in.t0 = g_need.tex0 || (g_need.tex1 && CYCLE_TYPE() != CYC_2) ? sample(tile, s, t) : zero;
    in.t1 = CYCLE_TYPE() == CYC_2 ? (g_need.tex1 ? sample(tile + 1, s, t) : zero) : in.t0;
    in.comb = zero;
    Color c = combine_cycle(0, &in);
    if (CYCLE_TYPE() == CYC_2) {
        in.comb = c;
        c = combine_cycle(1, &in);
    }
    return c;
}

// ---------------------------------------------------------------------------------------------
void rdp_triangle(const RdpVertex *v0, const RdpVertex *v1, const RdpVertex *v2) {
    if (!R.cimg_width) return;
    update_needs();
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (fabsf(area) < 1e-6f) return;
    float minx = fminf(v0->x, fminf(v1->x, v2->x)), maxx = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    float miny = fminf(v0->y, fminf(v1->y, v2->y)), maxy = fmaxf(v0->y, fmaxf(v1->y, v2->y));
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < R.sc_x0) x0 = R.sc_x0;
    if (y0 < R.sc_y0) y0 = R.sc_y0;
    if (x1 > R.sc_x1) x1 = R.sc_x1;
    if (y1 > R.sc_y1) y1 = R.sc_y1;
    int persp = TEX_PERSP();
    float inv = 1.0f / area;
    for (int y = y0; y < y1; y++) {
        float py = y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f;
            float w0 = ((v1->x - px) * (v2->y - py) - (v2->x - px) * (v1->y - py)) * inv;
            float w1 = ((v2->x - px) * (v0->y - py) - (v0->x - px) * (v2->y - py)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            Color shade = {v0->r * w0 + v1->r * w1 + v2->r * w2, v0->g * w0 + v1->g * w1 + v2->g * w2,
                           v0->b * w0 + v1->b * w1 + v2->b * w2, v0->a * w0 + v1->a * w1 + v2->a * w2};
            float s, t;
            if (persp) {
                float iw = v0->invw * w0 + v1->invw * w1 + v2->invw * w2;
                s = (v0->s * v0->invw * w0 + v1->s * v1->invw * w1 + v2->s * v2->invw * w2) / iw;
                t = (v0->t * v0->invw * w0 + v1->t * v1->invw * w1 + v2->t * v2->invw * w2) / iw;
            } else {
                s = v0->s * w0 + v1->s * w1 + v2->s * w2;
                t = v0->t * w0 + v1->t * w1 + v2->t * w2;
            }
            float z = v0->z * w0 + v1->z * w1 + v2->z * w2;
            write_pixel(x, y, shade_pixel(shade, s, t), shade.a, z);
        }
    }
}

static void fill_rect(int x0, int y0, int x1, int y1) {
    if (x0 < R.sc_x0) x0 = R.sc_x0;
    if (y0 < R.sc_y0) y0 = R.sc_y0;
    if (x1 > R.sc_x1) x1 = R.sc_x1;
    if (y1 > R.sc_y1) y1 = R.sc_y1;
    if (R.cimg == R.zimg) {  // clearing the Z image
        float *zb = zbuffer();
        if (zb)
            for (int y = y0; y < y1 && y < g_zbuf.h; y++)
                for (int x = x0; x < x1; x++) zb[y * g_zbuf.w + x] = 1.0f;
    }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t addr = R.cimg + (uint32_t)(y * (int)R.cimg_width + x) * 2;
            uint16_t c = (uint16_t)((x & 1) ? R.fill : (R.fill >> 16));
            mem_w16(g_ram, RAM(addr), c);
        }
}

void rdp_texrect(uint32_t w0, uint32_t w1, uint32_t st, uint32_t dsdt, int flip) {
    if (!R.cimg_width) return;
    update_needs();
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    int tile = (w1 >> 24) & 7;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    float s0 = (int16_t)(st >> 16) / 32.0f, t0 = (int16_t)st / 32.0f;
    float dsdx = (int16_t)(dsdt >> 16) / 1024.0f, dtdy = (int16_t)dsdt / 1024.0f;
    int cyc = CYCLE_TYPE();
    if (cyc == CYC_COPY) { dsdx /= 4.0f; lrx += 1; lry += 1; }
    else if (cyc == CYC_FILL) { lrx += 1; lry += 1; }
    int x0 = (int)ulx, y0 = (int)uly, x1 = (int)lrx, y1 = (int)lry;
    if (x0 < R.sc_x0) x0 = R.sc_x0;
    if (y0 < R.sc_y0) y0 = R.sc_y0;
    if (x1 > R.sc_x1) x1 = R.sc_x1;
    if (y1 > R.sc_y1) y1 = R.sc_y1;
    rsp_set_rect_tile(tile);
    Color white = {1, 1, 1, 1};
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x - ulx, dy = y - uly;
            float s = flip ? s0 + dy * dsdx : s0 + dx * dsdx;
            float t = flip ? t0 + dx * dtdy : t0 + dy * dtdy;
            Color c;
            if (cyc == CYC_COPY) {
                c = sample(tile, s, t);
                if (ALPHA_COMPARE() && c.a < 0.5f) continue;
                uint32_t addr = R.cimg + (uint32_t)(y * (int)R.cimg_width + x) * 2;
                mem_w16(g_ram, RAM(addr), pack16(c));
                continue;
            }
            c = shade_pixel(white, s, t);
            write_pixel(x, y, c, 1.0f, R.prim_depth);
        }
    rsp_set_rect_tile(-1);
}

void rdp_command(uint32_t w0, uint32_t w1) {
    uint8_t op = (uint8_t)(w0 >> 24);
    switch (op) {
    case 0xFF:  // SETCIMG
        R.cimg = w1 & 0x00FFFFFFu; R.cimg_width = (w0 & 0xFFF) + 1; R.cimg_siz = (w0 >> 19) & 3;
        break;
    case 0xFE: R.zimg = w1 & 0x00FFFFFFu; break;
    case 0xFD:  // SETTIMG
        R.timg = w1 & 0x00FFFFFFu; R.timg_width = (w0 & 0xFFF) + 1;
        R.timg_fmt = (w0 >> 21) & 7; R.timg_siz = (w0 >> 19) & 3;
        break;
    case 0xFC: R.combine_hi = w0 & 0x00FFFFFFu; R.combine_lo = w1; break;
    case 0xFB: R.env = unpack32(w1); break;
    case 0xFA: R.prim = unpack32(w1); R.prim_lod = (w0 & 0xFF) / 255.0f; break;
    case 0xF9: R.blend = unpack32(w1); break;
    case 0xF8: R.fog = unpack32(w1); break;
    case 0xF7: R.fill = w1; break;
    case 0xF6: {  // FILLRECT: lrx lry | ulx uly in 10.2
        int lrx = (int)((w0 >> 12) & 0xFFF) / 4, lry = (int)(w0 & 0xFFF) / 4;
        int ulx = (int)((w1 >> 12) & 0xFFF) / 4, uly = (int)(w1 & 0xFFF) / 4;
        int cyc = CYCLE_TYPE();
        if (cyc == CYC_FILL || cyc == CYC_COPY) { lrx++; lry++; }
        if (cyc == CYC_FILL) {
            fill_rect(ulx, uly, lrx, lry);
        } else {  // 1/2-cycle rectangles go through the combiner and blender
            update_needs();
            Color white = {1, 1, 1, 1};
            for (int y = uly < R.sc_y0 ? R.sc_y0 : uly; y < lry && y < R.sc_y1; y++)
                for (int x = ulx < R.sc_x0 ? R.sc_x0 : ulx; x < lrx && x < R.sc_x1; x++)
                    write_pixel(x, y, shade_pixel(white, 0, 0), 1.0f, R.prim_depth);
        }
        break;
    }
    case 0xF5: {  // SETTILE
        Tile *t = &R.tiles[(w1 >> 24) & 7];
        t->fmt = (w0 >> 21) & 7; t->siz = (w0 >> 19) & 3; t->line = (w0 >> 9) & 0x1FF; t->tmem = w0 & 0x1FF;
        t->palette = (w1 >> 20) & 15;
        t->cmt = (w1 >> 18) & 3; t->maskt = (w1 >> 14) & 15; t->shiftt = (w1 >> 10) & 15;
        t->cms = (w1 >> 8) & 3; t->masks = (w1 >> 4) & 15; t->shifts = w1 & 15;
        break;
    }
    case 0xF4:  // LOADTILE
        load_tile((w1 >> 24) & 7, (int)((w0 >> 12) & 0xFFF) / 4, (int)(w0 & 0xFFF) / 4,
                  (int)((w1 >> 12) & 0xFFF) / 4, (int)(w1 & 0xFFF) / 4);
        break;
    case 0xF3:  // LOADBLOCK
        load_block((w1 >> 24) & 7, (int)((w0 >> 12) & 0xFFF), (int)(w0 & 0xFFF), (int)((w1 >> 12) & 0xFFF), (int)(w1 & 0xFFF));
        break;
    case 0xF2: {  // SETTILESIZE
        Tile *t = &R.tiles[(w1 >> 24) & 7];
        t->uls = ((w0 >> 12) & 0xFFF) / 4.0f; t->ult = (w0 & 0xFFF) / 4.0f;
        t->lrs = ((w1 >> 12) & 0xFFF) / 4.0f; t->lrt = (w1 & 0xFFF) / 4.0f;
        break;
    }
    case 0xF0:  // LOADTLUT
        load_tlut((w1 >> 24) & 7, (int)((w0 >> 12) & 0xFFF) / 4, (int)((w1 >> 12) & 0xFFF) / 4);
        break;
    case 0xEF: R.om_hi = w0 & 0x00FFFFFFu; R.om_lo = w1; break;
    case 0xEE: R.prim_depth = ((w1 >> 16) & 0x7FFF) / 32767.0f; break;
    case 0xED:  // SETSCISSOR
        R.sc_x0 = (int)((w0 >> 12) & 0xFFF) / 4; R.sc_y0 = (int)(w0 & 0xFFF) / 4;
        R.sc_x1 = (int)((w1 >> 12) & 0xFFF) / 4; R.sc_y1 = (int)(w1 & 0xFFF) / 4;
        if (R.sc_y1 > 240) R.sc_y1 = 240;
        break;
    default: break;  // syncs, convert, key
    }
}

void rdp_reset(uint8_t *rdram) {
    g_ram = rdram;
}

void rdp_read_framebuffer(uint32_t addr, int width, int height, uint32_t *out) {
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            uint16_t c = mem_r16(g_ram, RAM(addr + (uint32_t)(y * width + x) * 2));
            uint32_t r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 6) & 31) * 255 / 31, b = ((c >> 1) & 31) * 255 / 31;
            out[y * width + x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
}

int rdp_tile_size(int tile, int *w, int *h) {
    const Tile *t = &R.tiles[tile & 7];
    *w = (int)(t->lrs - t->uls) + 1;
    *h = (int)(t->lrt - t->ult) + 1;
    return 1;
}
