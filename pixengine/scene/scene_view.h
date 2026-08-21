/**
 * @file scene_view.h
 * @brief A cached, single-DFS view over the pixengine components in a Scene.
 *
 * coopa::scene::Scene::get_components<T>() walks the WHOLE hierarchy with a
 * dynamic_cast per component and heap-allocates a fresh std::vector on
 * EVERY call (see coopa/scene/scene.h) -- fine for a one-off query, ruinous
 * called three times a frame across a Stardew-scale object count. SceneView
 * does one DFS resolving Transform2D/SpriteRenderer/SpriteAnimator/Camera2D
 * together and caches the result; call invalidate() whenever the tree's
 * structure changes (spawn, destroy, reparent) -- Scene has no structural-
 * change signal of its own, so this is a contract every future spawner must
 * honor. A SceneView used after its Scene destroys an object it cached is a
 * use-after-free; there is no automatic protection against this.
 */

#ifndef PIXENGINE_SCENE_SCENE_VIEW_H
#define PIXENGINE_SCENE_SCENE_VIEW_H

#include <vector>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <pixengine/render/sprite_draw_list.h>
#include <pixengine/scene/camera2d.h>
#include <pixengine/scene/sprite_animator.h>
#include <pixengine/scene/sprite_renderer.h>
#include <pixengine/scene/tilemap_renderer.h>
#include <pixengine/scene/transform2d.h>

namespace coopa {
namespace pix {

/// @brief One sprite-bearing object's resolved components, ready to emit.
struct SpriteRef {
    coopa::scene::SceneObject* obj = nullptr;
    Transform2D*    transform = nullptr;
    SpriteRenderer* renderer = nullptr;
    SpriteAnimator* animator = nullptr; ///< May be nullptr -- a static (unanimated) sprite.

    void emit(SpriteDrawList& list, bool debug_show_unloaded = false) const {
        if (!renderer) return;
        uint32_t frame_index = animator ? animator->current_frame_index() : 0;
        renderer->emit(list, frame_index, debug_show_unloaded);
    }
};

/**
 * @class SceneView
 * @brief Caches SpriteRef entries and the active Camera2D for one Scene.
 */
class SceneView {
public:
    explicit SceneView(coopa::scene::Scene& scene) : scene_(scene) {}

    /// @brief Marks the cache stale; call on spawn/destroy/reparent of any tracked object.
    void invalidate() { dirty_ = true; }

    /// @brief Rebuilds the cache if invalidate() was called (or this is the first call).
    void refresh_if_dirty() {
        if (!dirty_) return;
        sprites_.clear();
        tilemaps_.clear();
        camera_ = nullptr;

        for (auto& root : scene_.root_objects()) {
            gather_(*root);
        }
        dirty_ = false;
    }

    const std::vector<SpriteRef>&           sprite_renderers() const { return sprites_; }
    const std::vector<TilemapRenderer*>&    tilemaps()         const { return tilemaps_; }
    Camera2D* active_camera() const { return camera_; }

private:
    void gather_(coopa::scene::SceneObject& obj) {
        // Mirrors Scene::get_components<T>()'s existing (if slightly odd)
        // convention: an inactive object contributes nothing itself, but its
        // active descendants are still walked and collected.
        if (obj.active()) {
            if (auto* sr = obj.get_component<SpriteRenderer>()) {
                SpriteRef ref;
                ref.obj = &obj;
                ref.transform = obj.get_component<Transform2D>();
                ref.renderer = sr;
                ref.animator = obj.get_component<SpriteAnimator>();
                if (ref.transform) sprites_.push_back(ref);
            }
            if (auto* tr = obj.get_component<TilemapRenderer>()) {
                tilemaps_.push_back(tr);
            }
            if (!camera_) {
                camera_ = obj.get_component<Camera2D>();
            }
        }
        for (auto& child : obj.children()) {
            gather_(*child);
        }
    }

    coopa::scene::Scene& scene_;
    bool dirty_ = true;
    std::vector<SpriteRef> sprites_;
    std::vector<TilemapRenderer*> tilemaps_;
    Camera2D* camera_ = nullptr;
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_SCENE_SCENE_VIEW_H
