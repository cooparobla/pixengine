#version 450

// See gfx/surface2d/quad_vs.glsl for the shared affine-transform function this calls and
// the scale/offset convention it documents. scale = inv_half_extent, offset =
// -camera_pos * inv_half_extent -- computed once on the C++ side (SpritePass::draw()'s
// caller) instead of subtracting camera_pos here every vertex.

#include <gfx/surface2d/quad_vs.glsl>

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

layout(push_constant) uniform Push {
    vec2 scale;  // = inv_half_extent
    vec2 offset; // = -camera_pos * inv_half_extent
    vec4 tint;   // global tint/fade, RGBA multiply -- read by sprite.frag, unused here
} pc;

void main() {
    gl_Position = gfx_quad_2d_transform(in_pos, pc.scale, pc.offset);
    out_uv = in_uv;
    out_color = in_color;
}
