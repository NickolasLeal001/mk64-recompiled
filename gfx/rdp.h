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
// Internal resolution (1-4), widescreen and frame interpolation. With any of them, the RDRAM image
// is still drawn at 1x (what the game sees), and an enlarged (and, for 320-pixel images, widened to
// 16:9) copy is drawn for display.
void rdp_set_scale(int scale, int widescreen, int interp);
int rdp_get_scale(void);
// Display copy of the framebuffer at `addr`, in the format of rdp_read_framebuffer; its size goes
// to out_w x out_h (at most (2 * width * scale) x (height * scale)). Returns 0 when there is none.
int rdp_read_framebuffer_hires(uint32_t addr, int width, int height, uint32_t *out, int *out_w, int *out_h);
// In-between image of the framebuffer at `addr` (same format and size as the display copy), drawn
// by the last interpolated run that targeted it; returns that run's id, or 0 when there is none.
uint64_t rdp_read_framebuffer_interp(uint32_t addr, int width, int height, uint32_t *out, int *out_w, int *out_h);
// Interpolated runs (rsp_run_display_list_interp): primitives recorded in between go only to the
// in-between images, and the RDP state is restored at the end.
void rdp_mark_start(void);  // state an interpolated run starts from (start of a display list)
void rdp_interp_begin(void);
void rdp_interp_end(void);
int rdp_cimg_width(void);

#endif
