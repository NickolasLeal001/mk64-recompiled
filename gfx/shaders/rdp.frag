#version 450
// The RDP pixel pipeline for the display copies: the same texture fetch, combiner and blender as
// the software renderer (rdp.c), reading the snapshot of the primitive's RDP state.
#extension GL_GOOGLE_include_directive : require
#include "rdp_common.glsl"

layout(location = 0) noperspective in vec4 v_shade;
layout(location = 1) noperspective in vec3 v_stq;
layout(location = 2) noperspective in float v_z;
layout(location = 3) flat in uvec2 v_ids;
layout(location = 4) flat in vec4 v_p0;
layout(location = 5) flat in vec4 v_p1;

layout(location = 0) out vec4 o_color;

layout(std430, set = 2, binding = 0) readonly buffer Tmem { uint tm[]; };     // 1024 words per TMEM copy
layout(std430, set = 2, binding = 1) readonly buffer States { uint sw[]; };   // STATE_WORDS per state
layout(std430, set = 2, binding = 2) readonly buffer Patch { uint pw[]; };    // CPU-written pixels
layout(set = 3, binding = 0) uniform Target { vec4 u_size; };  // width, height, scale, side margin

uint S0;          // first word of the state
uint TB;          // first word of its TMEM copy
uint OM_HI, OM_LO;

float wf(uint k) { return uintBitsToFloat(sw[S0 + k]); }
vec4 wcolor(uint k) { return vec4(wf(k), wf(k + 1u), wf(k + 2u), wf(k + 3u)); }

uint tbyte(uint a) {
    a &= 0xFFFu;
    return (tm[TB + (a >> 2)] >> ((a & 3u) * 8u)) & 0xFFu;
}

vec4 rgba16(uint c) {
    return vec4(float((c >> 11) & 31u) / 31.0, float((c >> 6) & 31u) / 31.0, float((c >> 1) & 31u) / 31.0, float(c & 1u));
}

uint tlut_type() { return (OM_HI >> 14) & 3u; }

vec4 tlut_color(uint index) {
    uint a = 0x800u + index * 8u;
    uint c = tbyte(a) << 8 | tbyte(a + 1u);
    if (tlut_type() == 3u) {  // IA16
        float i = float(c >> 8) / 255.0;
        return vec4(i, i, i, float(c & 0xFFu) / 255.0);
    }
    return rgba16(c);
}

struct Tile { uint fmt, siz, line, tmem, palette, cms, cmt, masks, maskt, shifts, shiftt; float uls, ult, lrs, lrt; };

Tile get_tile(uint i) {
    uint b = S0 + ST_TILES + (i & 7u) * 8u;
    uint w0 = sw[b], w1 = sw[b + 1u];
    Tile t;
    t.fmt = w0 & 7u; t.siz = (w0 >> 3) & 3u; t.palette = (w0 >> 5) & 15u;
    t.cms = (w0 >> 9) & 3u; t.cmt = (w0 >> 11) & 3u; t.masks = (w0 >> 13) & 15u; t.maskt = (w0 >> 17) & 15u;
    t.shifts = (w0 >> 21) & 15u; t.shiftt = (w0 >> 25) & 15u;
    t.line = w1 & 0x1FFu; t.tmem = (w1 >> 9) & 0x1FFu;
    t.uls = uintBitsToFloat(sw[b + 2u]); t.ult = uintBitsToFloat(sw[b + 3u]);
    t.lrs = uintBitsToFloat(sw[b + 4u]); t.lrt = uintBitsToFloat(sw[b + 5u]);
    return t;
}

vec4 fetch_texel(Tile t, int s, int tt) {
    uint row = t.tmem * 8u + uint(tt) * t.line * 8u;
    uint odd = (tt & 1) != 0 ? 4u : 0u;
    uint us = uint(s);
    if (t.siz == SIZ_4) {
        uint b = tbyte((row + (us >> 1)) ^ odd);
        uint v = (us & 1u) != 0u ? (b & 15u) : (b >> 4);
        if (t.fmt == FMT_CI || tlut_type() >= 2u) return tlut_color((t.palette << 4) | v);
        if (t.fmt == FMT_IA) {
            float i = float(v >> 1) / 7.0;
            return vec4(i, i, i, float(v & 1u));
        }
        return vec4(float(v) / 15.0);
    }
    if (t.siz == SIZ_8) {
        uint b = tbyte((row + us) ^ odd);
        if (t.fmt == FMT_CI || tlut_type() >= 2u) return tlut_color(b);
        if (t.fmt == FMT_IA) {
            float i = float(b >> 4) / 15.0;
            return vec4(i, i, i, float(b & 15u) / 15.0);
        }
        return vec4(float(b) / 255.0);
    }
    if (t.siz == SIZ_16) {
        uint a = (row + us * 2u) ^ odd;
        uint c = tbyte(a) << 8 | tbyte(a + 1u);
        if (t.fmt == FMT_IA) {
            float i = float(c >> 8) / 255.0;
            return vec4(i, i, i, float(c & 0xFFu) / 255.0);
        }
        if (t.fmt == FMT_CI) return tlut_color(c >> 8);
        return rgba16(c);
    }
    // 32-bit RGBA: RG in low TMEM, BA in high TMEM
    uint a = (t.tmem * 8u + uint(tt) * t.line * 8u + us * 2u) ^ odd;
    return vec4(float(tbyte(a)), float(tbyte(a + 1u)), float(tbyte(a + 2048u)), float(tbyte(a + 2049u))) / 255.0;
}

