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

#include <volk/volk.h>
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
    uint32_t color; ///< Packed RGBA8, VK_FORMAT_R8G8B8A8_UNORM (r in the low byte).

    /// @brief Packs four [0,1] float color channels into SpriteVertex::color's layout.
    static uint32_t pack_color(float r, float g, float b, float a) {
        auto to_u8 = [](float v) {
            return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return to_u8(r) | (to_u8(g) << 8) | (to_u8(b) << 16) | (to_u8(a) << 24);
    }

    static VkVertexInputBindingDescription binding_description() {
        VkVertexInputBindingDescription binding{};
        binding.binding   = 0;
        binding.stride    = sizeof(SpriteVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    static std::vector<VkVertexInputAttributeDescription> attribute_descriptions() {
        std::vector<VkVertexInputAttributeDescription> attrs(3);
        attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT,  static_cast<uint32_t>(offsetof(SpriteVertex, x))};
        attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT,  static_cast<uint32_t>(offsetof(SpriteVertex, u))};
        attrs[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(SpriteVertex, color))};
        return attrs;
    }
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_RENDER_SPRITE_VERTEX_H
