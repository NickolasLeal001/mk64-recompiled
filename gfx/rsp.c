// High-level emulation of the F3DEX/F3DLX 0.95 graphics microcode (GBI1 command set): display
// lists, segments, matrix stack, vertex transform, lighting, fog, clipping and triangle setup.
// Triangles and rectangles are handed to the software RDP (rdp.c).
#include "rsp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rdp.h"
#include "recomp.h"

static uint8_t *g_ram;

#define RD32(a) mem_r32(g_ram, 0x80000000u | ((a) & 0x00FFFFFFu))
#define RD16(a) mem_r16(g_ram, 0x80000000u | ((a) & 0x00FFFFFFu))
#define RD8(a) mem_r8(g_ram, 0x80000000u | ((a) & 0x00FFFFFFu))

// Geometry mode bits (GBI1)
#define G_ZBUFFER 0x00000001
#define G_SHADE 0x00000004
#define G_SHADING_SMOOTH 0x00000200
#define G_CULL_FRONT 0x00001000
#define G_CULL_BACK 0x00002000
#define G_FOG 0x00010000
#define G_LIGHTING 0x00020000
#define G_TEXTURE_GEN 0x00040000
#define G_TEXTURE_GEN_LINEAR 0x00080000

typedef float Mat[4][4];

typedef struct Vert {
    float cx, cy, cz, cw;  // clip space
    float r, g, b, a;
    float s, t;
} Vert;

static struct {
    uint32_t segments[16];
    Mat proj, mv[18], mvp;
    int mv_depth;
    Vert vtx[64];
    uint32_t geometry;
    int num_lights;
    float light_col[8][3], light_dir[8][3];
    float light_model[8][3];  // light directions in model space for the current modelview
    int lights_dirty;
    float lookat_x[3], lookat_y[3];
    float lookat_model[2][3];
    float vscale[3], vtrans[3];
    float fog_mul, fog_off;
    int tex_on, tex_tile;
    float tex_sscale, tex_tscale;
    float tex_sraw, tex_traw;  // G_TEXTURE scale as the raw 16-bit value (texture generation)
    int f3dex;            // 1: F3DEX vertex/triangle encoding, 0: F3D
    uint32_t rdphalf1;
} S;

static uint32_t seg_addr(uint32_t a) {
    return (S.segments[(a >> 24) & 0x0F] + (a & 0x00FFFFFFu)) & 0x00FFFFFFu;
}

static void mat_mul(Mat out, Mat a, Mat b) {
    Mat r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(out, r, sizeof r);
}

// N64 Mtx: 16 s16 integer parts followed by 16 u16 fractions, row-major.
static void load_mtx(Mat m, uint32_t addr) {
    for (int k = 0; k < 16; k++) {
        int32_t whole = (int16_t)RD16(addr + 2 * k);
        uint32_t frac = RD16(addr + 32 + 2 * k);
        m[k / 4][k % 4] = (float)((whole * 65536) | (int32_t)frac) / 65536.0f;
    }
}

static void update_mvp(void) {
    mat_mul(S.mvp, S.mv[S.mv_depth], S.proj);
    S.lights_dirty = 1;
}

