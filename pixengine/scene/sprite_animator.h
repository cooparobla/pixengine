/**
 * @file sprite_animator.h
 * @brief Advances a sibling SpriteRenderer's animation over time.
 *
 * All mutable playback state (current animation, local frame index, elapsed
 * time, ping-pong direction) lives HERE, never on SpriteAsset (which is
 * immutable after publish — see sprite_asset.h) or on SpriteRenderer. Ten
 * SpriteRenderers sharing one SpriteAsset animate independently because each
 * has its own SpriteAnimator.
 *
 * Reads animation/frame-duration data off the sibling SpriteRenderer's
 * loaded asset -- this is the one direction of the two components'
 * dependency (see sprite_renderer.h's header comment for why it isn't
 * mutual). A SpriteAnimator with no sibling SpriteRenderer, or one whose
 * asset isn't loaded yet, simply does nothing.
 *
 * Per-frame named events (footstep/hit/etc., carried on
 * SpriteAnimation::events from the sidecar) are exposed as data but not yet
 * dispatched through the scene's EventBus -- out of scope for Phase 5;
 * current_frame_index() is enough for a caller to poll if needed.
 */

#ifndef PIXENGINE_SCENE_SPRITE_ANIMATOR_H
#define PIXENGINE_SCENE_SPRITE_ANIMATOR_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>

#include <pixengine/scene/sprite_renderer.h>

namespace coopa {
namespace pix {

/**
 * @class SpriteAnimator
 * @brief Owns per-instance animation playback state for a sibling SpriteRenderer.
 */
class SpriteAnimator : public coopa::scene::Component {
public:
    std::string default_animation;
    float       speed = 1.0f;
    bool        autoplay = true;

    std::string type_name() const override { return "SpriteAnimator"; }

    void start() override {
        if (autoplay && !default_animation.empty()) {
            play(default_animation);
        }
    }

    /// @brief Switches to a named animation, restarting playback from its first frame.
    void play(const std::string& name) {
        if (name == current_animation_) return;
        current_animation_ = name;
        local_frame_ = 0;
        elapsed_ = 0.0f;
        finished_ = false;
    }

    const std::string& current_animation() const { return current_animation_; }

    void update(float dt) override {
        if (current_animation_.empty() || finished_) return;
        const SpriteRenderer* renderer = owner ? owner->get_component<SpriteRenderer>() : nullptr;
        if (!renderer || !renderer->handle.is_loaded()) return;
        const SpriteAsset* asset = renderer->handle.get();
        const SpriteAnimation* anim = asset->find_animation(current_animation_);
        if (!anim || anim->frame_count == 0) return;

        if (anim->frame_count == 1) {
            local_frame_ = 0;
            return;
        }

        float total_duration = 0.0f;
        for (uint32_t i = 0; i < anim->frame_count; ++i) {
            total_duration += std::max(asset->frame(anim->first_frame + i).duration_s, 0.001f);
        }
        if (total_duration <= 0.0f) return;

        elapsed_ += dt * speed;

        switch (anim->loop) {
        case LoopMode::Loop: {
            float t = std::fmod(elapsed_, total_duration);
            if (t < 0.0f) t += total_duration;
            local_frame_ = frame_at_time_(*asset, *anim, t);
            break;
        }
        case LoopMode::PingPong: {
            float cycle = total_duration * 2.0f;
            float t = std::fmod(elapsed_, cycle);
            if (t < 0.0f) t += cycle;
            if (t < total_duration) {
                local_frame_ = frame_at_time_(*asset, *anim, t);
            } else {
                uint32_t fwd = frame_at_time_(*asset, *anim, t - total_duration);
                local_frame_ = anim->frame_count - 1 - fwd;
            }
            break;
        }
        case LoopMode::Once:
        case LoopMode::Hold:
        default: {
            if (elapsed_ >= total_duration) {
                local_frame_ = anim->frame_count - 1;
                finished_ = true;
            } else {
                local_frame_ = frame_at_time_(*asset, *anim, elapsed_);
            }
            break;
        }
        }
    }

    /// @brief The global index into SpriteAsset::frames() for the current playback position.
    uint32_t current_frame_index() const {
        const SpriteRenderer* renderer = owner ? owner->get_component<SpriteRenderer>() : nullptr;
        if (!renderer || !renderer->handle.is_loaded()) return 0;
        const SpriteAnimation* anim = renderer->handle.get()->find_animation(current_animation_);
        if (!anim) return 0;
        return anim->first_frame + local_frame_;
    }

private:
    static uint32_t frame_at_time_(const SpriteAsset& asset, const SpriteAnimation& anim, float t) {
        float acc = 0.0f;
        for (uint32_t i = 0; i < anim.frame_count; ++i) {
            float dur = std::max(asset.frame(anim.first_frame + i).duration_s, 0.001f);
            if (t < acc + dur) return i;
            acc += dur;
        }
        return anim.frame_count > 0 ? anim.frame_count - 1 : 0;
    }

    std::string current_animation_;
    uint32_t    local_frame_ = 0;
    float       elapsed_ = 0.0f;
    bool        finished_ = false;
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_SCENE_SPRITE_ANIMATOR_H
