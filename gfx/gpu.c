// GPU renderer on SDL_gpu (Vulkan driver). See gpu.h.
#include "gpu.h"

#include <stdlib.h>
#include <string.h>

#include "rdp_internal.h"
#include "ultra.h"

static char g_error[256];
const char *gpu_error(void) { return g_error; }

#ifndef MK64_GPU

int gpu_init(SDL_Window *window, int debug) {
    (void)window; (void)debug;
    SDL_snprintf(g_error, sizeof g_error, "compilado sem shaders (glslc do Vulkan SDK nao encontrado no build)");
    return 0;
}
int gpu_active(void) { return 0; }
void gpu_present(SDL_GPUTexture *tex, int w, int h, int log_w, int smooth) { (void)tex; (void)w; (void)h; (void)log_w; (void)smooth; }
SDL_GPUTexture *gpu_upload_image(int slot, const uint32_t *pixels, int w, int h) { (void)slot; (void)pixels; (void)w; (void)h; return NULL; }
int gpu_download(SDL_GPUTexture *tex, int w, int h, uint32_t *out) { (void)tex; (void)w; (void)h; (void)out; return 0; }
SDL_GPUTexture *gpu_test_target(int w, int h) { (void)w; (void)h; return NULL; }

#else

static const uint32_t VS_CODE[] =
#include "rdp.vert.inc"
;
static const uint32_t FS_CODE[] =
#include "rdp.frag.inc"
;

#define COLOR_FORMAT SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM

static SDL_GPUDevice *g_dev;
static SDL_Window *g_win;
static SDL_GPUTextureFormat g_depth_format;
static SDL_GPUShader *g_vs, *g_fs;

int gpu_active(void) { return g_dev != NULL; }

static int fail(const char *what) {
    SDL_snprintf(g_error, sizeof g_error, "%s: %s", what, SDL_GetError());
    return 0;
}

static SDL_GPUShader *make_shader(const uint32_t *code, size_t size, SDL_GPUShaderStage stage, Uint32 storage, Uint32 uniforms) {
    SDL_GPUShaderCreateInfo info = {0};
    info.code = (const Uint8 *)code;
    info.code_size = size;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_storage_buffers = storage;
    info.num_uniform_buffers = uniforms;
    return SDL_CreateGPUShader(g_dev, &info);
}

