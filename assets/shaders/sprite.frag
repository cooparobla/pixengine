#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;

layout(set = 0, binding = 0) uniform sampler2D atlas;

// scale/camera_pos are read by sprite.vert (see that file's doc); unused here, but must
// stay declared to match that stage's block byte-for-byte (shared VkPushConstantRange).
layout(push_constant) uniform Push {
    vec2 scale;
    vec2 offset;
    vec4 tint;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex = texture(atlas, in_uv); // premultiplied alpha (see pix_composite.h)

    // in_color/pc.tint are straight-alpha tints (e.g. a fade sets alpha < 1
    // with rgb untouched). Combining them with a premultiplied texture must
    // scale color by tint.rgb *and* scale everything -- color included -- by
    // tint.a, or a fade would leave rgb at full brightness while alpha drops,
    // producing a bright fringe once blended (BlendMode::PremultipliedAlpha
    // in gfxcoopa's pipeline.h expects its input pre-scaled like this).
    vec4 tint = in_color * pc.tint;
    out_color = vec4(tex.rgb * tint.rgb * tint.a, tex.a * tint.a);
}
