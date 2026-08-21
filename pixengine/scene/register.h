/**
 * @file register.h
 * @brief Registers pixengine's scene components as coopa::scene::SceneLoader parsers.
 *
 * Same shape and same teardown contract as
 * gfxcoopa/engine/components/register.h: call
 * coopa::scene::SceneLoader::clear_component_parsers() before the Device/
 * Allocator/CommandPool/AssetManager captured here are destroyed.
 *
 * Registers "Transform2D", "SpriteRenderer", "SpriteAnimator", "Camera2D".
 * Deliberately does NOT (and cannot) register anything for the name
 * "Transform" -- SceneLoader special-cases that name and handles it itself
 * before ever consulting the parser registry (see scene_loader.h's
 * parse_component_()), so a scene that forgets `auto_transform: false` gets
 * a silent, unused 3D TransformComponent with no way for pixengine to
 * detect or warn about it from in here. Every pixengine scene file MUST set
 * `auto_transform: false` and use Transform2D instead.
 */

#ifndef PIXENGINE_SCENE_REGISTER_H
#define PIXENGINE_SCENE_REGISTER_H

#include <iostream>
#include <string>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <fkYAML/node.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>

#include <uicoopa/layout/rect.h>

#include <pixengine/data/sprite_asset.h>
#include <pixengine/data/tilemap_asset.h>
#include <pixengine/scene/camera2d.h>
#include <pixengine/scene/sprite_animator.h>
#include <pixengine/scene/sprite_renderer.h>
#include <pixengine/scene/tilemap_renderer.h>
#include <pixengine/scene/transform2d.h>

namespace coopa {
namespace pix {

namespace detail {

inline glm::vec2 parse_vec2_(const fkyaml::node& node, glm::vec2 fallback) {
    if (!node.is_mapping()) return fallback;
    float x = node.contains("x") ? node.at("x").get_value<float>() : fallback.x;
    float y = node.contains("y") ? node.at("y").get_value<float>() : fallback.y;
    return {x, y};
}

inline glm::vec4 parse_color_(const fkyaml::node& node, glm::vec4 fallback) {
    if (!node.is_mapping()) return fallback;
    float r = node.contains("r") ? node.at("r").get_value<float>() : fallback.r;
    float g = node.contains("g") ? node.at("g").get_value<float>() : fallback.g;
    float b = node.contains("b") ? node.at("b").get_value<float>() : fallback.b;
    float a = node.contains("a") ? node.at("a").get_value<float>() : fallback.a;
    return {r, g, b, a};
}

} // namespace detail

/**
 * @param device    Vulkan logical device, forwarded to PixLoader-driven asset loads.
 * @param allocator VMA allocator.
 * @param cmd_pool  Command pool for one-shot atlas upload transfers.
 * @param assets    AssetManager with PixLoader already registered for SpriteAsset
 *                  (see pixengine/loaders/pix_loader.h). Must outlive every
 *                  subsequent SceneLoader::load() call, same as device/allocator/cmd_pool.
 */
inline void register_pix_components(coopa::gfx::core::Device& device,
                                    coopa::gfx::memory::Allocator& allocator,
                                    coopa::gfx::command::CommandPool& cmd_pool,
                                    coopa::asset::AssetManager& assets) {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;
    (void)device;
    (void)allocator;
    (void)cmd_pool;

    SceneLoader::register_component_parser("Transform2D",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* t = obj.add_component<Transform2D>();
            if (node.contains("position")) t->position = detail::parse_vec2_(node.at("position"), t->position);
            if (node.contains("rotation")) t->rotation = node.at("rotation").get_value<float>();
            if (node.contains("scale")) t->scale = detail::parse_vec2_(node.at("scale"), t->scale);
            // Component parsers here only opt IN to keys they recognize (see
            // the loop of `if (node.contains(...))` guards throughout this
            // file) -- SceneLoader never enumerates or validates the full key
            // set. A scene authored against an older build (e.g. one with a
            // now-removed Camera2D `pixel_snap` key) silently drops unknown
            // keys rather than failing to parse.
        });

