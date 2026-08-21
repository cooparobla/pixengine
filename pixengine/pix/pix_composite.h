/**
 * @file pix_composite.h
 * @brief Composites a PixFrame's layers into a single premultiplied-RGBA8
 *        pixel buffer, and trims it to its opaque bounding box.
 *
 * Mirrors coopixel's PixelDocument.render_frame_qimage() and
 * StrokeEffect.render_effect() (see coopixel/src/coopixel/models/{document,
 * effects}.py) pixel-for-pixel: per visible layer, draw effect "below"
 * pixels, then the layer's own pixels, then effect "above" pixels, each
 * multiplied by the layer's opacity and source-over composited onto the
 * canvas so far. Pure CPU, no Vulkan.
 */

#ifndef PIXENGINE_PIX_PIX_COMPOSITE_H
#define PIXENGINE_PIX_PIX_COMPOSITE_H

#include <algorithm>
#include <glm/glm.hpp>
#include <set>
#include <vector>

#include <pixengine/pix/color.h>
#include <pixengine/pix/pix_document.h>

namespace coopa {
namespace pix {

namespace detail {

using PixelList = std::vector<std::pair<glm::ivec2, uint32_t>>;

/**
 * @brief Reimplements StrokeEffect.render_effect() from coopixel/effects.py.
 *
 * Appends to below_out/above_out (never clears them — callers accumulate
 * across a layer's effect list) rather than returning fresh containers.
 */
inline void apply_stroke_effect(const PixEffect& eff, const PixelList& pixels,
                                int doc_width, int doc_height,
                                PixelList& below_out, PixelList& above_out) {
    if (!eff.enabled || pixels.empty() || eff.size <= 0) return;

    std::set<std::pair<int, int>> filled;
    for (const auto& p : pixels) filled.insert({p.first.x, p.first.y});
    if (filled.empty()) return;

    auto in_bounds = [&](int x, int y) { return x >= 0 && x < doc_width && y >= 0 && y < doc_height; };

    if (eff.position == "outside") {
        int radius = eff.size;
        float r2 = (radius + 0.5f) * (radius + 0.5f);
        std::set<std::pair<int, int>> stroke;
        for (const auto& [cx, cy] : filled) {
            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    if (dx == 0 && dy == 0) continue;
                    if (dx * dx + dy * dy <= r2) {
                        int nx = cx + dx, ny = cy + dy;
                        if (in_bounds(nx, ny)) stroke.insert({nx, ny});
                    }
                }
            }
        }
        for (const auto& c : stroke) {
            if (filled.find(c) == filled.end()) {
                below_out.emplace_back(glm::ivec2(c.first, c.second), eff.color);
            }
        }
    } else if (eff.position == "inside") {
        int radius = eff.size;
        float r2 = (radius + 0.5f) * (radius + 0.5f);
        for (const auto& [cx, cy] : filled) {
            bool is_border = false;
            for (int dx = -radius; dx <= radius && !is_border; ++dx) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    if (dx == 0 && dy == 0) continue;
                    if (dx * dx + dy * dy <= r2) {
                        int nx = cx + dx, ny = cy + dy;
                        if (!in_bounds(nx, ny) || filled.find({nx, ny}) == filled.end()) {
                            is_border = true;
                            break;
                        }
                    }
                }
            }
            if (is_border) above_out.emplace_back(glm::ivec2(cx, cy), eff.color);
        }
    } else if (eff.position == "center") {
        int out_radius = std::max(1, eff.size / 2);
        float r2 = (out_radius + 0.5f) * (out_radius + 0.5f);
        std::set<std::pair<int, int>> stroke;
        for (const auto& [cx, cy] : filled) {
            for (int dx = -out_radius; dx <= out_radius; ++dx) {
                for (int dy = -out_radius; dy <= out_radius; ++dy) {
                    if (dx * dx + dy * dy <= r2) {
                        int nx = cx + dx, ny = cy + dy;
                        if (in_bounds(nx, ny)) stroke.insert({nx, ny});
                    }
                }
            }
        }
        for (const auto& c : stroke) {
            if (filled.count(c)) {
                above_out.emplace_back(glm::ivec2(c.first, c.second), eff.color);
            } else {
                below_out.emplace_back(glm::ivec2(c.first, c.second), eff.color);
            }
        }
    }
    // Unrecognized effect types are a no-op, matching the base LayerEffect.
}

} // namespace detail

