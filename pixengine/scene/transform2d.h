/**
 * @file transform2d.h
 * @brief A 2D position/rotation/scale component, resolved top-down once per frame.
 *
 * libcoopa's own TransformComponent is 3D (glm::vec3 position, Euler-degrees
 * rotation, glm::mat4 world matrix via atomic dirty-flag propagation) — wrong
 * on every axis for a 2D engine: sprites need cheap world-Y as a plain float
 * for y-sorting, pixel snapping, and a pivot, none of which the 3D transform
 * has a place for. Scenes using pixengine MUST set `auto_transform: false`
 * (see register.h) so SceneLoader never attaches a TransformComponent, and
 * every positioned object declares `- type: Transform2D` instead — exactly
 * the precedent uicoopa's RectTransform already set.
 *
 * Composition is one O(n) top-down DFS (resolve_transforms()), not per-node
 * dirty flags: with auto_transform off, SceneObject::add_child() links no
 * parent transform for us anyway, so there is no cheaper incremental scheme
 * available without re-inventing parent pointers.
 *
 * Deliberately has NO pixel_snap field. pixengine renders sprites/tiles at
 * their plain continuous world_position, native resolution, no low-res
 * offscreen buffer or integer upscale -- matching a typical 2D pixel-art
 * project without a Pixel Perfect Camera. Earlier revisions of this engine
 * did snap positions to a virtual pixel grid (camera-relative, to avoid
 * shimmer under an integer-upscaled low-res target); that whole pipeline was
 * removed in favor of native-resolution rendering, so there is nothing left
 * to snap against.
 */

#ifndef PIXENGINE_SCENE_TRANSFORM2D_H
#define PIXENGINE_SCENE_TRANSFORM2D_H

#include <cmath>
#include <glm/glm.hpp>
#include <string>

#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

namespace coopa {
namespace pix {

/**
 * @class Transform2D
 * @brief Local position/rotation/scale, plus the world-space values
 *        resolve_transforms() writes once per frame.
 */
class Transform2D : public coopa::scene::Component {
public:
    glm::vec2 position{0.0f};
    float     rotation = 0.0f; ///< Degrees, counter-clockwise.
    glm::vec2 scale{1.0f};

    // Written by resolve_transforms(); treat as read-only elsewhere.
    glm::vec2 world_position{0.0f};
    float     world_rotation = 0.0f;
    glm::vec2 world_scale{1.0f};

    std::string type_name() const override { return "Transform2D"; }
};

namespace detail {

struct WorldState2D {
    glm::vec2 position{0.0f};
    float     rotation = 0.0f;
    glm::vec2 scale{1.0f};
};

inline void resolve_recursive(coopa::scene::SceneObject& obj, const WorldState2D& parent_world) {
    // Matches SceneObject::update()/late_update()'s subtree pruning: an
    // inactive object's descendants are never resolved (or rendered), so
    // there's nothing to compute for them this frame.
    if (!obj.active()) return;

    WorldState2D world = parent_world;

    if (auto* t = obj.get_component<Transform2D>()) {
        float rad = parent_world.rotation * (3.14159265358979323846f / 180.0f);
        float c = std::cos(rad), s = std::sin(rad);
        glm::vec2 scaled_local = t->position * parent_world.scale;
        glm::vec2 rotated{scaled_local.x * c - scaled_local.y * s,
                          scaled_local.x * s + scaled_local.y * c};

        world.position = parent_world.position + rotated;
        world.rotation = parent_world.rotation + t->rotation;
        world.scale = parent_world.scale * t->scale;

        t->world_position = world.position;
        t->world_rotation = world.rotation;
        t->world_scale = world.scale;
    }
    // Objects without a Transform2D are transparent group nodes: `world` was
    // seeded from parent_world above and is passed to children unchanged.

    for (auto& child : obj.children()) {
        resolve_recursive(*child, world);
    }
}

} // namespace detail

/**
 * @brief Resolves every Transform2D's world_position/world_rotation/world_scale
 *        from the scene hierarchy, top-down, in one pass.
 *
 * Call once per frame, after gameplay code has finished moving anything
 * (Component::update()) and before anything reads world_position/rotation/
 * scale (sprite emission, camera math, residency culling).
 */
inline void resolve_transforms(coopa::scene::Scene& scene) {
    detail::WorldState2D identity{};
    for (auto& root : scene.root_objects()) {
        detail::resolve_recursive(*root, identity);
    }
}

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_SCENE_TRANSFORM2D_H