static void normalize(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static void update_lights(void) {
    float (*m)[4] = S.mv[S.mv_depth];
    for (int k = 0; k < S.num_lights; k++) {
        for (int i = 0; i < 3; i++)
            S.light_model[k][i] = S.light_dir[k][0] * m[i][0] + S.light_dir[k][1] * m[i][1] + S.light_dir[k][2] * m[i][2];
        normalize(S.light_model[k]);
    }
    const float *axes[2] = {S.lookat_x, S.lookat_y};
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 3; i++)
            S.lookat_model[k][i] = axes[k][0] * m[i][0] + axes[k][1] * m[i][1] + axes[k][2] * m[i][2];
        normalize(S.lookat_model[k]);
    }
    S.lights_dirty = 0;
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void load_vertices(uint32_t addr, int first, int n) {
    if (S.lights_dirty && (S.geometry & G_LIGHTING)) update_lights();
    for (int k = 0; k < n && first + k < 64; k++) {
        uint32_t a = addr + 16 * k;
        float x = (int16_t)RD16(a), y = (int16_t)RD16(a + 2), z = (int16_t)RD16(a + 4);
        Vert *v = &S.vtx[first + k];
        v->cx = x * S.mvp[0][0] + y * S.mvp[1][0] + z * S.mvp[2][0] + S.mvp[3][0];
        v->cy = x * S.mvp[0][1] + y * S.mvp[1][1] + z * S.mvp[2][1] + S.mvp[3][1];
        v->cz = x * S.mvp[0][2] + y * S.mvp[1][2] + z * S.mvp[2][2] + S.mvp[3][2];
        v->cw = x * S.mvp[0][3] + y * S.mvp[1][3] + z * S.mvp[2][3] + S.mvp[3][3];
        float s = (int16_t)RD16(a + 8), t = (int16_t)RD16(a + 10);
        uint8_t c0 = RD8(a + 12), c1 = RD8(a + 13), c2 = RD8(a + 14), c3 = RD8(a + 15);
        if (S.geometry & G_LIGHTING) {
            float nrm[3] = {(int8_t)c0 / 127.0f, (int8_t)c1 / 127.0f, (int8_t)c2 / 127.0f};
            float r = S.light_col[S.num_lights][0], g = S.light_col[S.num_lights][1], b = S.light_col[S.num_lights][2];
            for (int l = 0; l < S.num_lights; l++) {
                float d = nrm[0] * S.light_model[l][0] + nrm[1] * S.light_model[l][1] + nrm[2] * S.light_model[l][2];
                if (d > 0) { r += d * S.light_col[l][0]; g += d * S.light_col[l][1]; b += d * S.light_col[l][2]; }
            }
            v->r = clamp01(r); v->g = clamp01(g); v->b = clamp01(b);
            if (S.geometry & G_TEXTURE_GEN) {
                // Environment mapping: the vertex normal against the lookat axes (moved into model
                // space like the lights), mapped to (d + 1) / 4 of the G_TEXTURE scale, in S10.5.
                float dx = nrm[0] * S.lookat_model[0][0] + nrm[1] * S.lookat_model[0][1] + nrm[2] * S.lookat_model[0][2];
                float dy = nrm[0] * S.lookat_model[1][0] + nrm[1] * S.lookat_model[1][1] + nrm[2] * S.lookat_model[1][2];
                dx = dx < -1 ? -1 : dx > 1 ? 1 : dx;
                dy = dy < -1 ? -1 : dy > 1 ? 1 : dy;
                if (S.geometry & G_TEXTURE_GEN_LINEAR) {
                    dx = acosf(-dx) / 4.0f;
                    dy = acosf(-dy) / 4.0f;
                } else {
                    dx = (dx + 1.0f) / 4.0f;
                    dy = (dy + 1.0f) / 4.0f;
                }
                v->s = dx * S.tex_sraw / 32.0f;
                v->t = dy * S.tex_traw / 32.0f;
            }
        } else {
            v->r = c0 / 255.0f; v->g = c1 / 255.0f; v->b = c2 / 255.0f;
        }
        v->a = c3 / 255.0f;
        // With G_FOG the shade alpha becomes the fog factor; it is computed per drawn vertex after
        // near-plane clipping (draw_triangle), since z/w of vertices behind the eye is meaningless.
        // Vertex s/t are S10.5; G_TEXTURE scale is 0.16.
        if (!((S.geometry & G_LIGHTING) && (S.geometry & G_TEXTURE_GEN))) {
            v->s = s * S.tex_sscale / 32.0f;
            v->t = t * S.tex_tscale / 32.0f;
        }
    }
}

static void to_screen(const Vert *v, RdpVertex *o) {
    float w = v->cw;
    float iw = 1.0f / w;
    o->x = (v->cx * iw) * S.vscale[0] + S.vtrans[0];
    o->y = -(v->cy * iw) * S.vscale[1] + S.vtrans[1];
    o->z = clamp01(((v->cz * iw) * S.vscale[2] + S.vtrans[2]) / 1023.0f);
    o->invw = iw;
    o->r = v->r; o->g = v->g; o->b = v->b; o->a = v->a;
    o->s = v->s; o->t = v->t;
}

static Vert lerp_vert(const Vert *a, const Vert *b, float t) {
    Vert v;
    v.cx = a->cx + (b->cx - a->cx) * t; v.cy = a->cy + (b->cy - a->cy) * t;
    v.cz = a->cz + (b->cz - a->cz) * t; v.cw = a->cw + (b->cw - a->cw) * t;
    v.r = a->r + (b->r - a->r) * t; v.g = a->g + (b->g - a->g) * t;
    v.b = a->b + (b->b - a->b) * t; v.a = a->a + (b->a - a->a) * t;
    v.s = a->s + (b->s - a->s) * t; v.t = a->t + (b->t - a->t) * t;
    return v;
}

