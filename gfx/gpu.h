// GPU renderer: draws the display copies of the framebuffers (internal resolution, wider aspect
// ratio, in-between images) with SDL_gpu on Vulkan, and presents them. The 1x image in RDRAM, the
// one the game and the replays see, is still drawn in software by rdp.c.
#ifndef GPU_H
#define GPU_H

#include <SDL3/SDL.h>
#include <stdint.h>

// Creates the Vulkan device (window: claimed for presentation; NULL for headless runs). Returns 0
// with the reason logged when there is no GPU path, and the caller keeps the software renderer.
int gpu_init(SDL_Window *window, int debug);
int gpu_active(void);
const char *gpu_error(void);

// Presentation. `tex` (w x h) fills the log_w x 240 logical area when it is wider than 4:3, or its
// 320-wide middle otherwise; the logical area is letterboxed into the window. `hud` (optional,
// hud_w x hud_h) goes over the picture's top left corner, at 2 logical pixels per pixel.
#define HUD_MARGIN 4
void gpu_present(SDL_GPUTexture *tex, int w, int h, int log_w, int smooth, SDL_GPUTexture *hud, int hud_w, int hud_h);
// A 32-bit ABGR image (rdp_read_framebuffer format) in a texture of the presenter, for frames the
// GPU did not draw.
SDL_GPUTexture *gpu_upload_image(int slot, const uint32_t *pixels, int w, int h);
// Reads a texture back (ABGR, like rdp_read_framebuffer); for screenshots only.
int gpu_download(SDL_GPUTexture *tex, int w, int h, uint32_t *out);
// Waits until the GPU has finished all submitted work (MK64_GPU_TIMING: frame times include it).
void gpu_wait_idle(void);

#endif
