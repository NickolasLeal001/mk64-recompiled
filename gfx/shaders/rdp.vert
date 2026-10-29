#version 450
// Vertices arrive in pixels of the render target; attributes are interpolated linearly in screen
// space (w = 1), and perspective-correct texture coordinates come premultiplied by 1/w.
#extension GL_GOOGLE_include_directive : require
#include "rdp_common.glsl"

layout(location = 0) in vec4 a_pos;    // x, y (target pixels), z (0..1), 1/w
layout(location = 1) in vec4 a_shade;  // shade color
layout(location = 2) in vec2 a_st;     // s, t (texels; times 1/w with perspective)
layout(location = 3) in uvec2 a_ids;   // state index, kind | flags << 8
layout(location = 4) in vec4 a_p0;     // per-kind parameters
layout(location = 5) in vec4 a_p1;

layout(set = 1, binding = 0) uniform Target { vec4 u_size; };  // width, height, scale, side margin

layout(location = 0) noperspective out vec4 v_shade;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out float v_z;
layout(location = 3) flat out uvec2 v_ids;
layout(location = 4) flat out vec4 v_p0;
layout(location = 5) flat out vec4 v_p1;

void main() {
    gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, clamp(a_pos.z * DEPTH_SCALE, 0.0, 1.0), 1.0);
    v_shade = a_shade;
    v_stq = vec3(a_st, a_pos.w);
    v_z = a_pos.z;
    v_ids = a_ids;
    v_p0 = a_p0;
    v_p1 = a_p1;
}