static void draw_triangle(int i0, int i1, int i2) {
    Vert in[3] = {S.vtx[i0], S.vtx[i1], S.vtx[i2]};
    if (!(S.geometry & G_SHADING_SMOOTH)) {
        // Flat shading takes the color of the first vertex.
        for (int k = 1; k < 3; k++) { in[k].r = in[0].r; in[k].g = in[0].g; in[k].b = in[0].b; in[k].a = in[0].a; }
    }
    // Clip against the near plane (z >= -w) in clip space.
    Vert poly[8];
    int n = 0;
    for (int k = 0; k < 3; k++) {
        const Vert *a = &in[k], *b = &in[(k + 1) % 3];
        float da = a->cz + a->cw, db = b->cz + b->cw;
        if (da >= 0) poly[n++] = *a;
        if ((da >= 0) != (db >= 0)) poly[n++] = lerp_vert(a, b, da / (da - db));
    }
    if (n < 3) return;
    RdpVertex sv[8];
    for (int k = 0; k < n; k++) {
        if (poly[k].cw <= 1e-5f) poly[k].cw = 1e-5f;
        if (S.geometry & G_FOG) poly[k].a = clamp01(((poly[k].cz / poly[k].cw) * S.fog_mul + S.fog_off) / 255.0f);
        to_screen(&poly[k], &sv[k]);
    }
    // Back/front face culling on the projected polygon (N64 screen y grows downward).
    float area = 0;
    for (int k = 0; k < n; k++) {
        const RdpVertex *a = &sv[k], *b = &sv[(k + 1) % n];
        area += a->x * b->y - b->x * a->y;
    }
    if ((S.geometry & G_CULL_BACK) && area > 0) return;
    if ((S.geometry & G_CULL_FRONT) && area < 0) return;
    if (!(S.geometry & G_SHADE)) {
        for (int k = 0; k < n; k++) { sv[k].r = sv[k].g = sv[k].b = sv[k].a = 1.0f; }
    }
    for (int k = 1; k + 1 < n; k++) rdp_triangle(&sv[0], &sv[k], &sv[k + 1]);
}

static int tri_index(uint32_t b) { return S.f3dex ? (int)(b / 2) : (int)(b / 10); }

static void movemem(uint32_t w0, uint32_t w1) {
    int index = (w0 >> 16) & 0xFF;
    uint32_t a = seg_addr(w1);
    if (index == 0x80) {  // viewport: scale and translation in quarter pixels
        for (int k = 0; k < 3; k++) {
            S.vscale[k] = (int16_t)RD16(a + 2 * k) / (k < 2 ? 4.0f : 1.0f);
            S.vtrans[k] = (int16_t)RD16(a + 8 + 2 * k) / (k < 2 ? 4.0f : 1.0f);
        }
    } else if (index == 0x82 || index == 0x84) {
        float *dst = index == 0x82 ? S.lookat_y : S.lookat_x;
        for (int k = 0; k < 3; k++) dst[k] = (int8_t)RD8(a + 8 + k) / 127.0f;
        normalize(dst);
        S.lights_dirty = 1;
    } else if (index >= 0x86 && index <= 0x94) {
        int l = (index - 0x86) / 2;
        for (int k = 0; k < 3; k++) {
            S.light_col[l][k] = RD8(a + k) / 255.0f;
            S.light_dir[l][k] = (int8_t)RD8(a + 8 + k) / 127.0f;
        }
        S.lights_dirty = 1;
    }
}

static void moveword(uint32_t w0, uint32_t w1) {
    int index = w0 & 0xFF, offset = (w0 >> 8) & 0xFFFF;
    switch (index) {
    case 0x02: S.num_lights = (int)((w1 - 0x80000000u) / 32) - 1; if (S.num_lights < 0) S.num_lights = 0; if (S.num_lights > 7) S.num_lights = 7; S.lights_dirty = 1; break;
    case 0x06: S.segments[(offset / 4) & 15] = w1 & 0x00FFFFFFu; break;
    case 0x08: S.fog_mul = (int16_t)(w1 >> 16); S.fog_off = (int16_t)w1; break;
    default: break;  // clip ratio, perspective normalization, point/light color edits
    }
}

