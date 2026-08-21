/**
 * @file tilemap_renderer.h
 * @brief Culls and emits a TilemapAsset's visible tiles every frame.
 *
 * Dynamic batching only (Phase 6): every visible tile is emitted as a fresh
 * quad into SpriteDrawList each frame, culled to the camera's visible world
 * rect via tile_rect_from_world_rect(). The quad count is bounded by the
 * TILEMAP's own size, not the view -- tile_rect_from_world_rect() clamps to
 * layer_size regardless of how much world area is visible, so a larger
 * window (more world visible at native resolution) doesn't blow this up; the
 * farm tilemap (30x20 tiles, 2 layers) caps out at 1200 quads a frame
 * regardless of zoom/resolution. Baked per-chunk static vertex buffers
 * (Phase 8, optimization only) would replace the inner loop below without
 * changing this component's public API.
 */

#ifndef PIXENGINE_SCENE_TILEMAP_RENDERER_H
#define PIXENGINE_SCENE_TILEMAP_RENDERER_H

#include <cstdint>
#include <glm/glm.hpp>
#include <string>

#include <coopa/asset/asset_handle.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>

#include <uicoopa/layout/rect.h>

#include <pixengine/data/tilemap_asset.h>
#include <pixengine/math/pixel_math.h>
#include <pixengine/render/sprite_draw_list.h>
#include <pixengine/render/sprite_vertex.h>
#include <pixengine/scene/transform2d.h>

namespace coopa {
namespace pix {

/**
 * @class TilemapRenderer
 * @brief Renders every layer of a loaded TilemapAsset, culled per frame.
 */
class TilemapRenderer : public coopa::scene::Component {
public:
    std::string tilemap; ///< Authored path, e.g. "tilemaps/farm.tilemap.yaml".

    /// Set by register.h's YAML parser (Phase 6: synchronous load at parse
    /// time, same as SpriteRenderer -- Phase 7's residency system streams
    /// sprites, not tilemaps, which are expected to be always-resident).
    coopa::asset::AssetHandle<TilemapAsset> handle;

    std::string type_name() const override { return "TilemapRenderer"; }

    /**
     * @brief Culls and appends every visible tile of every layer to the draw list.
     *
     * @param list               This frame's SpriteDrawList.
     * @param visible_world_rect The active camera's visible_world_rect(), used
     *                           for per-layer culling.
     * @param quads_emitted      Incremented by the number of tile quads
     *                           actually appended -- lets the caller log a
     *                           per-frame count to confirm culling is working.
     */
    void emit(SpriteDrawList& list, const coopa::ui::Rect& visible_world_rect,
             uint32_t& quads_emitted) const {
        if (!owner || !handle.is_loaded()) return;
        const TilemapAsset* asset = handle.get();
        const Transform2D* tf = owner->get_component<Transform2D>();
        glm::vec2 origin = tf ? tf->world_position : glm::vec2(0.0f);

        float ppu = asset->pixels_per_unit();
        if (ppu <= 0.0f) return;
        glm::vec2 tile_size_units = asset->tile_size_px() / ppu;

        for (const TileLayer& layer : asset->layers()) {
            if (layer.width == 0 || layer.height == 0) continue;

            TileRange range = tile_rect_from_world_rect(
                visible_world_rect, tile_size_units, glm::ivec2(layer.width, layer.height), origin);
            if (range.empty()) continue;

            for (int row = range.y0; row <= range.y1; ++row) {
                for (int col = range.x0; col <= range.x1; ++col) {
                    uint16_t gid = layer.at(static_cast<uint32_t>(col), static_cast<uint32_t>(row));
                    if (gid == 0) continue;

                    coopa::ui::Rect uv = asset->uv_for_gid(gid);

                    // Tile (col,row)'s bottom-left corner in world space: row
                    // increases downward from origin (world Y decreases per row).
                    glm::vec2 world_pos{
                        origin.x + col * tile_size_units.x,
                        origin.y - (row + 1) * tile_size_units.y,
                    };
                    float world_y = world_pos.y + layer.y_sort_offset;

                    list.add_sprite(asset->tileset_view(), world_pos, tile_size_units,
                                    glm::vec2(0.0f, 0.0f), 0.0f, glm::vec2(1.0f, 1.0f), uv,
                                    0xFFFFFFFFu, layer.sort_layer, layer.y_sort, world_y,
                                    static_cast<uint32_t>(row) * layer.width + static_cast<uint32_t>(col));
                    ++quads_emitted;
                }
            }
        }
    }
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_SCENE_TILEMAP_RENDERER_H
