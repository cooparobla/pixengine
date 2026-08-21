/**
 * @file atlas_packer.h
 * @brief Packs a set of rectangles (trimmed sprite frames) into one atlas.
 *
 * A shelf (row) packer: near-optimal for the near-uniform frame sizes a
 * character sheet produces, ~40 lines, deterministic. Not behind a virtual
 * interface — there is exactly one implementation and no second caller
 * waiting on a different one; swapping in a MaxRects packer later means
 * writing a new function with this same signature, which needs no interface
 * to already exist.
 */

#ifndef PIXENGINE_PIX_ATLAS_PACKER_H
#define PIXENGINE_PIX_ATLAS_PACKER_H

#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace coopa {
namespace pix {

/// @brief A placed rectangle's position and size within the packed atlas.
struct AtlasRect {
    int x = 0, y = 0, w = 0, h = 0;
};

/// @brief The result of pack_shelf(): overall atlas dimensions plus one
///        AtlasRect per input size, in the same order as the input.
struct AtlasPackResult {
    int atlas_width = 0;
    int atlas_height = 0;
    std::vector<AtlasRect> rects;
};

inline int next_pow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

/**
 * @brief Packs `sizes` into one atlas using a shelf (row) layout.
 *
 * Frames are placed widest-shelf-first by sorting on descending height
 * (ties broken by original index, so packing is deterministic across runs),
 * left to right within a shelf, wrapping to a new shelf when a rect would
 * overflow the atlas width. `padding` pixels separate every rect from its
 * neighbors and from the atlas border, eliminating sampling bleed between
 * unrelated frames. Atlas width is fixed from the widest single frame;
 * height grows by doubling from 64 up to `max_dimension` until everything
 * fits.
 *
 * @throws std::runtime_error if a single frame (plus padding) is wider than
 *         max_dimension, or if the content doesn't fit even at max_dimension
 *         height — callers should surface this as an AssetState::Failed
 *         load, not a crash.
 */
inline AtlasPackResult pack_shelf(const std::vector<glm::ivec2>& sizes,
                                  int padding = 1,
                                  int max_dimension = 4096) {
    AtlasPackResult result;
    size_t n = sizes.size();
    if (n == 0) {
        return result; // atlas_width/height stay 0, rects stays empty
    }

    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return sizes[a].y > sizes[b].y;
    });

    int max_w = 0;
    for (const auto& s : sizes) max_w = std::max(max_w, s.x);
    int atlas_w = std::max(64, next_pow2(max_w + 2 * padding));
    if (atlas_w > max_dimension) {
        throw std::runtime_error("pack_shelf: a frame is wider than max_dimension");
    }

    int atlas_h = 64;
    while (true) {
        int cursor_x = padding, cursor_y = padding, shelf_h = 0;
        bool fits = true;
        std::vector<AtlasRect> placed(n);

        for (size_t idx : order) {
            int w = sizes[idx].x, h = sizes[idx].y;
            if (cursor_x + w + padding > atlas_w) {
                cursor_x = padding;
                cursor_y += shelf_h + padding;
                shelf_h = 0;
            }
            if (cursor_y + h + padding > atlas_h) {
                fits = false;
                break;
            }
            placed[idx] = AtlasRect{cursor_x, cursor_y, w, h};
            cursor_x += w + padding;
            shelf_h = std::max(shelf_h, h);
        }

        if (fits) {
            result.atlas_width = atlas_w;
            result.atlas_height = atlas_h;
            result.rects = std::move(placed);
            return result;
        }
        if (atlas_h >= max_dimension) {
            throw std::runtime_error("pack_shelf: content exceeds max_dimension even at max atlas height");
        }
        atlas_h = std::min(atlas_h * 2, max_dimension);
    }
}

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_PIX_ATLAS_PACKER_H