// ---------------------------------------------------------------------------------------------
// Frame interpolation. Every display list run records the matrices it loads, keyed by what they
// are used for: the next display list or vertex buffer at the same level, plus the G_MTX flags.
// A display-only rerun of a frame's list blends each of its matrices with the matching one of the
// frame before. When a key has as many matrices in both frames they pair in order; otherwise (an
// object left or entered the view, particles came and went) a matrix pairs only with a clearly
// nearest one. A jump (camera cut, respawn) keeps the current matrix.
typedef struct MtxRec { uint32_t key; float pos[3]; Mat m; int used; } MtxRec;
#define MTX_RECS 2048
typedef struct MtxFrame { MtxRec rec[MTX_RECS]; int count; } MtxFrame;
static MtxFrame g_frames[2], *g_prev = &g_frames[0], *g_cur = &g_frames[1];
static uint32_t g_run_keys[MTX_RECS];  // keys met so far by the interpolated run
static int g_run_count;
static int g_interp;
static float g_interp_t;
#define MTX_JUMP 150.0f  // world units per frame; karts move about 10

static uint32_t mtx_key(uint32_t pc, int flags) {
    for (int k = 0; k < 32; k++, pc += 8) {
        uint32_t w0 = RD32(pc), w1 = RD32(pc + 4);
        uint8_t op = (uint8_t)(w0 >> 24);
        if (op == 0x06 || op == 0x04) return seg_addr(w1) | (op == 0x04 ? 0x01000000u : 0) | (uint32_t)(flags & 7) << 28;
        if (op == 0xB8) break;
    }
    return (uint32_t)(flags & 7) << 28;
}

// World position the matrix stands for: its translation, or the eye for a camera (look-at) matrix
// multiplied onto the projection.
static void mtx_pos(Mat m, int flags, float out[3]) {
    if ((flags & 3) == 1)
        for (int j = 0; j < 3; j++) out[j] = -(m[3][0] * m[j][0] + m[3][1] * m[j][1] + m[3][2] * m[j][2]);
    else
        for (int j = 0; j < 3; j++) out[j] = m[3][j];
}

static float dist3(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int mtx_close(const MtxRec *r, const float pos[3], Mat m) {
    if (dist3(r->pos, pos) > MTX_JUMP) return 0;
    for (int i = 0; i < 3; i++)  // rotation (and scale) change under ~40 degrees
        for (int j = 0; j < 3; j++)
            if (fabsf(r->m[i][j] - m[i][j]) > 0.7f * (fabsf(m[i][0]) + fabsf(m[i][1]) + fabsf(m[i][2]) + 1e-3f)) return 0;
    return 1;
}

static int key_count(const MtxFrame *f, uint32_t key) {
    int n = 0;
    for (int k = 0; k < f->count; k++) n += f->rec[k].key == key;
    return n;
}

static MtxRec *mtx_match(uint32_t key, const float pos[3], Mat m) {
    int nth = 0;
    for (int k = 0; k < g_run_count; k++) nth += g_run_keys[k] == key;
    if (g_run_count < MTX_RECS) g_run_keys[g_run_count++] = key;
    if (key_count(g_prev, key) == key_count(g_cur, key)) {
        for (int k = 0, seen = 0; k < g_prev->count; k++) {
            MtxRec *r = &g_prev->rec[k];
            if (r->key == key && seen++ == nth) return !r->used && mtx_close(r, pos, m) ? r : NULL;
        }
        return NULL;
    }
    MtxRec *best = NULL;
    float d1 = MTX_JUMP, d2 = 1e30f;
    for (int k = 0; k < g_prev->count; k++) {
        MtxRec *r = &g_prev->rec[k];
        if (r->key != key || r->used) continue;
        float d = dist3(r->pos, pos);
        if (d < d1) { d2 = d1; d1 = d; best = r; }
        else if (d < d2) d2 = d;
    }
    return best && d1 * 2.0f < d2 && mtx_close(best, pos, m) ? best : NULL;
}

static void mtx_track(uint32_t pc, int flags, Mat m) {
    uint32_t key = mtx_key(pc, flags);
    float pos[3];
    mtx_pos(m, flags, pos);
    if (!g_interp) {
        if (g_cur->count < MTX_RECS) {
            MtxRec *r = &g_cur->rec[g_cur->count++];
            r->key = key;
            memcpy(r->pos, pos, sizeof pos);
            memcpy(r->m, m, sizeof(Mat));
        }
        return;
    }
    MtxRec *r = mtx_match(key, pos, m);
    if (!r) return;
    r->used = 1;
    float t = g_interp_t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) m[i][j] = r->m[i][j] + (m[i][j] - r->m[i][j]) * t;
}