int wrap_coord(int c, uint mask, uint cm, int maxc) {
    bool clampc = (cm & 2u) != 0u || mask == 0u;
    if (clampc) c = c < 0 ? 0 : c > maxc ? maxc : c;
    if (mask != 0u) {
        int size = 1 << mask;
        if ((cm & 1u) != 0u) {  // mirror
            int m = c & ((size << 1) - 1);
            c = m >= size ? (size << 1) - 1 - m : m;
        } else {
            c &= size - 1;
        }
    }
    return c;
}

float shift_coord(float c, uint shift) {
    if (shift == 0u) return c;
    if (shift < 11u) return c / float(1 << shift);
    return c * float(1 << (16u - shift));
}

uint cycle_type() { return (OM_HI >> 20) & 3u; }

vec4 sample_tile(uint ti, float s, float t) {
    Tile tl = get_tile(ti);
    s = shift_coord(s, tl.shifts) - tl.uls;
    t = shift_coord(t, tl.shiftt) - tl.ult;
    int maxs = int(tl.lrs - tl.uls), maxt = int(tl.lrt - tl.ult);
    if (((OM_HI >> 12) & 3u) == 0u || cycle_type() == CYC_COPY) {
        int is = wrap_coord(int(floor(s)), tl.masks, tl.cms, maxs);
        int it = wrap_coord(int(floor(t)), tl.maskt, tl.cmt, maxt);
        return fetch_texel(tl, is, it);
    }
    // Like the RDP, the filter starts at the texel the coordinate falls on (no half-texel shift).
    float fs = floor(s), ft = floor(t);
    float a = s - fs, b = t - ft;
    int s0 = int(fs), t0 = int(ft);
    int s0w = wrap_coord(s0, tl.masks, tl.cms, maxs), s1w = wrap_coord(s0 + 1, tl.masks, tl.cms, maxs);
    int t0w = wrap_coord(t0, tl.maskt, tl.cmt, maxt), t1w = wrap_coord(t0 + 1, tl.maskt, tl.cmt, maxt);
    vec4 c00 = fetch_texel(tl, s0w, t0w), c10 = fetch_texel(tl, s1w, t0w);
    vec4 c01 = fetch_texel(tl, s0w, t1w), c11 = fetch_texel(tl, s1w, t1w);
    return (c00 * (1.0 - a) + c10 * a) * (1.0 - b) + (c01 * (1.0 - a) + c11 * a) * b;
}

// Color combiner: (A - B) * C + D per cycle, separately for color and alpha.
vec4 g_comb, g_t0, g_t1, g_shade, g_prim, g_env;
float g_lod;

vec3 sel_rgb(uint which, uint sel) {
    switch (sel) {
    case 0u: return g_comb.rgb;
    case 1u: return g_t0.rgb;
    case 2u: return g_t1.rgb;
    case 3u: return g_prim.rgb;
    case 4u: return g_shade.rgb;
    case 5u: return g_env.rgb;
    }
    if (which == 2u) {  // C
        switch (sel) {
        case 7u: return vec3(g_comb.a);
        case 8u: return vec3(g_t0.a);
        case 9u: return vec3(g_t1.a);
        case 10u: return vec3(g_prim.a);
        case 11u: return vec3(g_shade.a);
        case 12u: return vec3(g_env.a);
        case 13u: return vec3(1.0);
        case 14u: return vec3(g_lod);
        }
        return vec3(0.0);
    }
    return which != 1u && sel == 6u ? vec3(1.0) : vec3(0.0);  // A and D: 6 = 1
}

float sel_alpha(uint which, uint sel) {
    if (which == 2u) {  // C
        switch (sel) {
        case 0u: return 1.0;
        case 1u: return g_t0.a;
        case 2u: return g_t1.a;
        case 3u: return g_prim.a;
        case 4u: return g_shade.a;
        case 5u: return g_env.a;
        case 6u: return g_lod;
        }
        return 0.0;
    }
    switch (sel) {
    case 0u: return g_comb.a;
    case 1u: return g_t0.a;
    case 2u: return g_t1.a;
    case 3u: return g_prim.a;
    case 4u: return g_shade.a;
    case 5u: return g_env.a;
    case 6u: return 1.0;
    }
    return 0.0;
}

