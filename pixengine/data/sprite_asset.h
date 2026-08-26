/**
 * @file sprite_asset.h
 * @brief The published, GPU-resident result of loading a .pix sprite: one
 *        packed atlas texture plus per-frame/per-animation metadata.
 *
 * IMMUTABLE AFTER PUBLISH. coopa::asset::AssetHandle<T>::get() is const-only
 * by design, so every piece of MUTABLE per-instance playback state (current
 * animation, current frame index, elapsed time, ping-pong direction) belongs
 * on SpriteAnimator (Phase 5), never here — ten SpriteRenderers can share
 * one SpriteAsset and animate independently only if this class never changes
 * after PixLoader::finalize_typed() returns it.
 */

#ifndef PIXENGINE_DATA_SPRITE_ASSET_H
#define PIXENGINE_DATA_SPRITE_ASSET_H

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gfxcoopa/engine/data/texture.h>
#include <uicoopa/layout/rect.h>

#include <pixengine/pix/sprite_meta.h>

namespace coopa {
namespace pix {

/**
 * @struct SpriteFrame
 * @brief One packed frame's atlas placement and pivot, ready to hand to
 *        SpriteDrawList::add_sprite() with no further transformation.
 */
struct SpriteFrame {
    coopa::ui::Rect uv;          ///< Normalized atlas UV, half-texel inset; uv.min is the TOP of the source sprite.
    glm::vec2  size_px{0.0f};    ///< Trimmed frame size, source pixels == world units at pixels_per_unit=1.
    /// Pivot as a [0,1] fraction of size_px, BOTTOM-LEFT origin, +Y up --
    /// i.e. already in SpriteDrawList::add_sprite()'s pivot_norm convention.
    /// Derived once at load time from the sidecar's pivot (defined against
    /// the untrimmed canvas, top-left/+Y-down) re-expressed relative to this
    /// frame's trimmed bounding box; may fall outside [0,1] if the pivot
    /// point lies outside the trimmed opaque region (e.g. an intentionally
    /// off-canvas pivot), which is not an error.
    glm::vec2  pivot_norm{0.5f, 0.0f};
    float      duration_s = 0.1f;
};

/// @brief A named, contiguous run of frames within the shared frame list.
struct SpriteAnimation {
    std::string name;
    uint32_t    first_frame = 0;
    uint32_t    frame_count = 0;
    LoopMode    loop = LoopMode::Loop;
    float       speed = 1.0f;
    std::vector<std::pair<uint32_t, std::string>> events; ///< (local frame index, event name)
};

/**
 * @struct DecodedPixAtlas
 * @brief CPU-only intermediate between PixLoader::decode_typed() and
 *        finalize_typed() -- a fully packed atlas pixel buffer plus every
 *        frame/animation/hitbox/anchor derived from the .pix + sidecar.
 */
struct DecodedPixAtlas {
    std::vector<uint32_t> pixels; ///< atlas_w * atlas_h, premultiplied RGBA8 (see pix_composite.h)
    int atlas_w = 0;
    int atlas_h = 0;
    float pixels_per_unit = 16.0f;
    glm::ivec2 canvas_size{0, 0}; ///< The .pix's own (untrimmed) width/height.
    std::vector<SpriteFrame> frames;
    std::vector<SpriteAnimation> animations;
    std::vector<Hitbox> hitboxes;                                ///< Inert; sidecar-defined, engine only exposes it.
    std::unordered_map<std::string, glm::vec2> anchors;          ///< Inert; sidecar-defined, engine only exposes it.
};

/**
 * @class SpriteAsset
 * @brief The GPU-resident, immutable result of loading a .pix sprite.
 */
class SpriteAsset {
public:
    SpriteAsset(coopa::gfx::engine::data::Texture atlas, DecodedPixAtlas data)
        : atlas_(std::move(atlas)), data_(std::move(data)) {}

    coopa::gfx::TextureView atlas_view() const { return atlas_.view_typed(); }
    uint32_t atlas_width()  const { return atlas_.width(); }
    uint32_t atlas_height() const { return atlas_.height(); }

    const std::vector<SpriteFrame>& frames() const { return data_.frames; }
    const SpriteFrame& frame(size_t index) const { return data_.frames.at(index); }

    const SpriteAnimation* find_animation(std::string_view name) const {
        for (const auto& anim : data_.animations) {
            if (anim.name == name) return &anim;
        }
        return nullptr;
    }
    const SpriteAnimation* animation_at(size_t index) const {
        return index < data_.animations.size() ? &data_.animations[index] : nullptr;
    }
    size_t animation_count() const { return data_.animations.size(); }

    float pixels_per_unit() const { return data_.pixels_per_unit; }
    glm::ivec2 canvas_size() const { return data_.canvas_size; }
    const std::vector<Hitbox>& hitboxes() const { return data_.hitboxes; }
    const std::unordered_map<std::string, glm::vec2>& anchors() const { return data_.anchors; }

    // Move-only: destroying this frees the GPU image backing the atlas.
    SpriteAsset(SpriteAsset&&) = default;
    SpriteAsset& operator=(SpriteAsset&&) = default;
    SpriteAsset(const SpriteAsset&) = delete;
    SpriteAsset& operator=(const SpriteAsset&) = delete;

private:
    coopa::gfx::engine::data::Texture atlas_;
    DecodedPixAtlas data_;
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_DATA_SPRITE_ASSET_H