uint64_t rsp_stats_tris;
FILE *rsp_trace;  // when set, every command of the display lists is written here

static int g_rect_tile = -1;
int rsp_current_tile(void) { return g_rect_tile >= 0 ? g_rect_tile : S.tex_tile; }
void rsp_set_rect_tile(int tile) { g_rect_tile = tile; }

static void run_list(uint32_t dl) {
    uint32_t stack[18];
    int sp = 0;
    uint32_t pc = dl & 0x00FFFFFFu;
    for (int guard = 0; guard < 2000000; guard++) {
        uint32_t w0 = RD32(pc), w1 = RD32(pc + 4);
        if (rsp_trace) fprintf(rsp_trace, "%*s%06x: %08x %08x\n", sp * 2, "", pc, w0, w1);
        pc += 8;
        uint8_t op = (uint8_t)(w0 >> 24);
        switch (op) {
        case 0x00: break;  // G_SPNOOP
        case 0x01: {       // G_MTX
            int p = (w0 >> 16) & 0xFF;
            Mat m;
            load_mtx(m, seg_addr(w1));
            mtx_track(pc, p, m);
            if (p & 0x01) {  // projection
                if (p & 0x02) memcpy(S.proj, m, sizeof m);
                else mat_mul(S.proj, m, S.proj);
            } else {
                if ((p & 0x04) && S.mv_depth < 17) {
                    memcpy(S.mv[S.mv_depth + 1], S.mv[S.mv_depth], sizeof(Mat));
                    S.mv_depth++;
                }
                if (p & 0x02) memcpy(S.mv[S.mv_depth], m, sizeof m);
                else mat_mul(S.mv[S.mv_depth], m, S.mv[S.mv_depth]);
            }
            update_mvp();
            break;
        }
        case 0x03: movemem(w0, w1); break;
        case 0x04: {  // G_VTX
            int n, first;
            if ((w0 & 0xF) == 0xF) {  // F3DEX: (v0*2)<<16 | n<<10 | (16n - 1)
                S.f3dex = 1;
                n = (w0 >> 10) & 0x3F;
                first = ((w0 >> 16) & 0xFF) / 2;
            } else {                  // F3D: (n-1)<<20 | v0<<16 | 16n
                S.f3dex = 0;
                n = ((w0 >> 20) & 0xF) + 1;
                first = (w0 >> 16) & 0xF;
            }
            load_vertices(seg_addr(w1), first, n);
            break;
        }
        case 0x06:  // G_DL (push or branch)
            if (!((w0 >> 16) & 0x01) && sp < 18) stack[sp++] = pc;
            pc = seg_addr(w1);
            break;
        case 0xAF: break;  // G_LOAD_UCODE
        case 0xB0:         // G_BRANCH_Z: branch to RDPHALF_1 if the vertex is closer than w1
            {
                const Vert *v = &S.vtx[((w0 >> 1) & 0x7FF) / 5 > 63 ? 0 : ((w0 >> 1) & 0x7FF) / 5];
                float z = v->cw != 0 ? (v->cz / v->cw) * S.vscale[2] + S.vtrans[2] : 0;
                if (z <= (float)(int32_t)w1) pc = seg_addr(S.rdphalf1);
            }
            break;
        case 0xB1:  // G_TRI2
            draw_triangle(tri_index((w0 >> 16) & 0xFF), tri_index((w0 >> 8) & 0xFF), tri_index(w0 & 0xFF));
            draw_triangle(tri_index((w1 >> 16) & 0xFF), tri_index((w1 >> 8) & 0xFF), tri_index(w1 & 0xFF));
            rsp_stats_tris += 2;
            break;
        case 0xB2: break;  // G_MODIFYVTX / RDPHALF_CONT (consumed by texrect)
        case 0xB3: break;  // G_RDPHALF_2
        case 0xB4: S.rdphalf1 = w1; break;
        case 0xB5: {       // G_LINE3D used as quad (F3DEX gSP1Quadrangle)
            int a = tri_index((w1 >> 24) & 0xFF), b = tri_index((w1 >> 16) & 0xFF);
            int c = tri_index((w1 >> 8) & 0xFF), d = tri_index(w1 & 0xFF);
            draw_triangle(a, b, c);
            draw_triangle(a, c, d);
            rsp_stats_tris += 2;
            break;
        }
        case 0xB6: S.geometry &= ~w1; break;  // G_CLEARGEOMETRYMODE
        case 0xB7: S.geometry |= w1; break;   // G_SETGEOMETRYMODE
        case 0xB8:                             // G_ENDDL
            if (sp == 0) {
                rdp_flush();
                return;
            }
            pc = stack[--sp];
            break;
        case 0xB9: case 0xBA: {  // G_SETOTHERMODE_L / _H: shift, length, data
            uint32_t hi, lo;
            rdp_get_othermode(&hi, &lo);
            int shift = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
            uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
            if (op == 0xB9) lo = (lo & ~mask) | (w1 & mask);
            else hi = (hi & ~mask) | (w1 & mask);
            rdp_set_othermode(hi, lo);
            break;
        }
        case 0xBB:  // G_TEXTURE
            S.tex_on = w0 & 0xFF;
            S.tex_tile = (w0 >> 8) & 7;
            S.tex_sscale = (w1 >> 16) / 65536.0f;
            S.tex_tscale = (w1 & 0xFFFF) / 65536.0f;
            S.tex_sraw = (float)(w1 >> 16);
            S.tex_traw = (float)(w1 & 0xFFFF);
            if (S.tex_sscale == 0) S.tex_sscale = 1.0f / 65536.0f;
            if (S.tex_tscale == 0) S.tex_tscale = 1.0f / 65536.0f;
            break;
        case 0xBC: moveword(w0, w1); break;
        case 0xBD:  // G_POPMTX
            if (S.mv_depth > 0) S.mv_depth--;
            update_mvp();
            break;
        case 0xBE: break;  // G_CULLDL: drawing everything is always correct
        case 0xBF:         // G_TRI1
            draw_triangle(tri_index((w1 >> 16) & 0xFF), tri_index((w1 >> 8) & 0xFF), tri_index(w1 & 0xFF));
            rsp_stats_tris++;
            break;
        case 0xE4: case 0xE5: {  // G_TEXRECT(FLIP): the next two commands carry s/t and ds/dt
            uint32_t st = RD32(pc + 4), dsdt = RD32(pc + 12);
            pc += 16;
            rdp_texrect(w0, w1, st, dsdt, op == 0xE5);
            break;
        }
        case 0xFD: case 0xFE: case 0xFF:  // SETTIMG/ZIMG/CIMG: the microcode resolves the segment
            rdp_command(w0, seg_addr(w1));
            break;
        default:
            if (op >= 0xC0) rdp_command(w0, w1);
            break;
        }
    }
    rdp_flush();  // display list ran past the guard without G_ENDDL
}