vec4 combine_cycle(uint cycle) {
    uint h = sw[S0 + ST_COMB_HI], l = sw[S0 + ST_COMB_LO];
    uint a, b, c, d, aa, ab, ac, ad;
    if (cycle == 0u) {
        a = (h >> 20) & 15u; c = (h >> 15) & 31u; aa = (h >> 12) & 7u; ac = (h >> 9) & 7u;
        b = (l >> 28) & 15u; d = (l >> 15) & 7u; ab = (l >> 12) & 7u; ad = (l >> 9) & 7u;
    } else {
        a = (h >> 5) & 15u; c = h & 31u; aa = (l >> 21) & 7u; ac = (l >> 18) & 7u;
        b = (l >> 24) & 15u; d = (l >> 6) & 7u; ab = (l >> 3) & 7u; ad = l & 7u;
    }
    vec3 rgb = clamp((sel_rgb(0u, a) - sel_rgb(1u, b)) * sel_rgb(2u, c) + sel_rgb(3u, d), 0.0, 1.0);
    float al = clamp((sel_alpha(0u, aa) - sel_alpha(1u, ab)) * sel_alpha(2u, ac) + sel_alpha(3u, ad), 0.0, 1.0);
    return vec4(rgb, al);
}

vec4 shade_pixel(vec4 shade, float s, float t) {
    uint flags = sw[S0 + ST_FLAGS];
    bool need0 = (flags & 1u) != 0u, need1 = (flags & 2u) != 0u, two = cycle_type() == CYC_2;
    uint tile = sw[S0 + ST_TILE];
    g_shade = shade;
    g_prim = wcolor(ST_PRIM);
    g_env = wcolor(ST_ENV);
    g_lod = wf(ST_PRIM_LOD);
    g_t0 = need0 || (need1 && !two) ? sample_tile(tile, s, t) : vec4(0.0);
    g_t1 = two ? (need1 ? sample_tile(tile + 1u, s, t) : vec4(0.0)) : g_t0;
    g_comb = vec4(0.0);
    vec4 c = combine_cycle(0u);
    if (two) {
        g_comb = c;
        c = combine_cycle(1u);
    }
    return c;
}

// Blender: (P * A + M * B) / (A + B), blending only with FORCE_BL; otherwise the final cycle
// passes P. The memory color is not readable here, so every value is carried as base + k * memory
// (linear in the memory color) and the fixed-function blend adds memory * k (src ONE, dst
// SRC_ALPHA). The memory alpha (G_BL_A_MEM) is taken as 1.
struct Lin { vec3 base; float k; };

Lin blend_in(uint sel, Lin pixel) {
    if (sel == 0u) return pixel;
    if (sel == 1u) return Lin(vec3(0.0), 1.0);
    if (sel == 2u) return Lin(wcolor(ST_BLEND).rgb, 0.0);
    return Lin(wcolor(ST_FOG).rgb, 0.0);
}

Lin blend_cycle(uint cycle, Lin pixel, float pixel_a, float shade_a, bool final_cycle) {
    uint sh = cycle == 0u ? 30u : 28u;
    uint p = (OM_LO >> sh) & 3u, a = (OM_LO >> (sh - 4u)) & 3u, m = (OM_LO >> (sh - 8u)) & 3u, b = (OM_LO >> (sh - 12u)) & 3u;
    Lin P = blend_in(p, pixel), M = blend_in(m, pixel);
    if (final_cycle && ((OM_LO >> 14) & 1u) == 0u) return P;
    float A = a == 0u ? pixel_a : a == 1u ? wf(ST_FOG + 3u) : a == 2u ? shade_a : 0.0;
    float B = b == 0u ? 1.0 - A : b == 1u ? 1.0 : b == 2u ? 1.0 : 0.0;
    float sum = A + B;
    if (sum <= 0.0) sum = 1.0;
    return Lin((P.base * A + M.base * B) / sum, (P.k * A + M.k * B) / sum);
}

