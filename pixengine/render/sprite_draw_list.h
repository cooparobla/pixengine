/**
 * @file sprite_draw_list.h
 * @brief Accumulates one frame's sprite geometry, sorts it, and batches it
 *        into indexed draw calls.
 *
 * Two-phase: add_quad()/add_sprite() record a SpriteQuad plus a packed
 * uint64_t sort key (world content can be emitted in any order);
 * sort_and_flatten() stable-sorts an index array by that key and only then
 * writes vertices_/indices_/batches_, so adjacent same-texture quads in
 * SORTED order -- not insertion order -- coalesce into one draw call.
 *
 * Forked from, not reusing, coopa::ui::DrawList: that class batches strictly
 * in insertion order for UI (correct there -- widgets are already emitted
 * back-to-front) and only ever produces axis-aligned quads from a canvas-
 * space Rect, with no rotation/pivot/flip. coopa::ui::Rect is still reused
 * here for UV/world rects -- it's pure glm math with no UI dependency.
 */

#ifndef PIXENGINE_RENDER_SPRITE_DRAW_LIST_H
#define PIXENGINE_RENDER_SPRITE_DRAW_LIST_H

#include <gfxcoopa/types/texture_view.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <numeric>
#include <vector>

#include <uicoopa/layout/rect.h>

#include <pixengine/render/sprite_vertex.h>

namespace coopa {
namespace pix {

/// @brief A contiguous run of indices sharing one atlas texture, in sorted order.
struct SpriteBatch {
    uint32_t    first_index = 0;
    uint32_t    index_count = 0;
    coopa::gfx::TextureView texture_view;
};

/**
 * @brief Packs a draw-order sort key: sort_layer, then depth (y-sort or
 *        insertion order within the layer), then a texture bucket (so
 *        same-texture quads at the same layer/depth coalesce), then a
 *        stable per-call tiebreak. A single uint64_t compare orders two quads.
 *
 * @param sort_layer     Signed layer index (e.g. Ground=-100 .. Overlay=100).
 * @param y_sort         If true, depth comes from `world_y` (descending world_y
 *                       sorts first / further back); if false, from `order_in_layer`.
 * @param world_y        World-space Y used when y_sort is true, quantized over a
 *                       fixed +/-4096 unit range -- comfortably larger than any
 *                       Stardew-scale map.
 * @param order_in_layer Explicit draw order used when y_sort is false.
 * @param texture        Groups same-texture quads adjacent in sorted order.
 * @param sub_order      Final tiebreak within one entity (e.g. body before a
 *                       decal on the exact same texture/layer/depth).
 */
inline uint64_t pack_sprite_sort_key(int32_t sort_layer, bool y_sort, float world_y,
                                     uint32_t order_in_layer, coopa::gfx::TextureView texture,
                                     uint8_t sub_order = 0) {
    constexpr float    kWorldExtent = 4096.0f;                      // +/- half-range the quantizer covers
    constexpr uint32_t kDepthBits   = 0xFFFFFFu;                    // 24 bits
    constexpr float    kDepthScale  = static_cast<float>(kDepthBits) / (2.0f * kWorldExtent);

    uint64_t layer_bits = static_cast<uint64_t>(static_cast<int32_t>(sort_layer) + 32768) & 0xFFFFu;

    uint64_t depth_bits;
    if (y_sort) {
        // Larger world_y sorts FIRST (drawn further back); a lower Y (closer
        // to "camera"/screen-bottom) draws later, on top -- standard 2D
        // top-down y-sort. Depth therefore increases as world_y decreases.
        float shifted = (-world_y + kWorldExtent) * kDepthScale;
        depth_bits = static_cast<uint64_t>(std::clamp(shifted, 0.0f, static_cast<float>(kDepthBits)));
    } else {
        depth_bits = static_cast<uint64_t>(order_in_layer) & kDepthBits;
    }

    uint64_t texture_bucket = static_cast<uint64_t>(std::hash<coopa::gfx::TextureView>{}(texture)) & 0xFFFFu;
    uint64_t sub = sub_order;

    return (layer_bits << 48) | (depth_bits << 24) | (texture_bucket << 8) | sub;
}

/**
 * @class SpriteDrawList
 * @brief Records world-space sprite quads, then sorts and batches them.
 */
class SpriteDrawList {
public:
    /// @brief Resets the list for a new frame.
    void begin() {
        quads_.clear();
        order_.clear();
        vertices_.clear();
        indices_.clear();
        batches_.clear();
    }

    /**
     * @brief Records a single quad from four already-transformed world-space
     *        corners and their matching UVs, in the order bottom-left,
     *        bottom-right, top-right, top-left.
     */
    void add_quad(coopa::gfx::TextureView tex, const glm::vec2 pos[4], const glm::vec2 uv[4], uint32_t color,
                 int32_t sort_layer, bool y_sort, float world_y, uint32_t order_in_layer,
                 uint8_t sub_order = 0) {
        SpriteQuad q;
        for (int i = 0; i < 4; ++i) {
            q.pos[i] = pos[i];
            q.uv[i] = uv[i];
        }
        q.color = color;
        q.texture = tex;
        q.sort_key = pack_sprite_sort_key(sort_layer, y_sort, world_y, order_in_layer, tex, sub_order);
        quads_.push_back(q);
    }

