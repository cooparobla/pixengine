/**
 * @file residency.h
 * @brief Usage-driven GPU residency for sprites: stream a sprite's atlas in
 *        when it enters range, stream it out when it leaves.
 *
 * Two layers, deliberately separate:
 *  - decide_residency() is a PURE function -- no Vulkan, no AssetManager, no
 *    Scene -- so the acquire budget, distance-priority, and release
 *    hysteresis logic is fully headless-testable.
 *  - SpriteResidencySystem is the thin per-frame applier: it builds
 *    ResidencyItems from a SceneView's sprites, calls decide_residency(),
 *    and turns the resulting actions into AssetManager::load_async() /
 *    handle-drop calls.
 *
 * This leans entirely on AssetManager's EXISTING eviction machinery
 * (set_idle_eviction/set_max_idle_frames) rather than reinventing one: once
 * this system drops a SpriteRenderer's handle, the slot's ref_count can
 * reach zero and AssetManager retires the GPU payload itself, on its own
 * schedule, with its own grace period protecting in-flight GPU work. Two
 * layers of hysteresis exist on purpose: ours (release_delay_frames) stops
 * ACQUIRE/RELEASE churn at the visibility boundary; AssetManager's
 * (max_idle_frames) stops GPU thrash for a sprite that leaves and returns
 * within a few seconds after that.
 */

#ifndef PIXENGINE_SCENE_RESIDENCY_H
#define PIXENGINE_SCENE_RESIDENCY_H

#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include <coopa/asset/asset_handle.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/asset/asset_state.h>

#include <uicoopa/layout/rect.h>

#include <pixengine/data/sprite_asset.h>
#include <pixengine/scene/scene_view.h>
#include <pixengine/scene/sprite_renderer.h>
#include <pixengine/scene/transform2d.h>