int gpu_init(SDL_Window *window, int debug) {
    g_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug != 0, "vulkan");
    if (!g_dev && debug) g_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");  // no validation layers
    if (!g_dev) return fail("SDL_CreateGPUDevice (vulkan)");
    g_depth_format = SDL_GPUTextureSupportsFormat(g_dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                                  SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                         ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT : SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    g_vs = make_shader(VS_CODE, sizeof VS_CODE, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    g_fs = make_shader(FS_CODE, sizeof FS_CODE, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    if (!g_vs || !g_fs) {
        fail("SDL_CreateGPUShader");
        SDL_DestroyGPUDevice(g_dev);
        g_dev = NULL;
        return 0;
    }
    if (window) {
        if (!SDL_ClaimWindowForGPUDevice(g_dev, window)) {
            fail("SDL_ClaimWindowForGPUDevice");
            SDL_DestroyGPUDevice(g_dev);
            g_dev = NULL;
            return 0;
        }
        g_win = window;
        // No vsync, like the SDL_Renderer path: the game paces itself on the emulated retrace.
        SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
        if (SDL_WindowSupportsGPUPresentMode(g_dev, window, SDL_GPU_PRESENTMODE_IMMEDIATE)) mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
        else if (SDL_WindowSupportsGPUPresentMode(g_dev, window, SDL_GPU_PRESENTMODE_MAILBOX)) mode = SDL_GPU_PRESENTMODE_MAILBOX;
        SDL_SetGPUSwapchainParameters(g_dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
    }
    ultra_log("GPU: %s, %s", SDL_GetGPUDeviceDriver(g_dev),
            SDL_GetStringProperty(SDL_GetGPUDeviceProperties(g_dev), SDL_PROP_GPU_DEVICE_NAME_STRING, "?"));
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Presentation

typedef struct Upload { SDL_GPUTexture *tex; SDL_GPUTransferBuffer *tb; int w, h; } Upload;
static Upload g_uploads[2];

SDL_GPUTexture *gpu_upload_image(int slot, const uint32_t *pixels, int w, int h) {
    Upload *u = &g_uploads[slot & 1];
    if (!u->tex || u->w != w || u->h != h) {
        if (u->tex) SDL_ReleaseGPUTexture(g_dev, u->tex);
        if (u->tb) SDL_ReleaseGPUTransferBuffer(g_dev, u->tb);
        SDL_GPUTextureCreateInfo ti = {0};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = COLOR_FORMAT;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = (Uint32)w;
        ti.height = (Uint32)h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        u->tex = SDL_CreateGPUTexture(g_dev, &ti);
        SDL_GPUTransferBufferCreateInfo bi = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, (Uint32)(w * h * 4), 0};
        u->tb = SDL_CreateGPUTransferBuffer(g_dev, &bi);
        u->w = w;
        u->h = h;
    }
    void *dst = SDL_MapGPUTransferBuffer(g_dev, u->tb, true);
    memcpy(dst, pixels, (size_t)w * h * 4);
    SDL_UnmapGPUTransferBuffer(g_dev, u->tb);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {u->tb, 0, (Uint32)w, (Uint32)h};
    SDL_GPUTextureRegion region = {u->tex, 0, 0, 0, 0, 0, (Uint32)w, (Uint32)h, 1};
    SDL_UploadToGPUTexture(copy, &src, &region, true);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    return u->tex;
}

void gpu_present(SDL_GPUTexture *tex, int w, int h, int log_w, int smooth) {
    if (!g_win) return;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    SDL_GPUTexture *swap = NULL;
    Uint32 sw = 0, sh = 0;
    if (!SDL_AcquireGPUSwapchainTexture(cmd, g_win, &swap, &sw, &sh) || !swap) {
        SDL_SubmitGPUCommandBuffer(cmd);  // window minimized or too many frames in flight
        return;
    }
    SDL_GPUColorTargetInfo clear = {0};
    clear.texture = swap;
    clear.load_op = SDL_GPU_LOADOP_CLEAR;
    clear.store_op = SDL_GPU_STOREOP_STORE;
    clear.clear_color.a = 1.0f;
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &clear, 1, NULL));
    if (tex) {
        // Letterbox of the log_w x 240 logical area, like SDL_LOGICAL_PRESENTATION_LETTERBOX.
        float scale = SDL_min((float)sw / (float)log_w, (float)sh / 240.0f);
        float lw = (float)log_w * scale, lh = 240.0f * scale;
        float x = ((float)sw - lw) / 2.0f, y = ((float)sh - lh) / 2.0f;
        if (w * 3 <= h * 4) {  // a 4:3 picture sits in the middle of a wider logical area
            x += ((float)log_w - 320.0f) / 2.0f * scale;
            lw = 320.0f * scale;
        }
        SDL_GPUBlitInfo blit = {0};
        blit.source.texture = tex;
        blit.source.w = (Uint32)w;
        blit.source.h = (Uint32)h;
        blit.destination.texture = swap;
        blit.destination.x = (Uint32)(x + 0.5f);
        blit.destination.y = (Uint32)(y + 0.5f);
        blit.destination.w = (Uint32)(lw + 0.5f);
        blit.destination.h = (Uint32)(lh + 0.5f);
        blit.load_op = SDL_GPU_LOADOP_LOAD;
        blit.filter = smooth ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(cmd, &blit);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

// M0 check of the render target path: a target cleared to a fixed color (MK64_GPU_TEST).
SDL_GPUTexture *gpu_test_target(int w, int h) {
    static SDL_GPUTexture *tex;
    if (!tex) {
        SDL_GPUTextureCreateInfo ti = {0};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = COLOR_FORMAT;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = (Uint32)w;
        ti.height = (Uint32)h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        tex = SDL_CreateGPUTexture(g_dev, &ti);
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    SDL_GPUColorTargetInfo ct = {0};
    ct.texture = tex;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.clear_color = (SDL_FColor){0.2f, 0.4f, 0.8f, 1.0f};
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &ct, 1, NULL));
    SDL_SubmitGPUCommandBuffer(cmd);
    return tex;
}

int gpu_download(SDL_GPUTexture *tex, int w, int h, uint32_t *out) {
    SDL_GPUTransferBufferCreateInfo bi = {SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, (Uint32)(w * h * 4), 0};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(g_dev, &bi);
    if (!tb) return 0;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region = {tex, 0, 0, 0, 0, 0, (Uint32)w, (Uint32)h, 1};
    SDL_GPUTextureTransferInfo dst = {tb, 0, (Uint32)w, (Uint32)h};
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_dev, true, &fence, 1);
    SDL_ReleaseGPUFence(g_dev, fence);
    const uint32_t *src = SDL_MapGPUTransferBuffer(g_dev, tb, false);
    for (int k = 0; k < w * h; k++) out[k] = src[k] | 0xFF000000u;
    SDL_UnmapGPUTransferBuffer(g_dev, tb);
    SDL_ReleaseGPUTransferBuffer(g_dev, tb);
    return 1;
}

#endif
