/**
 * @file tilemap_asset.h
 * @brief The published, GPU-resident result of loading a .tilemap.yaml: one
 *        whole-canvas tileset texture plus dense per-layer gid grids.
 *
 * The tileset image is the tileset .pix's composited canvas used AS-IS (no
 * trim, no shelf-packing) -- unlike SpriteAsset's atlas, a tileset is
 * already a uniform grid at authoring time; trimming or repacking it would
 * break the fixed (col,row) <-> gid mapping every layer's gids rely on.
 */

#ifndef PIXENGINE_DATA_TILEMAP_ASSET_H
#define PIXENGINE_DATA_TILEMAP_ASSET_H

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

#include <gfxcoopa/engine/data/texture.h>
#include <uicoopa/layout/rect.h>

namespace coopa {
namespace pix {

/// @brief One layer's dense, row-major gid grid. gid 0 means empty.
struct TileLayer {
    std::string name;
    int         sort_layer = 0;
    bool        y_sort = false;
    float       y_sort_offset = 0.0f;
    uint32_t    width = 0;
    uint32_t    height = 0;
    std::vector<uint16_t> gids; ///< row-major, origin TOP-LEFT, size width*height

    uint16_t at(uint32_t x, uint32_t y) const { return gids[static_cast<size_t>(y) * width + x]; }
};

/**
 * @brief Converts a gid to its (col,row) position in the tileset grid.
 * @return {UINT32_MAX,UINT32_MAX} if gid < first_gid or columns == 0 (no
 *         valid tile), so callers can check for that sentinel without a
 *         separate bounds branch at every call site.
 */
inline glm::uvec2 gid_to_tile_coord(uint16_t gid, uint32_t first_gid, uint32_t columns) {
    if (gid < first_gid || columns == 0) {
        return glm::uvec2(UINT32_MAX, UINT32_MAX);
    }
    uint32_t index = static_cast<uint32_t>(gid) - first_gid;
    return glm::uvec2(index % columns, index / columns);
}

/// @brief Inert collision metadata: parsed and exposed, no system consumes it (out of scope).
struct TilemapCollisionRule {
    std::string layer;
    std::vector<uint16_t> solid_gids;
};

/// @brief CPU-only intermediate between TilemapLoader's decode_typed() and finalize_typed().
struct DecodedTilemap {
    glm::vec2 tile_size_px{16.0f, 16.0f};
    float     pixels_per_unit = 16.0f;
    std::string tileset_source; ///< Path (relative to the tilemap's own file) to the tileset .pix.
    uint32_t  tileset_columns = 1;
    uint32_t  tileset_first_gid = 1;
    std::vector<uint32_t> tileset_pixels; ///< premultiplied RGBA8, tileset_width*tileset_height
    int tileset_width = 0;
    int tileset_height = 0;
    std::vector<TileLayer> layers;
    std::vector<TilemapCollisionRule> collision;
};

/**
 * @class TilemapAsset
 * @brief The GPU-resident, immutable result of loading a tilemap.
 */
class TilemapAsset {
public:
    TilemapAsset(coopa::gfx::engine::data::Texture tileset, DecodedTilemap data)
        : tileset_(std::move(tileset)), data_(std::move(data)) {}

    VkImageView tileset_view() const { return tileset_.view(); }

    const std::vector<TileLayer>& layers() const { return data_.layers; }
    glm::vec2 tile_size_px() const { return data_.tile_size_px; }
    float pixels_per_unit() const { return data_.pixels_per_unit; }
    const std::vector<TilemapCollisionRule>& collision() const { return data_.collision; }

    /// @brief Normalized, half-texel-inset atlas UV for a gid, or an empty Rect if out of range.
    coopa::ui::Rect uv_for_gid(uint16_t gid) const {
        glm::uvec2 coord = gid_to_tile_coord(gid, data_.tileset_first_gid, data_.tileset_columns);
        if (coord.x == UINT32_MAX) return coopa::ui::Rect{};
        uint32_t col = coord.x, row = coord.y;

        float tw = data_.tile_size_px.x, th = data_.tile_size_px.y;
        float atlas_w = static_cast<float>(tileset_.width());
        float atlas_h = static_cast<float>(tileset_.height());
        if (atlas_w <= 0.0f || atlas_h <= 0.0f) return coopa::ui::Rect{};

        float x0 = col * tw, y0 = row * th;
        float u0 = (x0 + 0.5f) / atlas_w, v0 = (y0 + 0.5f) / atlas_h;
        float u1 = (x0 + tw - 0.5f) / atlas_w, v1 = (y0 + th - 0.5f) / atlas_h;
        return coopa::ui::Rect{{u0, v0}, {u1, v1}};
    }

    TilemapAsset(TilemapAsset&&) = default;
    TilemapAsset& operator=(TilemapAsset&&) = default;
    TilemapAsset(const TilemapAsset&) = delete;
    TilemapAsset& operator=(const TilemapAsset&) = delete;

private:
    coopa::gfx::engine::data::Texture tileset_;
    DecodedTilemap data_;
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_DATA_TILEMAP_ASSET_H
