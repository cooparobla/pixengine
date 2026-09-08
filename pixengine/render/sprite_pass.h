/**
 * @file sprite_pass.h
 * @brief Owns the sprite graphics pipeline, per-frame streaming geometry
 *        buffers, and the TextureView -> descriptor-set cache for every
 *        sprite atlas drawn.
 *
 * Thin wrapper around gfxcoopa's TexturedQuad2DPass (engine/passes/textured_quad_2d_pass.h)
 * -- the descriptor layout/pool, TextureView->DescriptorSet cache, and streaming
 * vertex/index buffers are shared with uicoopa's UiPass now; this file only owns what's
 * genuinely sprite-specific: the world-space camera push constant (vs. UiPass's
 * canvas-space one), the caller-supplied best-fit viewport/scissor (set once per frame, not
 * per batch -- SpriteDrawList has no clip stack, culling happens earlier against the
 * camera's visible world rect), and the premultiplied-alpha / nearest-filter combination
 * pixel art needs (vs. UiPass's straight-alpha / bilinear).
 *
 * Draws directly into the swapchain render pass at native resolution — the
 * same pass UiPass draws into — rather than a separate low-res offscreen
 * target. The viewport is a caller-supplied rect (see pixel_math.h's
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
#include <cstdint>
#include <memory>
#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/passes/textured_quad_2d_pass.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/sampler_desc.h>

#include <pixengine/math/pixel_math.h>
#include <pixengine/render/sprite_vertex.h>
#include <pixengine/render/sprite_draw_list.h>

namespace coopa {
namespace pix {

/**
 * @brief Matches the `Push` block declared in sprite.vert/sprite.frag exactly -- see
 *        gfx/surface2d/quad_vs.glsl's doc for the scale/offset prefix every 2D quad pass
 *        shares. `scale`/`offset` here ARE `inv_half_extent`/`-camera_pos*inv_half_extent`
 *        (see SpritePass::draw()'s doc) -- the caller no longer passes camera_pos directly.
 */
struct SpritePush {
    float scale[2];
    float offset[2];
    float tint[4];
};

/**
 * @class SpritePass
 * @brief Records the world-space sprite draw calls for one frame into the
 *        swapchain render pass, at native resolution.
 */
class SpritePass {
public:
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
    {
        using namespace coopa::gfx;
        using namespace coopa::gfx::engine::passes;

        TexturedQuad2DDesc desc;
        desc.vertex             = SpriteVertex::layout();
        desc.vertex_stride      = sizeof(SpriteVertex);
        desc.blend_mode         = pipeline::BlendMode::PremultipliedAlpha;
        desc.push_constant_size = sizeof(SpritePush);
        // Nearest, not linear -- pixel art. Unified onto SamplerDesc::pixel_art() (was
        // Sampler::nearest(device) here but SamplerDesc::pixel_art() for the fallback
        // texture below -- two names for what should be one setting; TexturedQuad2DDesc
        // takes a single SamplerDesc for both the sampler and the fallback texture, which
        // forces that consistency).
        desc.sampler_desc   = coopa::gfx::SamplerDesc::pixel_art();
        desc.fallback_pixel = {255, 0, 255, 255}; // opaque magenta -- unmistakable against pixel art
        desc.initial_max_verts   = kInitialMaxVerts;
        desc.initial_max_indices = kInitialMaxIndices;
        desc.max_textures        = max_textures;

        pass_ = std::make_unique<TexturedQuad2DPass>(
            device, allocator, cmd_pool, swapchain_pass, vert_spv, frag_spv, desc);
    }

    /**
     * @brief Resolves every atlas referenced by draw_list's batches into a descriptor set.
     *
     * MUST be called before Renderer::begin_frame() (see file header).
     * Already-registered views are skipped, so calling every frame is cheap.
     */
    void register_textures(const SpriteDrawList& draw_list) {
        for (const SpriteBatch& batch : draw_list.batches()) {
            pass_->register_view(batch.texture_view);
        }
    }

    /**
     * @brief Records this frame's sprite draw calls into an already-open render pass.
     * @param cmd          Command buffer, mid-recording inside the swapchain render pass.
     * @param frame_index  renderer.current_frame() — selects this frame's geometry buffers.
     * @param push         Camera (as scale/offset -- see SpritePush's own doc) + tint push
     *                     constants. Caller fills `scale = inv_half_extent`,
     *                     `offset = -camera_pos * inv_half_extent`.
     * @param draw_list    This frame's batched sprite geometry.
     * @param viewport     Destination rect from pixel_math.h's compute_fit_viewport() --
     *                     the world renders into this centered, best-fit-scaled sub-rect
     *                     of the window, not necessarily the full framebuffer.
     * @param screen_w     Framebuffer width in pixels -- clamps the scissor.
     * @param screen_h     Framebuffer height in pixels.
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

        pass_->ensure_capacity(frame_index, draw_list.vertices().size(), draw_list.indices().size());
        pass_->vertex_buffer(frame_index).upload(draw_list.vertices().data(), draw_list.vertices().size() * sizeof(SpriteVertex));
        pass_->index_buffer(frame_index).upload(draw_list.indices().data(), draw_list.indices().size() * sizeof(uint32_t));

        pass_->bind(cmd);
        cmd.set_viewport(viewport.x, viewport.y, viewport.w, viewport.h);
        cmd.set_scissor(x0, y0, clipped_w, clipped_h);
        cmd.bind_vertex_buffer(pass_->vertex_buffer(frame_index));
        cmd.bind_index_buffer(pass_->index_buffer(frame_index));
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, push);

        for (const SpriteBatch& batch : draw_list.batches()) {
            if (batch.index_count == 0) continue;
            cmd.bind_descriptor_set(pass_->descriptor_set_for(batch.texture_view));
            cmd.draw_indexed(batch.index_count, batch.first_index, 0, 1);
        }
    }

private:
    static constexpr uint32_t kInitialMaxVerts   = 4096;
    static constexpr uint32_t kInitialMaxIndices = 6144;

    std::unique_ptr<coopa::gfx::engine::passes::TexturedQuad2DPass> pass_;
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_RENDER_SPRITE_PASS_H