static uint8_t g_start[sizeof S];  // RSP state at the start of the last rsp_run_display_list
static int g_start_rect_tile;

void rsp_run_display_list(uint8_t *rdram, uint32_t dl) {
    g_ram = rdram;
    memcpy(g_start, &S, sizeof S);
    g_start_rect_tile = g_rect_tile;
    MtxFrame *f = g_prev;
    g_prev = g_cur;
    g_cur = f;
    g_cur->count = 0;
    rdp_mark_start();
    run_list(dl);
}

void rsp_run_display_list_interp(uint8_t *rdram, uint32_t dl, float t) {
    static uint8_t saved[sizeof S];
    g_ram = rdram;
    memcpy(saved, &S, sizeof S);
    memcpy(&S, g_start, sizeof S);
    int rect_tile = g_rect_tile;
    g_rect_tile = g_start_rect_tile;
    uint64_t tris = rsp_stats_tris;
    FILE *trace = rsp_trace;
    rsp_trace = NULL;
    for (int k = 0; k < g_prev->count; k++) g_prev->rec[k].used = 0;
    g_run_count = 0;
    g_interp = 1;
    g_interp_t = t;
    rdp_interp_begin();
    run_list(dl);
    rdp_interp_end();
    g_interp = 0;
    memcpy(&S, saved, sizeof S);
    g_rect_tile = rect_tile;
    rsp_stats_tris = tris;
    rsp_trace = trace;
}

void rsp_reset(void) {
    memset(&S, 0, sizeof S);
    for (int k = 0; k < 4; k++) S.proj[k][k] = S.mv[0][k][k] = S.mvp[k][k] = 1;
    S.tex_sscale = S.tex_tscale = 1.0f;
    S.vscale[0] = S.vtrans[0] = 160;
    S.vscale[1] = S.vtrans[1] = 120;
    S.vscale[2] = S.vtrans[2] = 511;
}
