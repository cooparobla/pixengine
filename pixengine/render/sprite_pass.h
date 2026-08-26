/**
 * @file sprite_pass.h
 * @brief Owns the sprite graphics pipeline, per-frame streaming geometry
 *        buffers, and the TextureView -> descriptor-set cache for every
 *        sprite atlas drawn.
 *
 * A near-clone of uicoopa/render/ui_pass.h with the differences a world-space,
 * pixel-art sprite pass needs: Sampler::nearest (UiPass hard-codes linear,
 * which blurs pixel art), BlendMode::PremultipliedAlpha (the sprite atlas is
 * stored premultiplied — see pix_composite.h), a camera push constant instead
 * of inv_canvas_size, and no clip-rect scissoring (SpriteDrawList has no clip
 * stack; culling happens earlier, against the camera's visible world rect).
 * Draws directly into the swapchain render pass at native resolution — the
 * same pass UiPass draws into — rather than a separate low-res offscreen
 * target; draw() sets its own viewport/scissor since nothing upstream does
 * anymore. The viewport is a caller-supplied rect (see pixel_math.h's
 * compute_fit_viewport()), not necessarily the full framebuffer -- the world
 * renders into a centered, best-fit-scaled sub-rect of the window, letting
 * a reference resolution's field of view stay constant independent of the
 * actual window size; UiPass separately draws HUD content at full native
 * resolution, unscaled.
 *
 * Same hard constraint as UiPass: DescriptorSet::bind_image() calls
 * updates descriptor sets immediately, which is unsafe once a render pass is
 * open. register_textures() MUST run before Renderer::begin_frame().
 */

#ifndef PIXENGINE_RENDER_SPRITE_PASS_H
#define PIXENGINE_RENDER_SPRITE_PASS_H

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/presentation/renderer.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/sampler_desc.h>
#include <gfxcoopa/types/texture_view.h>

#include <pixengine/math/pixel_math.h>
#include <pixengine/render/sprite_vertex.h>
#include <pixengine/render/sprite_draw_list.h>

namespace coopa {
namespace pix {

/// @brief Matches the `Push` block declared in sprite.vert/sprite.frag exactly.
struct SpritePush {
    float inv_half_extent[2];
    float camera_pos[2];
    float tint[4];
};

/**
 * @class SpritePass
 * @brief Records the world-space sprite draw calls for one frame into the
 *        swapchain render pass, at native resolution.
 */
class SpritePass {
public:
    static constexpr uint32_t kFrames            = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;
    static constexpr uint32_t kInitialMaxVerts   = 4096;
    static constexpr uint32_t kInitialMaxIndices = 6144;

    /**
     * @param device        Logical device.
     * @param allocator     VMA allocator.
     * @param cmd_pool      Command pool for the fallback texture's one-shot upload.
     * @param swapchain_pass The swapchain render pass — the SAME one UiPass draws
     *                      sprites and UI into, sprites first.
     * @param vert_spv      Path to sprite.vert.spv.
     * @param frag_spv      Path to sprite.frag.spv.
     * @param max_textures  Upper bound on distinct atlases drawn in a single frame.
     */
    SpritePass(coopa::gfx::core::Device& device,
              coopa::gfx::memory::Allocator& allocator,
              coopa::gfx::command::CommandPool& cmd_pool,
              coopa::gfx::pipeline::RenderPass& swapchain_pass,
              const std::string& vert_spv,
              const std::string& frag_spv,
              uint32_t max_textures = 256)
        : device_(device), allocator_(&allocator)
    {
        using namespace coopa::gfx;

        vert_shader_ = std::make_unique<pipeline::Shader>(device, vert_spv, ShaderStage::Vertex);
        frag_shader_ = std::make_unique<pipeline::Shader>(device, frag_spv, ShaderStage::Fragment);

        desc_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
            pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, ShaderStage::Fragment)
                .build(device));

