// Shared by rdp.vert, rdp.frag and gpu.c: primitive kinds and the layout of a state snapshot.
// Primitive kinds (low byte of the vertex's second id; K_FLIP marks a flipped texrect)
#define K_TRI 0u
#define K_TEXRECT 1u
#define K_RECT 2u
#define K_FILL 3u
#define K_SOLID 4u
#define K_PATCH 5u
#define K_ZCLEAR 6u
#define K_FLIP 256u

// Depth images hold z * DEPTH_SCALE: the software Z buffer compares raw floats (cleared to 1),
// so z beyond 1 must still be ordered, and the depth attachment only keeps 0..1.
#define DEPTH_SCALE 0.5

// State snapshot: STATE_WORDS 32-bit words (floats as their bits)
#define ST_OM_HI 0u
#define ST_OM_LO 1u
#define ST_COMB_HI 2u
#define ST_COMB_LO 3u
#define ST_FILL 4u
#define ST_TILE 5u
#define ST_TMEM 6u       // first word of its TMEM copy
#define ST_FLAGS 7u      // 1: needs TEXEL0, 2: needs TEXEL1
#define ST_PRIM 8u
#define ST_ENV 12u
#define ST_BLEND 16u
#define ST_FOG 20u
#define ST_PRIM_LOD 24u
#define ST_PRIM_DEPTH 25u
#define ST_SCISSOR 26u   // x0, y0, x1, y1 in pixels of the render target (exclusive end)
#define ST_TILES 32u     // 8 tiles of 8 words: bit fields, line | tmem << 9, uls, ult, lrs, lrt
#define STATE_WORDS 96u

// Image formats, sizes and cycle types (C code has them in rdp_internal.h)
#ifndef RDP_INTERNAL_H
#define FMT_RGBA 0u
#define FMT_CI 2u
#define FMT_IA 3u
#define FMT_I 4u
#define SIZ_4 0u
#define SIZ_8 1u
#define SIZ_16 2u
#define SIZ_32 3u
#define CYC_1 0u
#define CYC_2 1u
#define CYC_COPY 2u
#define CYC_FILL 3u
#endif