    SceneLoader::register_component_parser("SpriteRenderer",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* sr = obj.add_component<SpriteRenderer>();
            if (node.contains("sprite")) sr->sprite = node.at("sprite").get_value<std::string>();
            if (node.contains("color")) sr->color = detail::parse_color_(node.at("color"), sr->color);
            if (node.contains("sort_layer")) sr->sort_layer = node.at("sort_layer").get_value<int>();
            if (node.contains("order_in_layer")) sr->order_in_layer = node.at("order_in_layer").get_value<int>();
            if (node.contains("y_sort")) sr->y_sort = node.at("y_sort").get_value<bool>();
            if (node.contains("y_sort_offset")) sr->y_sort_offset = node.at("y_sort_offset").get_value<float>();
            if (node.contains("cull_size")) sr->cull_size = detail::parse_vec2_(node.at("cull_size"), sr->cull_size);
            if (node.contains("flip_x")) sr->flip_x = node.at("flip_x").get_value<bool>();
            if (node.contains("flip_y")) sr->flip_y = node.at("flip_y").get_value<bool>();

            // Deliberately no assets.load() here (Phase 5/6 did this synchronously
            // at parse time) -- Phase 7's SpriteResidencySystem acquires/releases
            // sr->handle every frame based on visibility, via load_async(). A
            // SpriteRenderer starts with an empty handle and stays that way until
            // it first enters the camera's (margin-expanded) visible rect.
            (void)sr;
        });

    SceneLoader::register_component_parser("SpriteAnimator",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* anim = obj.add_component<SpriteAnimator>();
            if (node.contains("default_animation")) {
                anim->default_animation = node.at("default_animation").get_value<std::string>();
            }
            if (node.contains("speed")) anim->speed = node.at("speed").get_value<float>();
            if (node.contains("autoplay")) anim->autoplay = node.at("autoplay").get_value<bool>();
        });

    SceneLoader::register_component_parser("Camera2D",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* cam = obj.add_component<Camera2D>();
            if (node.contains("zoom")) cam->zoom = node.at("zoom").get_value<float>();
            if (node.contains("offset")) cam->offset = detail::parse_vec2_(node.at("offset"), cam->offset);
            if (node.contains("follow_target")) {
                cam->follow_target_name = node.at("follow_target").get_value<std::string>();
            }
            if (node.contains("follow_lerp")) cam->follow_lerp = node.at("follow_lerp").get_value<float>();
            if (node.contains("follow_deadzone")) {
                cam->follow_deadzone = detail::parse_vec2_(node.at("follow_deadzone"), cam->follow_deadzone);
            }
            if (node.contains("clamp_to_bounds")) cam->clamp_to_bounds = node.at("clamp_to_bounds").get_value<bool>();
            if (node.contains("world_bounds")) {
                const auto& wb = node.at("world_bounds");
                glm::vec2 min = wb.contains("min") ? detail::parse_vec2_(wb.at("min"), glm::vec2(0.0f)) : glm::vec2(0.0f);
                glm::vec2 max = wb.contains("max") ? detail::parse_vec2_(wb.at("max"), glm::vec2(0.0f)) : glm::vec2(0.0f);
                cam->world_bounds = coopa::ui::Rect{min, max};
            }
        });

    SceneLoader::register_component_parser("TilemapRenderer",
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* tr = obj.add_component<TilemapRenderer>();
            if (node.contains("tilemap")) tr->tilemap = node.at("tilemap").get_value<std::string>();

            if (!tr->tilemap.empty()) {
                // Phase 6: a synchronous load at parse time, same as SpriteRenderer.
                tr->handle = assets.load<TilemapAsset>(tr->tilemap, ctx.scene_dir);
                if (tr->handle.is_failed()) {
                    std::cerr << "[register_pix_components] Failed to load tilemap '" << tr->tilemap
                              << "' (scene_dir=" << ctx.scene_dir << "): " << tr->handle.error() << std::endl;
                }
            }
        });
}

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_SCENE_REGISTER_H
