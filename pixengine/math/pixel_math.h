/**
 * @file pixel_math.h
 * @brief Pure world-space tile-culling math, plus best-fit viewport scaling.
 *
 * No Vulkan, no scene, no assets — everything here is a plain function over
 * plain numbers so it can be exercised headlessly in test.cpp. This is the
 * invariant that keeps pixengine's test suite runnable without a display:
 * this header must never include <volk/volk.h>.
 *
 * Formerly also held the low-res-offscreen + integer-upscale pipeline's
 * letterbox/pixel-snap math (Letterbox, compute_letterbox(), snap_to_pixel())
 * -- removed when pixengine switched to rendering sprites directly at native
 * resolution with continuous (unsnapped) positions, matching a typical 2D
 * pixel-art project without a Pixel Perfect Camera. compute_fit_viewport()
 * below is a deliberately narrower reintroduction: a stable reference-
 * resolution field of view, uniformly (fractionally, not integer-locked)
 * scaled to best-fit the actual window -- no offscreen buffer, no pixel
 * snapping, no forced integer scale. It answers a different question than
 * the old Letterbox did ("how much of the world is visible, and how big does
 * it render, independent of window size") rather than ("what integer scale
 * keeps every source pixel a crisp block").
 */

#ifndef PIXENGINE_MATH_PIXEL_MATH_H
#define PIXENGINE_MATH_PIXEL_MATH_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

#include <uicoopa/layout/rect.h>

namespace coopa {
namespace pix {

/**
 * @struct FitViewport
 * @brief A centered destination rect (in swapchain pixels) that best-fits a
 *        ref_w x ref_h reference resolution into an sw x sh window, at a
 *        single uniform (continuous, fractional) scale.
 */
struct FitViewport {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    float scale = 0.0f; ///< Output pixels per reference-resolution pixel.
};

/**
 * @brief Computes the centered, uniformly-scaled destination rect for
 *        fitting a ref_w x ref_h reference resolution into an sw x sh window,
 *        preserving aspect ratio ("contain" scaling -- the whole reference
 *        resolution is always visible, centered, with letterbox/pillarbox
 *        space on whichever axis has room to spare).
 *
 * scale = min(sw/ref_w, sh/ref_h), unclamped and fractional -- unlike the
 * old integer-only Letterbox this deliberately shrinks below 1 for windows
 * smaller than the reference resolution rather than cropping. x/y are always
 * >= 0 by construction (the constraining axis has zero padding, the other
 * has non-negative padding), so callers don't need Letterbox's negative-
 * origin handling.
 */
inline FitViewport compute_fit_viewport(uint32_t sw, uint32_t sh, uint32_t ref_w, uint32_t ref_h) {
    if (sw == 0 || sh == 0 || ref_w == 0 || ref_h == 0) {
        return FitViewport{};
    }

    float scale = std::min(static_cast<float>(sw) / static_cast<float>(ref_w),
                           static_cast<float>(sh) / static_cast<float>(ref_h));
    float w = static_cast<float>(ref_w) * scale;
    float h = static_cast<float>(ref_h) * scale;
    float x = (static_cast<float>(sw) - w) * 0.5f;
    float y = (static_cast<float>(sh) - h) * 0.5f;

    return FitViewport{x, y, w, h, scale};
}

/**
 * @struct TileRange
 * @brief An inclusive [x0,x1] x [y0,y1] tile-index box, clamped to a layer's
 *        bounds. empty() when the query rect had no overlap.
 */
struct TileRange {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    bool empty() const { return x1 < x0 || y1 < y0; }
};

/**
 * @brief Converts a world-space AABB into the inclusive tile-index range of
 *        a layer that overlaps it, clamped to the layer's bounds.
 *
 * Tile row 0 is the layer's TOP (largest world Y) -- .pix/tilemap convention
 * is top-left origin, but world space is +Y up, so row index and world Y
 * move in OPPOSITE directions: a larger world Y maps to a SMALLER row. Get
 * this flip wrong here once rather than at every call site; it's the classic
 * tilemap off-by-one/mirror bug.
 *
 * @param world_rect      World-space AABB to test (e.g. the camera's
 *                        visible_world_rect(), already expanded by a margin).
 * @param tile_size_units One tile's size in WORLD units (tile_size_px / pixels_per_unit).
 * @param layer_size      Layer size in tiles (width, height).
 * @param layer_origin    World position of tile (col=0,row=0)'s TOP-LEFT corner.
 * @return The inclusive tile range overlapping world_rect, clamped to
 *         [0,layer_size), or an empty() range if there's no overlap.
 */
inline TileRange tile_rect_from_world_rect(const coopa::ui::Rect& world_rect,
                                           glm::vec2 tile_size_units,
                                           glm::ivec2 layer_size,
                                           glm::vec2 layer_origin) {
    if (tile_size_units.x <= 0.0f || tile_size_units.y <= 0.0f ||
        layer_size.x <= 0 || layer_size.y <= 0) {
        return TileRange{0, 0, -1, -1};
    }

    int x0 = static_cast<int>(std::floor((world_rect.min.x - layer_origin.x) / tile_size_units.x));
    int x1 = static_cast<int>(std::floor((world_rect.max.x - layer_origin.x) / tile_size_units.x));
    // Y inverted: the query's max.y (visually highest) maps to the SMALLEST row.
    int y0 = static_cast<int>(std::floor((layer_origin.y - world_rect.max.y) / tile_size_units.y));
    int y1 = static_cast<int>(std::floor((layer_origin.y - world_rect.min.y) / tile_size_units.y));

    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, layer_size.x - 1);
    y1 = std::min(y1, layer_size.y - 1);

    if (x1 < x0 || y1 < y0) return TileRange{0, 0, -1, -1};
    return TileRange{x0, y0, x1, y1};
}

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_MATH_PIXEL_MATH_H
