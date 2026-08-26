/**
 * @file sprite_vertex.h
 * @brief The 2D vertex format used by SpriteDrawList / SpritePass.
 *
 * Layout-identical to coopa::ui::UiVertex (pos vec2, uv vec2, packed RGBA8
 * color) but defined independently here rather than included from uicoopa:
 * sprite rendering must not *require* the UI library as a dependency, even
 * though the two happen to share a vertex shape. Positions are WORLD-SPACE
 * units, not screen/canvas pixels — the camera lives in a push constant
 * (see sprite_pass.h) so world-space vertices are camera-motion-invariant,
 * which matters once static tilemap chunks are baked (Phase 8).
 */

#ifndef PIXENGINE_RENDER_SPRITE_VERTEX_H
#define PIXENGINE_RENDER_SPRITE_VERTEX_H

#include <gfxcoopa/types/vertex_layout.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace coopa {
namespace pix {

/// @brief Position (world units), UV, and packed color for a single sprite vertex.
struct SpriteVertex {
    float    x, y;  ///< World-space position.
    float    u, v;  ///< Texture coordinates into the sprite atlas.
    uint32_t color; ///< Packed RGBA8, Format::RGBA8_Unorm (r in the low byte).

    /// @brief Packs four [0,1] float color channels into SpriteVertex::color's layout.
    static uint32_t pack_color(float r, float g, float b, float a) {
        auto to_u8 = [](float v) {
            return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return to_u8(r) | (to_u8(g) << 8) | (to_u8(b) << 16) | (to_u8(a) << 24);
    }

    /// @brief This vertex format's binding+attribute layout, for the sealed Pipeline ctor.
    static coopa::gfx::VertexLayout layout() {
        return coopa::gfx::VertexLayout{}
            .binding(0, sizeof(SpriteVertex))
            .attribute(0, coopa::gfx::Format::RG32_Sfloat, static_cast<uint32_t>(offsetof(SpriteVertex, x)))
            .attribute(1, coopa::gfx::Format::RG32_Sfloat, static_cast<uint32_t>(offsetof(SpriteVertex, u)))
            .attribute(2, coopa::gfx::Format::RGBA8_Unorm, static_cast<uint32_t>(offsetof(SpriteVertex, color)));
    }
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_RENDER_SPRITE_VERTEX_H
