// Recorded RDP work shared by the software rasterizer (rdp.c) and the GPU renderer (gpu.c):
// state snapshots, primitives and the display copies of the framebuffers.
#ifndef RDP_INTERNAL_H
#define RDP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "rdp.h"

// Image formats (G_IM_FMT_*) and sizes (G_IM_SIZ_*)
enum { FMT_RGBA, FMT_YUV, FMT_CI, FMT_IA, FMT_I };
enum { SIZ_4, SIZ_8, SIZ_16, SIZ_32 };

typedef struct Tile {
    int fmt, siz, line, tmem, palette;
    int cms, cmt, masks, maskt, shifts, shiftt;
    float uls, ult, lrs, lrt;  // texels
} Tile;

typedef struct Color { float r, g, b, a; } Color;

typedef struct RdpState {
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
    int tile;                        // tile sampled: G_TEXTURE tile or the texrect's own
    int need_tex0, need_tex1, need_mem;  // work the combiner/blender actually need
} RdpState;

// Display copy of one color image (internal resolution, wider aspect ratio or interpolation).
// A wider aspect ratio widens the copies of 320-pixel color images by `ox` pixels on each side
// (240 lines high); full-width scissors and rectangles reach the new edges.
typedef struct HiTarget {
    uint32_t addr;
    int w, ox, stride;  // 1x width; side margin and row length in enlarged pixels
    uint16_t *color, *shadow;  // color: software only; shadow: RDRAM as of the last flush
    uint16_t *icolor;   // frame interpolation: the in-between image (rdp_interp_begin/end)
    uint64_t istamp;    // interpolated run that drew icolor
    int pending;
    uint64_t used;
    struct GpuTarget *gpu;  // its textures when the GPU draws the display copies
} HiTarget;

enum { OP_TRI, OP_TEXRECT, OP_FILL, OP_RECT };

typedef struct Op {
    int kind;
    const RdpState *st;
    HiTarget *hi;  // display copy of its color image (internal resolution > 1)
    union {
        RdpVertex v[3];
        struct { float ulx, uly, lrx, lry, s0, t0, dsdx, dtdy; int flip; } tr;
        struct { int x0, y0, x1, y1; } rc;
    } u;
} Op;

// Othermode fields
#define OM_CYCLE_TYPE(st) (((st)->om_hi >> 20) & 3)
enum { CYC_1, CYC_2, CYC_COPY, CYC_FILL };

#endif
