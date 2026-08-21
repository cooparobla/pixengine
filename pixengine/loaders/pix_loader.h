/**
 * @file pix_loader.h
 * @brief coopa::asset loader for SpriteAsset: decodes+composites+packs a
 *        .pix (off the main thread), uploads the packed atlas (on it).
 *
 * Mirrors gfxcoopa/engine/loaders/texture_loader.h's shape exactly. The
 * entire cost of parsing a .pix's sparse pixel maps, compositing every
 * frame, and shelf-packing the atlas happens in decode_typed() on the
 * AssetManager's worker thread; finalize_typed() only ever touches the GPU.
 */

#ifndef PIXENGINE_LOADERS_PIX_LOADER_H
#define PIXENGINE_LOADERS_PIX_LOADER_H

#include <volk/volk.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <memory>
#include <sstream>
#include <vector>

#include <coopa/asset/asset_loader.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/engine/data/texture.h>

#include <pixengine/pix/color.h>
#include <pixengine/pix/pix_document.h>
#include <pixengine/pix/pix_decoder.h>
#include <pixengine/pix/pix_composite.h>
#include <pixengine/pix/atlas_packer.h>
#include <pixengine/pix/sprite_meta.h>
#include <pixengine/data/sprite_asset.h>

namespace coopa {
namespace pix {

/**
 * @class PixLoader
 * @brief Registers as the coopa::asset loader for SpriteAsset.
 *
 * @code
 * assets.register_loader<coopa::pix::SpriteAsset>(
 *     std::make_unique<coopa::pix::PixLoader>(device, allocator, cmd_pool));
 * auto handle = assets.load<coopa::pix::SpriteAsset>("sprites/player.pix");
 * @endcode
 */
class PixLoader : public coopa::asset::TypedAssetLoader<SpriteAsset, DecodedPixAtlas> {
public:
    PixLoader(coopa::gfx::core::Device& device,
              coopa::gfx::memory::Allocator& allocator,
              coopa::gfx::command::CommandPool& cmd_pool)
        : device_(device), allocator_(allocator), cmd_pool_(cmd_pool) {}

    std::shared_ptr<DecodedPixAtlas> decode_typed(const coopa::asset::AssetId& id,
                                                  const coopa::asset::LoadContext& ctx) override {
        (void)id;
        PixDocument doc = load_pix_document(ctx.resolved_path);
        SpriteMeta meta = load_sidecar_(ctx.resolved_path);

        // Composite + trim every frame of every animation up front, remembering
        // which (animation, local-frame) each trimmed image came from so the
        // packer's output can be re-attached to the right SpriteFrame/SpriteAnimation.
        struct PendingFrame {
            size_t anim_index;
            size_t frame_in_anim;
            int duration_ms;
            TrimmedImage trimmed;
        };
        std::vector<PendingFrame> pending;
        std::vector<glm::ivec2> sizes;
        pending.reserve(16);
        sizes.reserve(16);

        for (size_t ai = 0; ai < doc.animations.size(); ++ai) {
            const PixAnimation& anim = doc.animations[ai];
            for (size_t fi = 0; fi < anim.frames.size(); ++fi) {
                const PixFrame& frame = anim.frames[fi];
                std::vector<uint32_t> canvas = composite_frame(doc.width, doc.height, frame, meta.apply_effects);
                TrimmedImage trimmed = trim_opaque(canvas, doc.width, doc.height);
                sizes.push_back(glm::ivec2(trimmed.width, trimmed.height));
                pending.push_back(PendingFrame{ai, fi, frame.duration_ms, std::move(trimmed)});
            }
        }

        AtlasPackResult packed = pack_shelf(sizes, /*padding=*/1, /*max_dimension=*/4096);

        auto result = std::make_shared<DecodedPixAtlas>();
        result->atlas_w = packed.atlas_width;
        result->atlas_h = packed.atlas_height;
        result->pixels.assign(static_cast<size_t>(std::max(packed.atlas_width, 0)) *
                                   static_cast<size_t>(std::max(packed.atlas_height, 0)),
                               pack_rgba8(0, 0, 0, 0));
        result->pixels_per_unit = meta.pixels_per_unit;
        result->canvas_size = glm::ivec2(doc.width, doc.height);
        result->hitboxes = meta.hitboxes;
        result->anchors = meta.anchors;
        result->frames.resize(pending.size());

        for (size_t i = 0; i < pending.size(); ++i) {
            const PendingFrame& pf = pending[i];
            const AtlasRect& rect = packed.rects[i];
            blit_(result->pixels, packed.atlas_width, packed.atlas_height, rect, pf.trimmed);
            result->frames[i] = build_frame_(doc, meta, pf.anim_index, pf.frame_in_anim,
                                             rect, pf.trimmed, pf.duration_ms,
                                             packed.atlas_width, packed.atlas_height);
        }

        size_t cursor = 0;
        for (size_t ai = 0; ai < doc.animations.size(); ++ai) {
            const PixAnimation& anim = doc.animations[ai];
            AnimMeta anim_meta = find_anim_meta(meta, anim.name);

            SpriteAnimation sa;
            sa.name = anim.name;
            sa.first_frame = static_cast<uint32_t>(cursor);
            sa.frame_count = static_cast<uint32_t>(anim.frames.size());
            sa.loop = anim_meta.loop;
            sa.speed = anim_meta.speed;
            for (const AnimEvent& ev : anim_meta.events) {
                sa.events.emplace_back(static_cast<uint32_t>(ev.frame), ev.name);
            }
            result->animations.push_back(std::move(sa));
            cursor += anim.frames.size();
        }

        return result;
    }