    /**
     * @brief Records a pivoted, rotated, optionally-flipped sprite quad.
     *
     * @param tex          Atlas texture view.
     * @param world_pos    World-space position of the sprite's pivot.
     * @param size_units   Full sprite size in world units.
     * @param pivot_norm   Pivot as a [0,1] fraction of size_units, e.g. (0.5,0) = bottom-center.
     * @param rotation_deg Rotation in degrees, counter-clockwise, about the pivot.
     * @param flip         Component-wise +-1 multipliers, applied about the pivot before rotation.
     * @param uv           Atlas UV rect; uv.min is the TOP of the sprite (matching .pix's
     *                     top-left/+Y-down source convention), mapped here to the quad's
     *                     +Y (top, world-up) corners -- the one place that orientation
     *                     flip must happen, so get it wrong once, not per caller.
     * @param color        Packed RGBA8 tint, see SpriteVertex::pack_color().
     * @param sort_layer, y_sort, world_y, order_in_layer, sub_order  See pack_sprite_sort_key().
     */
    void add_sprite(coopa::gfx::TextureView tex, const glm::vec2& world_pos, const glm::vec2& size_units,
                    const glm::vec2& pivot_norm, float rotation_deg, const glm::vec2& flip,
                    const coopa::ui::Rect& uv, uint32_t color,
                    int32_t sort_layer = 0, bool y_sort = false, float world_y = 0.0f,
                    uint32_t order_in_layer = 0, uint8_t sub_order = 0) {
        glm::vec2 local_min = -pivot_norm * size_units;
        glm::vec2 local_max = local_min + size_units;

        glm::vec2 local[4] = {
            {local_min.x, local_min.y}, // bottom-left
            {local_max.x, local_min.y}, // bottom-right
            {local_max.x, local_max.y}, // top-right
            {local_min.x, local_max.y}, // top-left
        };

        float rad = rotation_deg * (3.14159265358979323846f / 180.0f);
        float c = std::cos(rad), s = std::sin(rad);

        glm::vec2 pos[4];
        for (int i = 0; i < 4; ++i) {
            glm::vec2 p = local[i] * flip; // flip about the pivot, before rotation
            glm::vec2 r{p.x * c - p.y * s, p.x * s + p.y * c};
            pos[i] = world_pos + r;
        }

        glm::vec2 uvs[4] = {
            {uv.min.x, uv.max.y}, // bottom-left  <- bottom of the source sprite
            {uv.max.x, uv.max.y}, // bottom-right
            {uv.max.x, uv.min.y}, // top-right    <- top of the source sprite
            {uv.min.x, uv.min.y}, // top-left
        };

        add_quad(tex, pos, uvs, color, sort_layer, y_sort, world_y, order_in_layer, sub_order);
    }

    /**
     * @brief Stable-sorts recorded quads by sort key and builds the final
     *        vertex/index/batch streams. Call once per frame after every
     *        add_quad()/add_sprite() for the frame.
     */
    void sort_and_flatten() {
        vertices_.clear();
        indices_.clear();
        batches_.clear();

        order_.resize(quads_.size());
        std::iota(order_.begin(), order_.end(), uint32_t{0});
        std::stable_sort(order_.begin(), order_.end(), [this](uint32_t a, uint32_t b) {
            return quads_[a].sort_key < quads_[b].sort_key;
        });

        vertices_.reserve(quads_.size() * 4);
        indices_.reserve(quads_.size() * 6);

        for (uint32_t idx : order_) {
            const SpriteQuad& q = quads_[idx];
            if (batches_.empty() || batches_.back().texture_view != q.texture) {
                SpriteBatch b{};
                b.first_index = static_cast<uint32_t>(indices_.size());
                b.index_count = 0;
                b.texture_view = q.texture;
                batches_.push_back(b);
            }

            uint32_t base = static_cast<uint32_t>(vertices_.size());
            for (int i = 0; i < 4; ++i) {
                vertices_.push_back({q.pos[i].x, q.pos[i].y, q.uv[i].x, q.uv[i].y, q.color});
            }
            indices_.push_back(base + 0);
            indices_.push_back(base + 1);
            indices_.push_back(base + 2);
            indices_.push_back(base + 0);
            indices_.push_back(base + 2);
            indices_.push_back(base + 3);
            batches_.back().index_count += 6;
        }
    }

    const std::vector<SpriteVertex>& vertices() const { return vertices_; }
    const std::vector<uint32_t>&     indices()  const { return indices_; }
    const std::vector<SpriteBatch>&  batches()  const { return batches_; }

    /// @brief Number of quads recorded so far this frame (pre-sort_and_flatten()).
    size_t quad_count() const { return quads_.size(); }

private:
    struct SpriteQuad {
        glm::vec2   pos[4];
        glm::vec2   uv[4];
        uint32_t    color = 0;
        coopa::gfx::TextureView texture;
        uint64_t    sort_key = 0;
    };

    std::vector<SpriteQuad> quads_;
    std::vector<uint32_t>   order_;

    std::vector<SpriteVertex> vertices_;
    std::vector<uint32_t>     indices_;
    std::vector<SpriteBatch>  batches_;
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_RENDER_SPRITE_DRAW_LIST_H