// Coverage, alpha compare, depth and blending of one combined pixel.
void write_pixel(vec4 comb, float shade_a, float z) {
    if ((OM_LO & 3u) == 1u && comb.a < wf(ST_BLEND + 3u)) discard;
    // Coverage x alpha (texture-edge modes) drops mostly transparent pixels.
    if (((OM_LO >> 12) & 1u) != 0u && comb.a < 0.5 && ((OM_LO >> 14) & 1u) == 0u) discard;
    if (((OM_LO >> 2) & 1u) != 0u) z = wf(ST_PRIM_DEPTH);
    if (((OM_LO >> 10) & 3u) == 3u) z -= 0.002;  // decal: on top of what is there
    gl_FragDepth = clamp(z * DEPTH_SCALE, 0.0, 1.0);
    Lin pixel = Lin(comb.rgb, 0.0);
    Lin out_c;
    if (cycle_type() == CYC_2) {
        Lin c0 = blend_cycle(0u, pixel, comb.a, shade_a, false);
        out_c = blend_cycle(1u, c0, comb.a, shade_a, true);
    } else {
        out_c = blend_cycle(0u, pixel, comb.a, shade_a, true);
    }
    o_color = vec4(out_c.base, out_c.k);
}

void main() {
    uint kind = v_ids.y & 255u;
    ivec2 xy = ivec2(gl_FragCoord.xy);
    float S = u_size.z;
    int ox = int(u_size.w);
    if (kind == K_SOLID) {
        o_color = vec4(v_shade.rgb, 0.0);
        gl_FragDepth = v_z;
        return;
    }
    if (kind == K_ZCLEAR) {
        o_color = vec4(0.0);
        gl_FragDepth = DEPTH_SCALE;
        return;
    }
    if (kind == K_PATCH) {  // pixels the CPU wrote, enlarged: p0 = word offset, 1x width, 1x x0, y0
        int px = (xy.x - ox) / int(S) - int(v_p0.z), py = xy.y / int(S) - int(v_p0.w);
        uint w = pw[uint(v_p0.x) + uint(py) * uint(v_p0.y) + uint(px)];
        if ((w >> 16) == 0u) discard;
        o_color = vec4(rgba16(w & 0xFFFFu).rgb, 0.0);
        gl_FragDepth = 1.0;
        return;
    }
    S0 = v_ids.x * STATE_WORDS;
    TB = sw[S0 + ST_TMEM];
    OM_HI = sw[S0 + ST_OM_HI];
    OM_LO = sw[S0 + ST_OM_LO];
    if (xy.x < int(sw[S0 + ST_SCISSOR]) || xy.y < int(sw[S0 + ST_SCISSOR + 1u]) ||
        xy.x >= int(sw[S0 + ST_SCISSOR + 2u]) || xy.y >= int(sw[S0 + ST_SCISSOR + 3u]))
        discard;
    if (kind == K_FILL) {  // two 16-bit pixels alternate along the row (in 1x pixels)
        uint fill = sw[S0 + ST_FILL];
        uint c = (((xy.x - ox) / int(S)) & 1) != 0 ? fill & 0xFFFFu : fill >> 16;
        o_color = vec4(rgba16(c).rgb, 0.0);
        gl_FragDepth = 1.0;
        return;
    }
    if (kind == K_RECT) {
        write_pixel(shade_pixel(vec4(1.0), 0.0, 0.0), 1.0, wf(ST_PRIM_DEPTH));
        return;
    }
    float s, t;
    if (kind == K_TEXRECT) {
        // Texel position of the pixel, in 1x pixels: its corner, as the RDP samples. Enlarged
        // pixels stay within the 1x pixels the rectangle covers, so pictures drawn in strips never
        // filter in texels from beyond a strip edge. p0 = ulx, uly, lrx, lry; p1 = s0, t0, dsdx, dtdy.
        float lx = float(int(v_p0.x)), hx = float(int(v_p0.z) - 1);
        float ly = float(int(v_p0.y)), hy = float(int(v_p0.w) - 1);
        float px = clamp(float(xy.x - ox) / S, lx, hx), py = clamp(float(xy.y) / S, ly, hy);
        float dx = px - v_p0.x, dy = py - v_p0.y;
        bool flip = (v_ids.y & K_FLIP) != 0u;
        s = flip ? v_p1.x + dy * v_p1.z : v_p1.x + dx * v_p1.z;
        t = flip ? v_p1.y + dx * v_p1.w : v_p1.y + dy * v_p1.w;
        if (cycle_type() == CYC_COPY) {
            vec4 c = sample_tile(sw[S0 + ST_TILE], s, t);
            if ((OM_LO & 3u) != 0u && c.a < 0.5) discard;
            o_color = vec4(c.rgb, 0.0);
            gl_FragDepth = 1.0;
            return;
        }
        write_pixel(shade_pixel(vec4(1.0), s, t), 1.0, wf(ST_PRIM_DEPTH));
        return;
    }
    // Triangle: s, t come premultiplied by 1/w with perspective (TEX_PERSP), in which case q = 1/w.
    s = v_stq.x / v_stq.z;
    t = v_stq.y / v_stq.z;
    write_pixel(shade_pixel(v_shade, s, t), v_shade.a, v_z);
}
