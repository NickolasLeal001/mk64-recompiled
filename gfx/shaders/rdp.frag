#version 450
// The RDP pixel pipeline for the display copies.
#extension GL_GOOGLE_include_directive : require
#include "rdp_common.glsl"

layout(location = 0) noperspective in vec4 v_shade;
layout(location = 1) noperspective in vec3 v_stq;
layout(location = 2) noperspective in float v_z;
layout(location = 3) flat in uvec2 v_ids;
layout(location = 4) flat in vec4 v_p0;
layout(location = 5) flat in vec4 v_p1;

layout(location = 0) out vec4 o_color;

void main() {
    uint kind = v_ids.y & 255u;
    if (kind == K_SOLID) {
        o_color = vec4(v_shade.rgb, 0.0);
        gl_FragDepth = v_z;
        return;
    }
    discard;
}