        desc_pool_ = std::make_unique<pipeline::DescriptorPool>(
            pipeline::DescriptorPoolBuilder().add_sets(*desc_layout_, max_textures).build(device));

        pipeline::PipelineDesc desc;
        desc.shaders = {vert_shader_.get(), frag_shader_.get()};
        desc.vertex  = SpriteVertex::layout();
        desc.descriptor_layouts = {desc_layout_.get()};
        desc.push_constants = { { ShaderStage::Vertex | ShaderStage::Fragment, 0, sizeof(SpritePush) } };
        desc.raster.cull = CullMode::None;
        desc.depth.test  = false;
        desc.depth.write = false;
        desc.blend.mode  = pipeline::BlendMode::PremultipliedAlpha;

        pipeline_ = std::make_unique<pipeline::Pipeline>(device, swapchain_pass, desc);

        sampler_ = std::make_unique<coopa::gfx::engine::util::Sampler>(
            coopa::gfx::engine::util::Sampler::nearest(device));

        // 1x1 opaque magenta: bound in place of any texture a caller forgot to
        // register_textures() before this frame's draw() -- loud on purpose,
        // never a silent crash (see file header + register_textures() doc).
        std::array<uint8_t, 4> magenta_px{255, 0, 255, 255};
        fallback_texture_ = std::make_unique<coopa::gfx::engine::data::Texture>(
            coopa::gfx::engine::data::Texture::upload(
                device, allocator, cmd_pool, magenta_px.data(), 1, 1,
                Format::RGBA8_Unorm, SamplerDesc::pixel_art()));

        for (uint32_t i = 0; i < kFrames; ++i) {
            vbo_[i] = std::make_unique<coopa::gfx::memory::Buffer>(
                coopa::gfx::memory::Buffer::vertex(device, allocator, kInitialMaxVerts * sizeof(SpriteVertex)));
            ibo_[i] = std::make_unique<coopa::gfx::memory::Buffer>(
                coopa::gfx::memory::Buffer::index(device, allocator, kInitialMaxIndices * sizeof(uint32_t)));
            vbo_capacity_[i] = kInitialMaxVerts;
            ibo_capacity_[i] = kInitialMaxIndices;
        }

        register_view_(fallback_texture_->view_typed());
    }

    /**
     * @brief Resolves every atlas referenced by draw_list's batches into a descriptor set.
     *
     * MUST be called before Renderer::begin_frame() (see file header).
     * Already-registered views are skipped, so calling every frame is cheap.
     */
    void register_textures(const SpriteDrawList& draw_list) {
        for (const SpriteBatch& batch : draw_list.batches()) {
            register_view_(batch.texture_view);
        }
    }

    /**
     * @brief Records this frame's sprite draw calls into an already-open render pass.
     * @param cmd         Command buffer, mid-recording inside the swapchain render pass.
     * @param frame_index renderer.current_frame() — selects this frame's geometry buffers.
     * @param push        Camera + tint push constants (see SpritePush).
     * @param draw_list   This frame's batched sprite geometry.
     * @param viewport    Destination rect from pixel_math.h's compute_fit_viewport() --
     *                    the world renders into this centered, best-fit-scaled sub-rect
     *                    of the window, not necessarily the full framebuffer.
     * @param screen_w    Framebuffer width in pixels -- clamps the scissor.
     * @param screen_h    Framebuffer height in pixels.
     */
    void draw(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_index,
              const SpritePush& push, const SpriteDrawList& draw_list,
              const FitViewport& viewport, uint32_t screen_w, uint32_t screen_h) {
        if (draw_list.indices().empty()) return;

        // Clamp the scissor to the framebuffer -- viewport.x/y are already
        // >= 0 by compute_fit_viewport()'s construction, but the rect can
        // still extend past the far edge from float rounding at extreme
        // aspect ratios; a scissor offset can never be negative either way.
        int32_t x0 = std::max(static_cast<int32_t>(viewport.x), 0);
        int32_t y0 = std::max(static_cast<int32_t>(viewport.y), 0);
        int32_t x1 = std::min(static_cast<int32_t>(viewport.x + viewport.w), static_cast<int32_t>(screen_w));
        int32_t y1 = std::min(static_cast<int32_t>(viewport.y + viewport.h), static_cast<int32_t>(screen_h));
        uint32_t clipped_w = x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0;
        uint32_t clipped_h = y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0;
        if (clipped_w == 0 || clipped_h == 0) return;

        ensure_capacity_(frame_index, draw_list);

        vbo_[frame_index]->upload(draw_list.vertices().data(), draw_list.vertices().size() * sizeof(SpriteVertex));
        ibo_[frame_index]->upload(draw_list.indices().data(), draw_list.indices().size() * sizeof(uint32_t));

        cmd.bind_pipeline(*pipeline_);
        cmd.set_viewport(viewport.x, viewport.y, viewport.w, viewport.h);
        cmd.set_scissor(x0, y0, clipped_w, clipped_h);
        cmd.bind_vertex_buffer(*vbo_[frame_index]);
        cmd.bind_index_buffer(*ibo_[frame_index]);
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, push);

