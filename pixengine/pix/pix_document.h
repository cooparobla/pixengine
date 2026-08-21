/**
 * @file pix_document.h
 * @brief In-memory representation of a decoded .pix (coopixel) document.
 *
 * Mirrors coopixel's own schema (see coopixel/src/coopixel/models/document.py)
 * one-to-one: PixDocument -> PixAnimation -> PixFrame -> PixLayer -> PixEffect.
 * Pure data, no Vulkan, no CAML — pix_decoder.h fills this from an fkyaml
 * tree, pix_composite.h consumes it.
 */

#ifndef PIXENGINE_PIX_PIX_DOCUMENT_H
#define PIXENGINE_PIX_PIX_DOCUMENT_H

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace coopa {
namespace pix {

/**
 * @struct PixEffect
 * @brief A layer effect. coopixel implements exactly one type ("stroke");
 *        unrecognized types are kept around (type name + fields) but produce
 *        no pixels, matching the base LayerEffect.render_effect() no-op.
 */
struct PixEffect {
    std::string type = "stroke";
    bool        enabled = true;
    int         size = 1;               ///< clamped [1,10] by coopixel; not re-clamped here
    uint32_t    color = 0xFF000000u;     ///< packed RGBA8 (opaque black), see color.h
    std::string position = "outside";    ///< "outside" | "inside" | "center"
};

/**
 * @struct PixLayer
 * @brief One layer within a frame.
 *
 * Pixels are a flat, (x,y)-sorted vector rather than a hash map: decode fills
 * it once (from coopixel's sparse "x,y" -> "#RRGGBBAA" dict) and composite
 * only ever iterates it — a 128x128 frame is ~16k entries, and a sorted
 * vector is both cheaper to build and cache-friendlier to walk than a map.
 */
struct PixLayer {
    std::string name = "Layer";
    bool  visible = true;
    bool  locked = false;
    float opacity = 1.0f;

    /// (x, y) -> packed straight-alpha RGBA8. Sorted by (y, x) ascending.
    std::vector<std::pair<glm::ivec2, uint32_t>> pixels;

    std::vector<PixEffect> effects;
};

/// @brief One frame of an animation: an ordered stack of layers plus playback duration.
struct PixFrame {
    std::string name = "Frame";
    int duration_ms = 100;
    std::vector<PixLayer> layers;
};

/// @brief A named sequence of frames.
struct PixAnimation {
    std::string name = "new-animation";
    int fps = 10;
    std::vector<PixFrame> frames;
};

/// @brief The whole decoded document: canvas size plus every animation.
struct PixDocument {
    int width = 32;
    int height = 32;
    std::vector<PixAnimation> animations;
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_PIX_PIX_DOCUMENT_H
