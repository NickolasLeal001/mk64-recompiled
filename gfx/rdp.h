// Software RDP: TMEM, texture formats, color combiner, blender, Z buffer and rasterization,
// drawing straight into the color image in RDRAM (RGBA16) like the real chip.
#ifndef RDP_H
#define RDP_H

#include <stdint.h>

typedef struct RdpVertex {
    float x, y;      // screen pixels
    float z;         // 0 (near) .. 1 (far)
    float invw;      // 1/w for perspective-correct texturing
    float r, g, b, a;  // shade, 0..1
    float s, t;      // texel coordinates (already scaled), before tile offset
} RdpVertex;

void rdp_reset(uint8_t *rdram);
// Draws all recorded primitives (in parallel bands). Called at the end of each display list.
void rdp_flush(void);
// Executes one RDP command (opcodes 0xE4-0xFF); `next` gives the following words for texrect.
void rdp_command(uint32_t w0, uint32_t w1);
void rdp_texrect(uint32_t w0, uint32_t w1, uint32_t st, uint32_t dsdt, int flip);
void rdp_triangle(const RdpVertex *v0, const RdpVertex *v1, const RdpVertex *v2);
void rdp_set_othermode(uint32_t hi, uint32_t lo);
void rdp_get_othermode(uint32_t *hi, uint32_t *lo);
// Texture tile and scale chosen by the RSP's G_TEXTURE.
int rdp_tile_size(int tile, int *w, int *h);

// Converts the RGBA16 image at `addr` to 32-bit ABGR8888 (SDL_PIXELFORMAT_ABGR8888) for display.
void rdp_read_framebuffer(uint32_t addr, int width, int height, uint32_t *out);
int rdp_cimg_width(void);

#endif