        for (const SpriteBatch& batch : draw_list.batches()) {
            if (batch.index_count == 0) continue;

            auto it = descriptor_cache_.find(batch.texture_view);
            if (it == descriptor_cache_.end()) {
                it = descriptor_cache_.find(fallback_texture_->view_typed());
            }
            cmd.bind_descriptor_set(*it->second);
            cmd.draw_indexed(batch.index_count, batch.first_index, 0, 1);
        }
    }

private:
    void register_view_(coopa::gfx::TextureView view) {
        if (descriptor_cache_.count(view)) return;
        auto set = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *desc_pool_, *desc_layout_);
        set->bind_image(0, view, *sampler_);
        descriptor_cache_[view] = std::move(set);
    }

    void ensure_capacity_(uint32_t frame_index, const SpriteDrawList& draw_list) {
        size_t needed_verts = draw_list.vertices().size();
        size_t needed_indices = draw_list.indices().size();
        if (needed_verts > vbo_capacity_[frame_index]) {
            size_t new_capacity = std::max(needed_verts, static_cast<size_t>(vbo_capacity_[frame_index]) * 2);
            vbo_[frame_index].reset();
            vbo_[frame_index] = std::make_unique<coopa::gfx::memory::Buffer>(
                coopa::gfx::memory::Buffer::vertex(device_, *allocator_, new_capacity * sizeof(SpriteVertex)));
            vbo_capacity_[frame_index] = static_cast<uint32_t>(new_capacity);
        }
        if (needed_indices > ibo_capacity_[frame_index]) {
            size_t new_capacity = std::max(needed_indices, static_cast<size_t>(ibo_capacity_[frame_index]) * 2);
            ibo_[frame_index].reset();
            ibo_[frame_index] = std::make_unique<coopa::gfx::memory::Buffer>(
                coopa::gfx::memory::Buffer::index(device_, *allocator_, new_capacity * sizeof(uint32_t)));
            ibo_capacity_[frame_index] = static_cast<uint32_t>(new_capacity);
        }
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator* allocator_;

    std::unique_ptr<coopa::gfx::pipeline::Shader>              vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader>              frag_shader_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> desc_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      desc_pool_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline>            pipeline_;
    std::unique_ptr<coopa::gfx::engine::util::Sampler>         sampler_;
    std::unique_ptr<coopa::gfx::engine::data::Texture>         fallback_texture_;

    std::unordered_map<coopa::gfx::TextureView, std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>> descriptor_cache_;

    std::unique_ptr<coopa::gfx::memory::Buffer> vbo_[kFrames];
    std::unique_ptr<coopa::gfx::memory::Buffer> ibo_[kFrames];
    uint32_t vbo_capacity_[kFrames] = {};
    uint32_t ibo_capacity_[kFrames] = {};
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_RENDER_SPRITE_PASS_H