namespace coopa {
namespace pix {

/// @brief One sprite's residency-relevant state for this frame's decision pass.
struct ResidencyItem {
    bool     wants_visible = false;        ///< Estimated AABB (cull_size) intersects the keep rect this frame.
    bool     has_handle = false;           ///< The renderer already references a slot (loading, loaded, or failed).
    bool     is_loading = false;           ///< That slot is specifically mid-load (never re-acquire while true).
    float    distance_to_camera = 0.0f;    ///< Acquire priority: closer wins the budget first.
    uint16_t out_of_range_frames = 0;      ///< Consecutive frames wants_visible has been false.
};

/// @brief What to do with one item's handle this frame.
enum class ResidencyAction { None, Acquire, Release };

/**
 * @brief Pure decision function: no Vulkan, no AssetManager, no Scene.
 *
 * Release is unbudgeted (every item past its hysteresis threshold releases
 * the same frame — dropping a handle is cheap; AssetManager's own grace
 * period protects the actual GPU teardown). Acquire is budgeted and
 * prioritized by ascending distance, so a sudden flood of newly-visible
 * sprites (e.g. a camera cut) streams in over several frames instead of
 * spiking the main thread with `max_acquires_this_frame` GPU stalls at once.
 *
 * @param items                   One entry per candidate sprite.
 * @param max_acquires_this_frame Upper bound on Acquire actions returned.
 * @param release_delay_frames    Minimum consecutive out-of-range frames before Release.
 * @return One action per item, same order as `items`.
 */
inline std::vector<ResidencyAction> decide_residency(const std::vector<ResidencyItem>& items,
                                                      uint32_t max_acquires_this_frame,
                                                      uint16_t release_delay_frames) {
    std::vector<ResidencyAction> actions(items.size(), ResidencyAction::None);

    for (size_t i = 0; i < items.size(); ++i) {
        const ResidencyItem& it = items[i];
        if (!it.wants_visible && it.has_handle && !it.is_loading &&
            it.out_of_range_frames >= release_delay_frames) {
            actions[i] = ResidencyAction::Release;
        }
    }

    std::vector<size_t> candidates;
    for (size_t i = 0; i < items.size(); ++i) {
        const ResidencyItem& it = items[i];
        if (it.wants_visible && !it.has_handle && !it.is_loading) {
            candidates.push_back(i);
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [&items](size_t a, size_t b) {
        return items[a].distance_to_camera < items[b].distance_to_camera;
    });

    uint32_t acquired = 0;
    for (size_t idx : candidates) {
        if (acquired >= max_acquires_this_frame) break;
        actions[idx] = ResidencyAction::Acquire;
        ++acquired;
    }

    return actions;
}

/**
 * @class SpriteResidencySystem
 * @brief Per-frame applier: SceneView sprites -> ResidencyItems -> decide_residency() -> handle churn.
 */
class SpriteResidencySystem {
public:
    /**
     * @param assets                 Shared AssetManager; set_idle_eviction(true) should
     *                               already be configured by the caller (see file doc).
     * @param scene_dir              Base dir sprite paths resolve against (the scene file's directory).
     * @param max_acquires_per_frame Acquire budget -- throttles upload_image_2d's
     *                               vkQueueWaitIdle from stacking multiple stalls in one frame.
     * @param release_delay_frames   Consecutive out-of-range frames required before releasing.
     * @param residency_margin       Extra world-space padding added to the camera's visible
     *                               rect before culling -- a prefetch margin so a sprite is
     *                               essentially never simultaneously visible and unloaded.
     */
    SpriteResidencySystem(coopa::asset::AssetManager& assets, std::string scene_dir,
                          uint32_t max_acquires_per_frame = 4, uint16_t release_delay_frames = 30,
                          float residency_margin = 4.0f)
        : assets_(assets), scene_dir_(std::move(scene_dir)),
          max_acquires_per_frame_(max_acquires_per_frame),
          release_delay_frames_(release_delay_frames),
          residency_margin_(residency_margin) {}

    /**
     * @brief Runs one frame's residency decision + application pass.
     * @param sprites             This frame's cached SpriteRef list (see SceneView).
     * @param camera_visible_rect The active camera's visible_world_rect().
     */
    void tick(const std::vector<SpriteRef>& sprites, const coopa::ui::Rect& camera_visible_rect) {
        coopa::ui::Rect keep_rect{
            {camera_visible_rect.min.x - residency_margin_, camera_visible_rect.min.y - residency_margin_},
            {camera_visible_rect.max.x + residency_margin_, camera_visible_rect.max.y + residency_margin_},
        };

        std::vector<ResidencyItem> items;
        std::vector<SpriteRenderer*> renderers;
        items.reserve(sprites.size());
        renderers.reserve(sprites.size());

        for (const SpriteRef& ref : sprites) {
            if (!ref.renderer || !ref.transform || !ref.obj) continue;
            SpriteRenderer* sr = ref.renderer;

            glm::vec2 half = sr->cull_size * 0.5f;
            coopa::ui::Rect aabb{ref.transform->world_position - half, ref.transform->world_position + half};
            bool wants_visible = ref.obj->active() && aabb_overlaps_(aabb, keep_rect);

            uint16_t& oor = out_of_range_frames_[sr];
            if (wants_visible) {
                oor = 0;
            } else if (oor < 0xFFFFu) {
                ++oor;
            }

            ResidencyItem item;
            item.wants_visible = wants_visible;
            item.has_handle = sr->handle.is_valid();
            item.is_loading = sr->handle.is_valid() && sr->handle.state() == coopa::asset::AssetState::Loading;
            item.distance_to_camera = glm::length(ref.transform->world_position - keep_rect.center());
            item.out_of_range_frames = oor;

            items.push_back(item);
            renderers.push_back(sr);
        }

        std::vector<ResidencyAction> actions = decide_residency(items, max_acquires_per_frame_, release_delay_frames_);

        for (size_t i = 0; i < renderers.size(); ++i) {
            SpriteRenderer* sr = renderers[i];
            if (actions[i] == ResidencyAction::Acquire) {
                sr->handle = assets_.load_async<SpriteAsset>(sr->sprite, scene_dir_);
            } else if (actions[i] == ResidencyAction::Release) {
                sr->handle = coopa::asset::AssetHandle<SpriteAsset>{};
                out_of_range_frames_.erase(sr);
            }
        }
    }

private:
    static bool aabb_overlaps_(const coopa::ui::Rect& a, const coopa::ui::Rect& b) {
        return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y;
    }

    coopa::asset::AssetManager& assets_;
    std::string scene_dir_;
    uint32_t max_acquires_per_frame_;
    uint16_t release_delay_frames_;
    float residency_margin_;
    std::unordered_map<SpriteRenderer*, uint16_t> out_of_range_frames_;
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_SCENE_RESIDENCY_H
