// GPU renderer on SDL_gpu (Vulkan driver). See gpu.h.
//
// rdp.c records each primitive with a snapshot of the RDP state; for the display copies, every
// batch of recorded primitives becomes vertices (in pixels of the render target) plus the
// snapshots and their TMEM in storage buffers, and one ubershader (shaders/rdp.frag) does what the
// software rasterizer does per pixel. Work is queued in order (CPU-written pixels, copies to the
// in-between images, primitives) and submitted at the end of each flush.
#include "gpu.h"

#include <math.h>
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
void gpu_wait_idle(void) {}
void gpu_target_create(HiTarget *t) { t->gpu = NULL; }
void gpu_target_release(HiTarget *t) { t->gpu = NULL; }
void gpu_patch(HiTarget *t, int x0, int y0, int w, int h, const uint32_t *words) { (void)t; (void)x0; (void)y0; (void)w; (void)h; (void)words; }
void gpu_copy_interp(HiTarget *t) { (void)t; }
void gpu_draw_ops(const Op *ops, size_t count, int interp) { (void)ops; (void)count; (void)interp; }
void gpu_submit(void) {}
struct SDL_GPUTexture *gpu_target_texture(HiTarget *t, int interp) { (void)t; (void)interp; return NULL; }

#else

#include "shaders/rdp_common.glsl"

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

// Pipelines: depth test (Z_CMP) | depth write (Z_UPD) << 1, plus one for Z clears (depth only).
enum { PIPE_ZCLEAR = 4, PIPE_COUNT };
static SDL_GPUGraphicsPipeline *g_pipe[PIPE_COUNT];

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

typedef struct GpuVertex {
    float x, y, z, q;    // target pixels, depth, 1/w (1 without perspective)
    float r, g, b, a;    // shade
    float s, t;          // texels (times q)
    uint32_t state, kind;
    float p0[4], p1[4];  // per-kind parameters (see rdp.frag)
} GpuVertex;

static SDL_GPUGraphicsPipeline *make_pipeline(int index) {
    SDL_GPUVertexBufferDescription vb = {0, sizeof(GpuVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute attrs[6] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuVertex, x)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuVertex, r)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, s)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT2, offsetof(GpuVertex, state)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuVertex, p0)},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuVertex, p1)},
    };
    // The shader outputs the part of the result that does not depend on the memory color in RGB and
    // the memory color's weight in alpha: result = src + dst * src_alpha. Alpha is not written.
    SDL_GPUColorTargetDescription ct = {0};
    ct.format = COLOR_FORMAT;
    ct.blend_state.enable_blend = true;
    ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    ct.blend_state.enable_color_write_mask = true;
    ct.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
    SDL_GPUGraphicsPipelineCreateInfo pi = {0};
    pi.vertex_shader = g_vs;
    pi.fragment_shader = g_fs;
    pi.vertex_input_state.vertex_buffer_descriptions = &vb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = attrs;
    pi.vertex_input_state.num_vertex_attributes = 6;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.enable_depth_clip = false;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    pi.target_info.depth_stencil_format = g_depth_format;
    pi.target_info.has_depth_stencil_target = true;
    if (index == PIPE_ZCLEAR) {
        pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
        pi.depth_stencil_state.enable_depth_test = true;
        pi.depth_stencil_state.enable_depth_write = true;
    } else {
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        // Depth writes need the test enabled; "always" stands for writes without the test.
        pi.depth_stencil_state.enable_depth_test = index != 0;
        pi.depth_stencil_state.enable_depth_write = (index & 2) != 0;
        if (index == 2) pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    }
    return SDL_CreateGPUGraphicsPipeline(g_dev, &pi);
}

