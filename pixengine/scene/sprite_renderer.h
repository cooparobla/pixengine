/**
 * @file sprite_renderer.h
 * @brief Draws one frame of a SpriteAsset at its owner's resolved Transform2D.
 *
 * Deliberately does not know about SpriteAnimator: emit() takes an explicit
 * frame_index rather than looking up a sibling animator itself, so this
 * header never needs to include sprite_animator.h (which, in turn, DOES
 * depend on this one, to read animation data off SpriteRenderer::handle —
 * a cycle either way round would force, so the dependency runs one
 * direction only: SceneView (scene_view.h) is what actually bundles a
 * SpriteRenderer with its optional sibling SpriteAnimator and picks which
 * frame to pass in.
 */

#ifndef PIXENGINE_SCENE_SPRITE_RENDERER_H
#define PIXENGINE_SCENE_SPRITE_RENDERER_H

#include <cstdint>
#include <glm/glm.hpp>
#include <string>

#include <coopa/asset/asset_handle.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>

#include <pixengine/data/sprite_asset.h>
#include <pixengine/render/sprite_draw_list.h>
#include <pixengine/render/sprite_vertex.h>
#include <pixengine/scene/transform2d.h>

namespace coopa {
namespace pix {

/**
 * @class SpriteRenderer
 * @brief Per-object sprite draw parameters, plus the loaded asset handle.
 */
class SpriteRenderer : public coopa::scene::Component {
public:
    std::string sprite;               ///< Authored path, e.g. "sprites/player.pix".
    glm::vec4   color{1.0f, 1.0f, 1.0f, 1.0f}; ///< Straight-alpha RGBA tint.
    int         sort_layer = 0;
    int         order_in_layer = 0;
    bool        y_sort = false;
    float       y_sort_offset = 0.0f;
    glm::vec2   cull_size{2.0f, 2.0f}; ///< Residency estimate before the asset resolves (Phase 7).
    bool        flip_x = false;
    bool        flip_y = false;

    /// Set by register.h's YAML parser (Phase 5: a synchronous assets.load()
    /// at parse time; Phase 7 replaces this with streamed acquire/release).
    coopa::asset::AssetHandle<SpriteAsset> handle;

    std::string type_name() const override { return "SpriteRenderer"; }

    /**
     * @brief Appends this sprite's world-space quad to the draw list.
     *
     * No-op if the owner (or an ancestor's active() -- callers filter that,
     * see scene_view.h), the asset, or a Transform2D sibling isn't ready.
     *
     * @param frame_index Index into asset->frames(); the caller resolves this
     *                     from a sibling SpriteAnimator if present, else 0.
     * @param debug_show_unloaded When true, an unloaded/loading handle draws a
     *                     cull_size magenta placeholder instead of nothing --
     *                     lets Phase 7's residency streaming boundary be seen
     *                     directly rather than trusted on faith. Uses
     *                     VK_NULL_HANDLE as the "texture", which SpritePass
     *                     already renders as its own unregistered-texture
     *                     fallback (also magenta) -- no separate plumbing needed.
     *                     Default off: with a two-screen prefetch margin and an
     *                     acquire budget, a sprite is essentially never
     *                     simultaneously visible and unloaded, so a flashing
     *                     placeholder would be strictly worse than a one-frame pop.
     *
     * Renders at the object's plain continuous world position -- no
     * pixel-grid snapping. pixengine targets native-resolution rendering
     * (no low-res offscreen buffer + integer upscale), matching a typical 2D
     * pixel-art project without a Pixel Perfect Camera: sprites move
     * smoothly, accepting whatever nearest-neighbor sub-pixel sampling
     * variance results, rather than snapping to a virtual pixel grid.
     */
    void emit(SpriteDrawList& list, uint32_t frame_index, bool debug_show_unloaded = false) const {
        if (!owner) return;
        const Transform2D* tf = owner->get_component<Transform2D>();
        if (!tf) return;

        glm::vec2 render_pos = tf->world_position;

        if (!handle.is_loaded()) {
            if (debug_show_unloaded) {
                coopa::ui::Rect full_uv{{0.0f, 0.0f}, {1.0f, 1.0f}};
                list.add_sprite(VK_NULL_HANDLE, render_pos, cull_size, glm::vec2(0.5f, 0.5f),
                                tf->world_rotation, glm::vec2(1.0f, 1.0f), full_uv,
                                SpriteVertex::pack_color(1.0f, 0.0f, 1.0f, 1.0f),
                                sort_layer, y_sort, render_pos.y + y_sort_offset,
                                static_cast<uint32_t>(order_in_layer));
            }
            return;
        }
        const SpriteAsset* asset = handle.get();
        if (asset->frames().empty()) return;
        if (frame_index >= asset->frames().size()) frame_index = 0;

        const SpriteFrame& frame = asset->frame(frame_index);
        float ppu = asset->pixels_per_unit();
        if (ppu <= 0.0f) return;

        glm::vec2 size_units = (frame.size_px / ppu) * tf->world_scale;
        glm::vec2 flip{flip_x ? -1.0f : 1.0f, flip_y ? -1.0f : 1.0f};
        float world_y = render_pos.y + y_sort_offset;

        list.add_sprite(asset->atlas_view(), render_pos, size_units,
                        frame.pivot_norm, tf->world_rotation, flip, frame.uv,
                        SpriteVertex::pack_color(color.r, color.g, color.b, color.a),
                        sort_layer, y_sort, world_y, static_cast<uint32_t>(order_in_layer));
    }
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_SCENE_SPRITE_RENDERER_H