/**
 * @brief Composites one frame's visible layers into a width*height buffer of
 *        premultiplied-alpha packed RGBA8, top-left origin (matching .pix's
 *        own coordinate convention — the caller flips to +Y-up world space).
 *
 * @param apply_effects Runs the stroke effect when true (the sidecar's
 *        default); false skips straight to compositing raw layer pixels,
 *        useful for diagnosing an editor/engine mismatch.
 */
inline std::vector<uint32_t> composite_frame(int width, int height, const PixFrame& frame,
                                             bool apply_effects = true) {
    std::vector<uint32_t> canvas(static_cast<size_t>(width) * height, pack_rgba8(0, 0, 0, 0));
    if (width <= 0 || height <= 0) return canvas;

    auto plot = [&](const glm::ivec2& p, uint32_t color, float layer_opacity) {
        if (p.x < 0 || p.x >= width || p.y < 0 || p.y >= height) return;
        uint32_t src = scale_alpha(color, layer_opacity);
        size_t idx = static_cast<size_t>(p.y) * width + p.x;
        canvas[idx] = source_over(src, canvas[idx]);
    };

    for (const auto& layer : frame.layers) {
        if (!layer.visible || layer.opacity <= 0.0f) continue;

        detail::PixelList below, above;
        if (apply_effects) {
            for (const auto& eff : layer.effects) {
                detail::apply_stroke_effect(eff, layer.pixels, width, height, below, above);
            }
        }

        for (const auto& [pos, c] : below) plot(pos, c, layer.opacity);
        for (const auto& [pos, c] : layer.pixels) plot(pos, c, layer.opacity);
        for (const auto& [pos, c] : above) plot(pos, c, layer.opacity);
    }

    for (uint32_t& c : canvas) c = premultiply(c);
    return canvas;
}

/**
 * @struct TrimmedImage
 * @brief A composited frame cropped to its opaque bounding box.
 */
struct TrimmedImage {
    std::vector<uint32_t> pixels; ///< row-major, premultiplied RGBA8, width*height
    int width = 0;
    int height = 0;
    glm::ivec2 offset{0, 0};      ///< trim offset from the untrimmed canvas's top-left
};

/**
 * @brief Crops a composited canvas to its opaque (alpha > 0) bounding box.
 *
 * An all-transparent frame trims to a 1x1 transparent pixel, never a 0x0
 * rect — a zero-area atlas entry is unrepresentable geometry downstream.
 */
inline TrimmedImage trim_opaque(const std::vector<uint32_t>& canvas, int width, int height) {
    int min_x = width, min_y = height, max_x = -1, max_y = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (rgba8_a(canvas[static_cast<size_t>(y) * width + x]) > 0) {
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
    }

    TrimmedImage out;
    if (max_x < min_x) {
        out.width = 1;
        out.height = 1;
        out.offset = glm::ivec2(0, 0);
        out.pixels = {pack_rgba8(0, 0, 0, 0)};
        return out;
    }

    out.width = max_x - min_x + 1;
    out.height = max_y - min_y + 1;
    out.offset = glm::ivec2(min_x, min_y);
    out.pixels.resize(static_cast<size_t>(out.width) * out.height);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            out.pixels[static_cast<size_t>(y) * out.width + x] =
                canvas[static_cast<size_t>(y + min_y) * width + (x + min_x)];
        }
    }
    return out;
}

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_PIX_PIX_COMPOSITE_H
