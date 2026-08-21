#version 450

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

layout(push_constant) uniform Push {
    vec2 inv_half_extent; // 1 / (half-width, half-height) of the camera view, in world units
    vec2 camera_pos;      // Camera2D::world_position() -- plain continuous world
                          // position, no pixel-grid snapping anywhere in this pipeline.
    vec4 tint;            // global tint/fade, RGBA multiply
} pc;

void main() {
    vec2 view = in_pos - pc.camera_pos;
    vec2 ndc = view * pc.inv_half_extent;
    // World +Y up; Vulkan NDC +Y down -- flip here, once, rather than at every call site.
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    out_uv = in_uv;
    out_color = in_color;
}