int gpu_init(SDL_Window *window, int debug) {
    g_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug != 0, "vulkan");
    if (!g_dev && debug) g_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");  // no validation layers
    if (!g_dev) return fail("SDL_CreateGPUDevice (vulkan)");
    g_depth_format = SDL_GPUTextureSupportsFormat(g_dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                                  SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                         ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT : SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    g_vs = make_shader(VS_CODE, sizeof VS_CODE, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    g_fs = make_shader(FS_CODE, sizeof FS_CODE, SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1);
    int ok = g_vs && g_fs;
    if (!ok) fail("SDL_CreateGPUShader");
    for (int k = 0; ok && k < PIPE_COUNT; k++)
        if (!(g_pipe[k] = make_pipeline(k))) ok = fail("SDL_CreateGPUGraphicsPipeline");
    if (ok && window) {
        if (!SDL_ClaimWindowForGPUDevice(g_dev, window)) {
            ok = fail("SDL_ClaimWindowForGPUDevice");
        } else {
            g_win = window;
            // No vsync, like the SDL_Renderer path: the game paces itself on the emulated retrace.
            SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
            if (SDL_WindowSupportsGPUPresentMode(g_dev, window, SDL_GPU_PRESENTMODE_IMMEDIATE)) mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
            else if (SDL_WindowSupportsGPUPresentMode(g_dev, window, SDL_GPU_PRESENTMODE_MAILBOX)) mode = SDL_GPU_PRESENTMODE_MAILBOX;
            SDL_SetGPUSwapchainParameters(g_dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
        }
    }
    if (!ok) {
        SDL_DestroyGPUDevice(g_dev);  // also releases the shaders and pipelines
        g_dev = NULL;
        return 0;
    }
    ultra_log("GPU: %s, %s", SDL_GetGPUDeviceDriver(g_dev),
              SDL_GetStringProperty(SDL_GetGPUDeviceProperties(g_dev), SDL_PROP_GPU_DEVICE_NAME_STRING, "?"));
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Render targets: color (and in-between) images per display copy, depth images per size.

struct GpuTarget {
    SDL_GPUTexture *color, *icolor;
    int w, h, ox, scale;
};

typedef struct DepthBuf { SDL_GPUTexture *tex; int w, h; uint64_t used; } DepthBuf;
#define DEPTH_BUFS 4
static DepthBuf g_depth[DEPTH_BUFS];
static uint64_t g_depth_clock;

static SDL_GPUTexture *make_texture(int w, int h, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo ti = {0};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = format;
    ti.usage = usage;
    ti.width = (Uint32)w;
    ti.height = (Uint32)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    return SDL_CreateGPUTexture(g_dev, &ti);
}

// Clears a new color image to black (like the zeroed software image) or a depth image to z = 1.
static void clear_texture(SDL_GPUTexture *tex, int depth) {
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (depth) {
        SDL_GPUDepthStencilTargetInfo dt = {0};
        dt.texture = tex;
        dt.clear_depth = DEPTH_SCALE;  // z = 1
        dt.load_op = SDL_GPU_LOADOP_CLEAR;
        dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, NULL, 0, &dt));
    } else {
        SDL_GPUColorTargetInfo ct = {0};
        ct.texture = tex;
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        ct.clear_color.a = 1.0f;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &ct, 1, NULL));
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

static DepthBuf *depth_for(int w, int h) {
    DepthBuf *d = &g_depth[0];
    for (int k = 0; k < DEPTH_BUFS; k++) {
        if (g_depth[k].tex && g_depth[k].w == w && g_depth[k].h == h) {
            g_depth[k].used = ++g_depth_clock;
            return &g_depth[k];
        }
        if (g_depth[k].used < d->used) d = &g_depth[k];
    }
    // Queued work finds its depth image by size when submitted, and SDL keeps a released texture
    // alive while recorded commands use it.
    if (d->tex) SDL_ReleaseGPUTexture(g_dev, d->tex);
    d->tex = make_texture(w, h, g_depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    d->w = w;
    d->h = h;
    d->used = ++g_depth_clock;
    clear_texture(d->tex, 1);
    return d;
}

void gpu_target_create(HiTarget *t) {
    GpuTarget *g = calloc(1, sizeof *g);
    g->scale = rdp_get_scale();
    g->w = t->stride;
    g->h = 240 * g->scale;
    g->ox = t->ox;
    g->color = make_texture(g->w, g->h, COLOR_FORMAT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    clear_texture(g->color, 0);
    t->gpu = g;
}

void gpu_target_release(HiTarget *t) {
    if (!t->gpu) return;
    gpu_submit();
    SDL_ReleaseGPUTexture(g_dev, t->gpu->color);
    if (t->gpu->icolor) SDL_ReleaseGPUTexture(g_dev, t->gpu->icolor);
    free(t->gpu);
    t->gpu = NULL;
}

struct SDL_GPUTexture *gpu_target_texture(HiTarget *t, int interp) {
    return t->gpu ? (interp ? t->gpu->icolor : t->gpu->color) : NULL;
}

// ---------------------------------------------------------------------------------------------
// Queued work, in order: batches of vertices drawn into one image with one pipeline, copies of a
// display copy into its in-between image, and Z clears.

enum { B_DRAW, B_COPY, B_ZCLEAR };
typedef struct Batch {
    int type, pipe, icolor;
    GpuTarget *t;
    int dw, dh;  // size of the depth image (B_ZCLEAR)
    uint32_t first, count;
} Batch;

#define GROW(arr, count, cap, extra)                                         \
    do {                                                                     \
        if ((count) + (extra) > (cap)) {                                     \
            (cap) = ((count) + (extra)) * 2;                                 \
            (arr) = realloc((arr), (cap) * sizeof *(arr));                   \
        }                                                                    \
    } while (0)

static GpuVertex *g_v;
static size_t g_vn, g_vcap;
static uint32_t *g_st;  // state words
static size_t g_stn, g_stcap;
static uint32_t *g_tm;  // TMEM copies, 1024 words each
static size_t g_tmn, g_tmcap;
static uint32_t *g_pw;  // CPU-written pixels
static size_t g_pwn, g_pwcap;
static Batch *g_b;
static size_t g_bn, g_bcap;

// TMEM copies of the queued work, by content hash.
#define TMEM_SLOTS 1024
static struct { uint64_t hash; uint32_t base; } g_tmem_index[TMEM_SLOTS];
static size_t g_tmem_used;

static void add_batch(int type, GpuTarget *t, int icolor, int pipe, int dw, int dh, uint32_t verts) {
    Batch *last = g_bn ? &g_b[g_bn - 1] : NULL;
    if (type == B_DRAW && last && last->type == B_DRAW && last->t == t && last->icolor == icolor && last->pipe == pipe &&
        last->first + last->count == (uint32_t)g_vn - verts) {
        last->count += verts;
        return;
    }
    GROW(g_b, g_bn, g_bcap, 1);
    Batch *b = &g_b[g_bn++];
    b->type = type;
    b->t = t;
    b->icolor = icolor;
    b->pipe = pipe;
    b->dw = dw;
    b->dh = dh;
    b->first = (uint32_t)g_vn - verts;
    b->count = verts;
}

static uint32_t tmem_copy(const uint8_t *tmem) {
    uint64_t h = 1469598103934665603ull;
    const uint64_t *q = (const uint64_t *)tmem;
    for (int k = 0; k < 512; k++) h = (h ^ q[k]) * 1099511628211ull;
    size_t slot = (size_t)(h % TMEM_SLOTS);
    for (size_t n = 0; n < TMEM_SLOTS; n++, slot = (slot + 1) % TMEM_SLOTS) {
        if (g_tmem_index[slot].hash == h && g_tmem_index[slot].base != 0xFFFFFFFFu &&
            !memcmp(&g_tm[g_tmem_index[slot].base], tmem, 4096))
            return g_tmem_index[slot].base;
        if (g_tmem_index[slot].base == 0xFFFFFFFFu) break;
    }
    GROW(g_tm, g_tmn, g_tmcap, 1024);
    uint32_t base = (uint32_t)g_tmn;
    memcpy(&g_tm[base], tmem, 4096);
    g_tmn += 1024;
    if (g_tmem_used < TMEM_SLOTS / 2) {
        g_tmem_index[slot].hash = h;
        g_tmem_index[slot].base = base;
        g_tmem_used++;
    }
    return base;
}

static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// Columns of a span of 1x pixels in the enlarged image; a span covering the whole 1x width covers
// the whole widened row (as span_cols in rdp.c).
static void span_cols(const RdpState *st, int S, int ox, int ux0, int ux1, int *x0, int *x1) {
    *x0 = ux0 * S + ox;
    *x1 = ux1 * S + ox;
    if (ox && ux0 <= 0 && ux1 >= (int)st->cimg_width) { *x0 = 0; *x1 = (int)st->cimg_width * S + 2 * ox; }
}

static const RdpState *g_last_st;
static int g_last_ox;
static uint32_t g_last_index;

// Blender modes the GPU only approximates: the memory alpha (G_BL_A_MEM) as a weight is taken as
// 1. Each such othermode is logged once.
static void note_blender(const RdpState *st) {
    static uint32_t seen[32];
    static int nseen;
    int two = OM_CYCLE_TYPE(st) == CYC_2, force = (st->om_lo >> 14) & 1;
    int b0 = (st->om_lo >> 18) & 3, b1 = (st->om_lo >> 16) & 3;
    int mem_alpha = two ? (b0 == 1 || (force && b1 == 1)) : (force && b0 == 1);
    if (!mem_alpha || OM_CYCLE_TYPE(st) >= CYC_COPY) return;
    for (int k = 0; k < nseen; k++)
        if (seen[k] == st->om_lo) return;
    if (nseen < 32) seen[nseen++] = st->om_lo;
    ultra_log("GPU: blender com alfa da memoria (othermode %06x %08x), aproximado por 1", st->om_hi, st->om_lo);
}

static uint32_t add_state(const RdpState *st, int S, int ox) {
    if (st == g_last_st && ox == g_last_ox) return g_last_index;
    note_blender(st);
    GROW(g_st, g_stn, g_stcap, STATE_WORDS);
    uint32_t *w = &g_st[g_stn];
    memset(w, 0, STATE_WORDS * 4);
    w[ST_OM_HI] = st->om_hi;
    w[ST_OM_LO] = st->om_lo;
    w[ST_COMB_HI] = st->combine_hi;
    w[ST_COMB_LO] = st->combine_lo;
    w[ST_FILL] = st->fill;
    w[ST_TILE] = (uint32_t)st->tile;
    w[ST_FLAGS] = (st->need_tex0 ? 1u : 0u) | (st->need_tex1 ? 2u : 0u);
    const Color *cols[4] = {&st->prim, &st->env, &st->blend, &st->fog};
    for (int k = 0; k < 4; k++) {
        w[ST_PRIM + 4 * k] = fbits(cols[k]->r);
        w[ST_PRIM + 4 * k + 1] = fbits(cols[k]->g);
        w[ST_PRIM + 4 * k + 2] = fbits(cols[k]->b);
        w[ST_PRIM + 4 * k + 3] = fbits(cols[k]->a);
    }
    w[ST_PRIM_LOD] = fbits(st->prim_lod);
    w[ST_PRIM_DEPTH] = fbits(st->prim_depth);
    int x0, x1;
    span_cols(st, S, ox, st->sc_x0, st->sc_x1, &x0, &x1);
    w[ST_SCISSOR] = (uint32_t)(x0 < 0 ? 0 : x0);
    w[ST_SCISSOR + 1] = (uint32_t)(st->sc_y0 * S);
    w[ST_SCISSOR + 2] = (uint32_t)(x1 < 0 ? 0 : x1);
    w[ST_SCISSOR + 3] = (uint32_t)(st->sc_y1 * S);
    for (int k = 0; k < 8; k++) {
        const Tile *t = &st->tiles[k];
        uint32_t *tw = &w[ST_TILES + 8 * k];
        tw[0] = (uint32_t)(t->fmt & 7) | (uint32_t)(t->siz & 3) << 3 | (uint32_t)(t->palette & 15) << 5 |
                (uint32_t)(t->cms & 3) << 9 | (uint32_t)(t->cmt & 3) << 11 | (uint32_t)(t->masks & 15) << 13 |
                (uint32_t)(t->maskt & 15) << 17 | (uint32_t)(t->shifts & 15) << 21 | (uint32_t)(t->shiftt & 15) << 25;
        tw[1] = (uint32_t)(t->line & 0x1FF) | (uint32_t)(t->tmem & 0x1FF) << 9;
        tw[2] = fbits(t->uls);
        tw[3] = fbits(t->ult);
        tw[4] = fbits(t->lrs);
        tw[5] = fbits(t->lrt);
    }
    // Only states that sample textures need their TMEM.
    w[ST_TMEM] = st->need_tex0 || st->need_tex1 || OM_CYCLE_TYPE(st) == CYC_COPY ? tmem_copy(st->tmem) : 0;
    g_last_st = st;
    g_last_ox = ox;
    g_last_index = (uint32_t)(g_stn / STATE_WORDS);
    g_stn += STATE_WORDS;
    return g_last_index;
}

static GpuVertex *add_verts(int n) {
    GROW(g_v, g_vn, g_vcap, (size_t)n);
    GpuVertex *v = &g_v[g_vn];
    memset(v, 0, sizeof *v * (size_t)n);
    g_vn += (size_t)n;
    return v;
}

// An axis-aligned rectangle [x0, x1) x [y0, y1) of target pixels as two triangles.
static GpuVertex *add_quad(float x0, float y0, float x1, float y1, uint32_t state, uint32_t kind) {
    GpuVertex *v = add_verts(6);
    const float xs[6] = {x0, x1, x0, x1, x1, x0}, ys[6] = {y0, y0, y1, y0, y1, y1};
    for (int k = 0; k < 6; k++) {
        v[k].x = xs[k];
        v[k].y = ys[k];
        v[k].q = 1.0f;
        v[k].state = state;
        v[k].kind = kind;
    }
    return v;
}

static int zpipe(const RdpState *st) { return (int)((st->om_lo >> 4) & 3); }  // Z_CMP | Z_UPD << 1

void gpu_patch(HiTarget *t, int x0, int y0, int w, int h, const uint32_t *words) {
    if (!t->gpu || w <= 0 || h <= 0) return;
    int S = t->gpu->scale, ox = t->ox;
    GROW(g_pw, g_pwn, g_pwcap, (size_t)w * h);
    uint32_t offset = (uint32_t)g_pwn;
    memcpy(&g_pw[g_pwn], words, sizeof(uint32_t) * (size_t)w * h);
    g_pwn += (size_t)w * h;
    GpuVertex *v = add_quad((float)(x0 * S + ox), (float)(y0 * S), (float)((x0 + w) * S + ox), (float)((y0 + h) * S), 0, K_PATCH);
    for (int k = 0; k < 6; k++) {
        v[k].p0[0] = (float)offset;
        v[k].p0[1] = (float)w;
        v[k].p0[2] = (float)x0;
        v[k].p0[3] = (float)y0;
    }
    add_batch(B_DRAW, t->gpu, 0, 0, 0, 0, 6);
}

void gpu_copy_interp(HiTarget *t) {
    GpuTarget *g = t->gpu;
    if (!g) return;
    if (!g->icolor) g->icolor = make_texture(g->w, g->h, COLOR_FORMAT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    add_batch(B_COPY, g, 1, 0, 0, 0, 0);
}

static void draw_tri(const Op *op, GpuTarget *g, int icolor, int S) {
    const RdpState *st = op->st;
    const RdpVertex *u = op->u.v;
    int ox = g->ox;
    // A flat (orthographic, w = 1) triangle spanning the whole width, like the sky gradient, is
    // stretched to the widened edges; everything else keeps its place, shifted by the margin.
    float wd = (float)st->cimg_width;
    int stretch = ox && u[0].invw == 1.0f && u[1].invw == 1.0f && u[2].invw == 1.0f &&
                  fminf(u[0].x, fminf(u[1].x, u[2].x)) <= 1.0f && fmaxf(u[0].x, fmaxf(u[1].x, u[2].x)) >= wd - 1.0f;
    uint32_t state = add_state(st, S, ox);
    int persp = (st->om_hi >> 19) & 1;
    GpuVertex *v = add_verts(3);
    for (int k = 0; k < 3; k++) {
        float x = u[k].x * (float)S + (float)ox;
        if (stretch && u[k].x <= 1.0f) x = 0.0f;
        if (stretch && u[k].x >= wd - 1.0f) x = (float)(st->cimg_width * (uint32_t)S + 2u * (uint32_t)ox);
        v[k].x = x;
        v[k].y = u[k].y * (float)S;
        v[k].z = u[k].z;
        v[k].q = persp ? u[k].invw : 1.0f;
        v[k].r = u[k].r;
        v[k].g = u[k].g;
        v[k].b = u[k].b;
        v[k].a = u[k].a;
        v[k].s = u[k].s * v[k].q;
        v[k].t = u[k].t * v[k].q;
        v[k].state = state;
        v[k].kind = K_TRI;
    }
    add_batch(B_DRAW, g, icolor, zpipe(st), 0, 0, 3);
}

static void draw_texrect(const Op *op, GpuTarget *g, int icolor, int S) {
    const RdpState *st = op->st;
    int ox = g->ox;
    float ulx = op->u.tr.ulx, uly = op->u.tr.uly;
    int x0 = (int)(ulx * (float)S) + ox, y0 = (int)(uly * (float)S);
    int x1 = (int)(op->u.tr.lrx * (float)S) + ox, y1 = (int)(op->u.tr.lry * (float)S);
    int sx0, sx1;
    span_cols(st, S, ox, st->sc_x0, st->sc_x1, &sx0, &sx1);
    if (x0 < sx0) x0 = sx0;
    if (x1 > sx1) x1 = sx1;
    if (y0 < st->sc_y0 * S) y0 = st->sc_y0 * S;
    if (y1 > st->sc_y1 * S) y1 = st->sc_y1 * S;
    if (y1 > g->h) y1 = g->h;
    if (y0 >= y1) return;
    // A full-width picture (title, menus) gets black bars in the widened margins.
    if (ox && ulx <= 0.0f && op->u.tr.lrx >= (float)st->cimg_width) {
        add_quad(0.0f, (float)y0, (float)ox, (float)y1, 0, K_SOLID);
        add_quad((float)(g->w - ox), (float)y0, (float)g->w, (float)y1, 0, K_SOLID);
        add_batch(B_DRAW, g, icolor, 0, 0, 0, 12);
    }
    if (x0 >= x1) return;
    uint32_t state = add_state(st, S, ox);
    GpuVertex *v = add_quad((float)x0, (float)y0, (float)x1, (float)y1, state, K_TEXRECT | (op->u.tr.flip ? K_FLIP : 0u));
    for (int k = 0; k < 6; k++) {
        v[k].p0[0] = op->u.tr.ulx;
        v[k].p0[1] = op->u.tr.uly;
        v[k].p0[2] = op->u.tr.lrx;
        v[k].p0[3] = op->u.tr.lry;
        v[k].p1[0] = op->u.tr.s0;
        v[k].p1[1] = op->u.tr.t0;
        v[k].p1[2] = op->u.tr.dsdx;
        v[k].p1[3] = op->u.tr.dtdy;
    }
    add_batch(B_DRAW, g, icolor, OM_CYCLE_TYPE(st) == CYC_COPY ? 0 : zpipe(st), 0, 0, 6);
}

// Fill and FILLRECT rectangles, clipped like draw_fill / draw_rect in rdp.c.
static int rect_cols(const Op *op, int S, int ox, int h, int *x0, int *y0, int *x1, int *y1) {
    const RdpState *st = op->st;
    int sx0, sx1;
    span_cols(st, S, ox, op->u.rc.x0, op->u.rc.x1, x0, x1);
    span_cols(st, S, ox, st->sc_x0, st->sc_x1, &sx0, &sx1);
    if (*x0 < sx0) *x0 = sx0;
    if (*x1 > sx1) *x1 = sx1;
    *y0 = op->u.rc.y0 * S;
    *y1 = op->u.rc.y1 * S;
    if (*y0 < st->sc_y0 * S) *y0 = st->sc_y0 * S;
    if (*y1 > st->sc_y1 * S) *y1 = st->sc_y1 * S;
    if (*y1 > h) *y1 = h;
    return *x0 < *x1 && *y0 < *y1;
}

void gpu_draw_ops(const Op *ops, size_t count, int interp) {
    int S = rdp_get_scale();
    g_last_st = NULL;  // snapshots are freed after each flush and their addresses reused
    for (size_t k = 0; k < count; k++) {
        const Op *op = &ops[k];
        const RdpState *st = op->st;
        GpuTarget *g = op->hi ? op->hi->gpu : NULL;
        if (!g) {
            // Fills of the Z image clear the Z buffer of that width.
            if (op->kind != OP_FILL || st->cimg != st->zimg) continue;
            int ox = rdp_hi_ox((int)st->cimg_width), w = (int)st->cimg_width * S + 2 * ox;
            int x0, y0, x1, y1;
            if (!rect_cols(op, S, ox, 240 * S, &x0, &y0, &x1, &y1)) continue;
            add_quad((float)x0, (float)y0, (float)x1, (float)y1, 0, K_ZCLEAR);
            add_batch(B_ZCLEAR, NULL, 0, PIPE_ZCLEAR, w, 240 * S, 6);
            continue;
        }
        int x0, y0, x1, y1;
        switch (op->kind) {
        case OP_TRI: draw_tri(op, g, interp, S); break;
        case OP_TEXRECT: draw_texrect(op, g, interp, S); break;
        case OP_FILL:
            if (!rect_cols(op, S, g->ox, g->h, &x0, &y0, &x1, &y1)) break;
            add_quad((float)x0, (float)y0, (float)x1, (float)y1, add_state(st, S, g->ox), K_FILL);
            add_batch(B_DRAW, g, interp, 0, 0, 0, 6);
            break;
        default:
            if (!rect_cols(op, S, g->ox, g->h, &x0, &y0, &x1, &y1)) break;
            add_quad((float)x0, (float)y0, (float)x1, (float)y1, add_state(st, S, g->ox), K_RECT);
            add_batch(B_DRAW, g, interp, zpipe(st), 0, 0, 6);
            break;
        }
    }
}

// GPU buffers of the queued work, grown as needed.
typedef struct GrowBuffer { SDL_GPUBuffer *buf; uint32_t cap; } GrowBuffer;
static GrowBuffer g_vbuf, g_sbuf, g_tbuf, g_pbuf;
static SDL_GPUTransferBuffer *g_xfer;
static uint32_t g_xfer_cap;

static void ensure_buffer(GrowBuffer *b, uint32_t size, SDL_GPUBufferUsageFlags usage) {
    if (size < 64) size = 64;
    if (b->buf && b->cap >= size) return;
    if (b->buf) SDL_ReleaseGPUBuffer(g_dev, b->buf);
    SDL_GPUBufferCreateInfo bi = {usage, size * 2, 0};
    b->buf = SDL_CreateGPUBuffer(g_dev, &bi);
    b->cap = size * 2;
}

static void reset_queue(void) {
    g_vn = g_stn = g_tmn = g_pwn = g_bn = 0;
    memset(g_tmem_index, 0xFF, sizeof g_tmem_index);
    g_tmem_used = 0;
    g_last_st = NULL;
}

void gpu_submit(void) {
    if (!g_bn) {
        reset_queue();
        return;
    }
    uint32_t sizes[4] = {(uint32_t)(g_vn * sizeof(GpuVertex)), (uint32_t)(g_stn * 4), (uint32_t)(g_tmn * 4), (uint32_t)(g_pwn * 4)};
    const void *data[4] = {g_v, g_st, g_tm, g_pw};
    GrowBuffer *bufs[4] = {&g_vbuf, &g_sbuf, &g_tbuf, &g_pbuf};
    uint32_t total = 0;
    for (int k = 0; k < 4; k++) {
        ensure_buffer(bufs[k], sizes[k], k == 0 ? SDL_GPU_BUFFERUSAGE_VERTEX : SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        total += sizes[k];
    }
    if (!g_xfer || g_xfer_cap < total) {
        if (g_xfer) SDL_ReleaseGPUTransferBuffer(g_dev, g_xfer);
        g_xfer_cap = total * 2;
        SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, g_xfer_cap, 0};
        g_xfer = SDL_CreateGPUTransferBuffer(g_dev, &ti);
    }
    uint8_t *map = SDL_MapGPUTransferBuffer(g_dev, g_xfer, true);
    uint32_t off = 0;
    for (int k = 0; k < 4; k++) {
        if (sizes[k]) memcpy(map + off, data[k], sizes[k]);
        off += sizes[k];
    }
    SDL_UnmapGPUTransferBuffer(g_dev, g_xfer);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    off = 0;
    for (int k = 0; k < 4; k++) {
        if (sizes[k]) {
            SDL_GPUTransferBufferLocation src = {g_xfer, off};
            SDL_GPUBufferRegion dst = {bufs[k]->buf, 0, sizes[k]};
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
        }
        off += sizes[k];
    }
    SDL_EndGPUCopyPass(copy);

    SDL_GPURenderPass *pass = NULL;
    GpuTarget *pass_t = NULL;
    int pass_icolor = 0, pass_pipe = -1;
    DepthBuf *pass_depth = NULL;
    for (size_t k = 0; k < g_bn; k++) {
        Batch *b = &g_b[k];
        if (b->type == B_COPY) {
            if (pass) SDL_EndGPURenderPass(pass);
            pass = NULL;
            SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureLocation src = {b->t->color, 0, 0, 0, 0, 0}, dst = {b->t->icolor, 0, 0, 0, 0, 0};
            SDL_CopyGPUTextureToTexture(cp, &src, &dst, (Uint32)b->t->w, (Uint32)b->t->h, 1, false);
            SDL_EndGPUCopyPass(cp);
            continue;
        }
        DepthBuf *depth = b->type == B_ZCLEAR ? depth_for(b->dw, b->dh) : depth_for(b->t->w, b->t->h);
        if (!pass || pass_t != b->t || pass_icolor != b->icolor || pass_depth != depth) {
            if (pass) SDL_EndGPURenderPass(pass);
            SDL_GPUColorTargetInfo ct = {0};
            SDL_GPUDepthStencilTargetInfo dt = {0};
            dt.texture = depth->tex;
            dt.load_op = SDL_GPU_LOADOP_LOAD;
            dt.store_op = SDL_GPU_STOREOP_STORE;
            dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            if (b->t) {
                ct.texture = b->icolor ? b->t->icolor : b->t->color;
                ct.load_op = SDL_GPU_LOADOP_LOAD;
                ct.store_op = SDL_GPU_STOREOP_STORE;
            }
            pass = SDL_BeginGPURenderPass(cmd, b->t ? &ct : NULL, b->t ? 1 : 0, &dt);
            pass_t = b->t;
            pass_icolor = b->icolor;
            pass_depth = depth;
            pass_pipe = -1;
            SDL_GPUBufferBinding vb = {g_vbuf.buf, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            SDL_GPUBuffer *storage[3] = {g_tbuf.buf, g_sbuf.buf, g_pbuf.buf};
            SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 3);
            float size[4] = {(float)depth->w, (float)depth->h, (float)rdp_get_scale(), b->t ? (float)b->t->ox : 0.0f};
            SDL_PushGPUVertexUniformData(cmd, 0, size, sizeof size);
            SDL_PushGPUFragmentUniformData(cmd, 0, size, sizeof size);
        }
        if (pass_pipe != b->pipe) {
            SDL_BindGPUGraphicsPipeline(pass, g_pipe[b->pipe]);
            pass_pipe = b->pipe;
        }
        SDL_DrawGPUPrimitives(pass, b->count, 1, b->first, 0);
    }
    if (pass) SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    reset_queue();
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
        u->tex = make_texture(w, h, COLOR_FORMAT, SDL_GPU_TEXTUREUSAGE_SAMPLER);
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

void gpu_wait_idle(void) { SDL_WaitForGPUIdle(g_dev); }

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
