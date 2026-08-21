/**
 * @file camera2d.h
 * @brief The 2D camera: follow + deadzone + bounds clamp, and the projection
 *        math the sprite/tilemap passes and residency system both need.
 */

#ifndef PIXENGINE_SCENE_CAMERA2D_H
#define PIXENGINE_SCENE_CAMERA2D_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>

#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <uicoopa/layout/rect.h>

#include <pixengine/scene/transform2d.h>

namespace coopa {
namespace pix {

/**
 * @class Camera2D
 * @brief Drives its own Transform2D's position (follow + deadzone + bounds
 *        clamp) and provides the projection math for rendering and culling.
 *
 * Reads its follow target's world_position each update() -- since
 * resolve_transforms() runs once per frame AFTER Component::update() (see
 * the per-frame main loop in demo.cpp), the target's world_position here is
 * one frame stale. With follow_lerp smoothing this is imperceptible and
 * self-corrects continuously; it is not worth restructuring the frame around
 * to eliminate for a single-frame lag.
 */
class Camera2D : public coopa::scene::Component {
public:
    float zoom = 1.0f;
    glm::vec2 offset{0.0f};
    std::string follow_target_name;
    float follow_lerp = 10.0f;         ///< Exponential smoothing rate, per second.
    glm::vec2 follow_deadzone{0.0f};   ///< Half-extents; no camera movement while the target stays inside.
    bool clamp_to_bounds = false;
    coopa::ui::Rect world_bounds{};

    void start() override {
        transform_ = owner ? owner->get_component<Transform2D>() : nullptr;
        if (!follow_target_name.empty() && scene) {
            if (coopa::scene::SceneObject* target_obj = scene->find_object(follow_target_name)) {
                target_transform_ = target_obj->get_component<Transform2D>();
            }
        }
    }

    void update(float dt) override {
        if (!transform_) return;
        glm::vec2 pos = transform_->position;

        if (target_transform_) {
            glm::vec2 target = target_transform_->world_position + offset;
            glm::vec2 delta = target - pos;
            glm::vec2 move{0.0f};
            if (std::fabs(delta.x) > follow_deadzone.x) {
                move.x = delta.x - (delta.x > 0.0f ? follow_deadzone.x : -follow_deadzone.x);
            }
            if (std::fabs(delta.y) > follow_deadzone.y) {
                move.y = delta.y - (delta.y > 0.0f ? follow_deadzone.y : -follow_deadzone.y);
            }
            float t = 1.0f - std::exp(-follow_lerp * dt);
            pos += move * t;
        }

        if (clamp_to_bounds) {
            pos.x = std::clamp(pos.x, world_bounds.min.x, world_bounds.max.x);
            pos.y = std::clamp(pos.y, world_bounds.min.y, world_bounds.max.y);
        }

        transform_->position = pos;
    }

    /// @brief The camera's world position. Feeds the shader's camera_pos
    ///        push constant and visible_world_rect() directly -- pixengine
    ///        renders at continuous positions, no pixel-grid snapping.
    glm::vec2 world_position() const { return transform_ ? transform_->world_position : glm::vec2(0.0f); }

    /// @brief The world-space rect currently visible in a vw x vh (e.g. the
    ///        live swapchain extent) render target.
    coopa::ui::Rect visible_world_rect(float pixels_per_unit, uint32_t vw, uint32_t vh) const {
        glm::vec2 cam = world_position();
        float hx = zoom > 0.0f ? static_cast<float>(vw) / (2.0f * pixels_per_unit * zoom) : 0.0f;
        float hy = zoom > 0.0f ? static_cast<float>(vh) / (2.0f * pixels_per_unit * zoom) : 0.0f;
        return coopa::ui::Rect{{cam.x - hx, cam.y - hy}, {cam.x + hx, cam.y + hy}};
    }

    std::string type_name() const override { return "Camera2D"; }

private:
    Transform2D* transform_ = nullptr;
    Transform2D* target_transform_ = nullptr;
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_SCENE_CAMERA2D_H