    std::shared_ptr<SpriteAsset> finalize_typed(std::shared_ptr<DecodedPixAtlas> decoded,
                                                const coopa::asset::AssetId&,
                                                const coopa::asset::LoadContext&) override {
        auto atlas = coopa::gfx::engine::data::Texture::upload(
            device_, allocator_, cmd_pool_,
            reinterpret_cast<const uint8_t*>(decoded->pixels.data()),
            static_cast<uint32_t>(decoded->atlas_w), static_cast<uint32_t>(decoded->atlas_h),
            /*srgb=*/false, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        return std::make_shared<SpriteAsset>(std::move(atlas), std::move(*decoded));
    }

    const char* type_name() const override { return "SpriteAsset"; }

private:
    static SpriteMeta load_sidecar_(const std::string& pix_resolved_path) {
        std::filesystem::path sidecar_path(pix_resolved_path);
        sidecar_path.replace_extension(".sprite.yaml");
        if (!std::filesystem::exists(sidecar_path)) {
            return sprite_meta_defaults();
        }
        std::ifstream file(sidecar_path, std::ios::binary);
        std::ostringstream ss;
        ss << file.rdbuf();
        return parse_sprite_meta(ss.str());
    }

    static void blit_(std::vector<uint32_t>& atlas, int atlas_w, int atlas_h,
                      const AtlasRect& rect, const TrimmedImage& trimmed) {
        (void)atlas_h;
        for (int y = 0; y < trimmed.height; ++y) {
            for (int x = 0; x < trimmed.width; ++x) {
                size_t src_idx = static_cast<size_t>(y) * trimmed.width + x;
                size_t dst_idx = static_cast<size_t>(rect.y + y) * atlas_w + (rect.x + x);
                atlas[dst_idx] = trimmed.pixels[src_idx];
            }
        }
    }

    /**
     * @brief Builds one SpriteFrame, including the pivot re-projection from
     *        the sidecar's untrimmed-canvas/top-left-origin convention into
     *        this frame's trimmed/bottom-left-origin convention (see
     *        SpriteFrame::pivot_norm's doc comment for the full derivation).
     */
    static SpriteFrame build_frame_(const PixDocument& doc, const SpriteMeta& meta,
                                    size_t anim_index, size_t frame_in_anim,
                                    const AtlasRect& rect, const TrimmedImage& trimmed,
                                    int base_duration_ms, int atlas_w, int atlas_h) {
        SpriteFrame sf;

        float u0 = (rect.x + 0.5f) / static_cast<float>(atlas_w);
        float v0 = (rect.y + 0.5f) / static_cast<float>(atlas_h);
        float u1 = (rect.x + rect.w - 0.5f) / static_cast<float>(atlas_w);
        float v1 = (rect.y + rect.h - 0.5f) / static_cast<float>(atlas_h);
        sf.uv = coopa::ui::Rect{{u0, v0}, {u1, v1}};
        sf.size_px = glm::vec2(static_cast<float>(trimmed.width), static_cast<float>(trimmed.height));

        const PixAnimation& anim = doc.animations[anim_index];
        AnimMeta anim_meta = find_anim_meta(meta, anim.name);
        glm::vec2 pivot_sidecar = anim_meta.has_pivot ? anim_meta.pivot : meta.pivot;

        // Sidecar pivot is normalized against the UNTRIMMED canvas, bottom-left
        // origin +Y up (per its doc comment, "(0.5,0) = bottom-center"); .pix
        // pixel space is top-left origin +Y down -- flip Y converting between them.
        glm::vec2 pivot_px_topleft{pivot_sidecar.x * doc.width, (1.0f - pivot_sidecar.y) * doc.height};
        glm::vec2 pivot_rel_topleft = pivot_px_topleft - glm::vec2(trimmed.offset);

        sf.pivot_norm.x = trimmed.width > 0 ? pivot_rel_topleft.x / trimmed.width : 0.5f;
        sf.pivot_norm.y = trimmed.height > 0 ? 1.0f - (pivot_rel_topleft.y / trimmed.height) : 0.0f;

        int duration_ms = base_duration_ms;
        for (const FrameOverride& fo : anim_meta.frame_overrides) {
            if (static_cast<size_t>(fo.index) == frame_in_anim && fo.duration_ms >= 0) {
                duration_ms = fo.duration_ms;
            }
        }
        sf.duration_s = duration_ms / 1000.0f;

        return sf;
    }

    coopa::gfx::core::Device&         device_;
    coopa::gfx::memory::Allocator&    allocator_;
    coopa::gfx::command::CommandPool& cmd_pool_;
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_LOADERS_PIX_LOADER_H
