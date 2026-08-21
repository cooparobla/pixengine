/**
 * @file sprite_meta.h
 * @brief Parses the YAML sidecar (`<stem>.sprite.yaml`) that carries
 *        pivot/PPU/loop-mode/events/hitboxes/anchors alongside a .pix file.
 *
 * .pix itself (coopixel's format) has no pivot points, per-frame tags, loop
 * mode, or hitbox/anchor metadata — rather than extend coopixel, this data
 * lives in a hand-authored sidecar next to the .pix, merged in at load time
 * by pix_loader.h (Phase 4). Every key here is optional; an absent sidecar
 * parses to sprite_meta_defaults(). Pure CPU, no Vulkan.
 */

#ifndef PIXENGINE_PIX_SPRITE_META_H
#define PIXENGINE_PIX_SPRITE_META_H

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include <fkYAML/node.hpp>

namespace coopa {
namespace pix {

/// @brief Playback behavior once an animation reaches its last frame.
enum class LoopMode { Loop, Once, PingPong, Hold };

/// @brief Parses a loop-mode string; unrecognized values default to Loop.
inline LoopMode parse_loop_mode(const std::string& s) {
    if (s == "Once") return LoopMode::Once;
    if (s == "PingPong") return LoopMode::PingPong;
    if (s == "Hold") return LoopMode::Hold;
    return LoopMode::Loop; // "Loop", empty, or unrecognized
}

/// @brief A named playback event fired when an animation reaches a given local frame index.
struct AnimEvent {
    int frame = 0;
    std::string name;
};

/// @brief A per-frame override of a value the .pix itself carries (currently just duration).
struct FrameOverride {
    int index = 0;
    int duration_ms = -1; ///< -1 = not overridden; keep the .pix frame's own duration.
};

/// @brief Sidecar metadata for one named animation.
struct AnimMeta {
    LoopMode loop = LoopMode::Loop;
    float speed = 1.0f;
    bool has_pivot = false;      ///< true if this animation overrides the sprite-level pivot.
    glm::vec2 pivot{0.5f, 0.0f};
    std::vector<AnimEvent> events;
    std::vector<FrameOverride> frame_overrides;
};

/// @brief A named, inert (engine-exposed-only) rectangle in world units relative to the pivot.
struct Hitbox {
    std::string name;
    glm::vec2 min{0.0f}; ///< rect.{x,y}
    glm::vec2 size{0.0f}; ///< rect.{w,h}
};

/// @brief Fully parsed sidecar contents.
struct SpriteMeta {
    float pixels_per_unit = 16.0f;
    glm::vec2 pivot{0.5f, 0.0f};
    bool apply_effects = true;
    bool premultiply = true;
    std::unordered_map<std::string, AnimMeta> animations; ///< keyed by animation name
    std::vector<Hitbox> hitboxes;
    std::unordered_map<std::string, glm::vec2> anchors;
};

/// @brief The sidecar's defaults, used verbatim when no sidecar file exists.
inline SpriteMeta sprite_meta_defaults() { return SpriteMeta{}; }

namespace detail {

inline glm::vec2 parse_vec2(const fkyaml::node& node, glm::vec2 fallback) {
    if (!node.is_mapping()) return fallback;
    float x = node.contains("x") ? node.at("x").get_value<float>() : fallback.x;
    float y = node.contains("y") ? node.at("y").get_value<float>() : fallback.y;
    return {x, y};
}

inline AnimMeta parse_anim_meta(const fkyaml::node& node) {
    AnimMeta meta;
    if (!node.is_mapping()) return meta;

    if (node.contains("loop")) {
        meta.loop = parse_loop_mode(node.at("loop").get_value<std::string>());
    }
    if (node.contains("speed")) {
        meta.speed = node.at("speed").get_value<float>();
    }
    if (node.contains("pivot")) {
        meta.has_pivot = true;
        meta.pivot = parse_vec2(node.at("pivot"), meta.pivot);
    }
    if (node.contains("events")) {
        for (const auto& ev_node : node.at("events")) {
            AnimEvent ev;
            ev.frame = ev_node.contains("frame") ? ev_node.at("frame").get_value<int>() : 0;
            ev.name = ev_node.contains("name") ? ev_node.at("name").get_value<std::string>() : std::string();
            meta.events.push_back(ev);
        }
    }
    if (node.contains("frames")) {
        for (const auto& f_node : node.at("frames")) {
            FrameOverride fo;
            fo.index = f_node.contains("index") ? f_node.at("index").get_value<int>() : 0;
            if (f_node.contains("duration_ms")) {
                fo.duration_ms = f_node.at("duration_ms").get_value<int>();
            }
            meta.frame_overrides.push_back(fo);
        }
    }
    return meta;
}

} // namespace detail

/**
 * @brief Parses a sidecar YAML string into a SpriteMeta.
 *
 * The testable core, mirroring pix_decoder.h's decode_pix_document() split:
 * test.cpp exercises this directly with embedded YAML literals.
 */
inline SpriteMeta parse_sprite_meta(const std::string& yaml_text) {
    SpriteMeta meta = sprite_meta_defaults();
    fkyaml::node root = fkyaml::node::deserialize(yaml_text);
    if (!root.is_mapping()) return meta;

    if (root.contains("pixels_per_unit")) {
        meta.pixels_per_unit = root.at("pixels_per_unit").get_value<float>();
    }
    if (root.contains("pivot")) {
        meta.pivot = detail::parse_vec2(root.at("pivot"), meta.pivot);
    }
    if (root.contains("apply_effects")) {
        meta.apply_effects = root.at("apply_effects").get_value<bool>();
    }
    if (root.contains("premultiply")) {
        meta.premultiply = root.at("premultiply").get_value<bool>();
    }
    if (root.contains("animations") && root.at("animations").is_mapping()) {
        for (auto it : root.at("animations").map_items()) {
            std::string name = it.key().get_value<std::string>();
            meta.animations.emplace(name, detail::parse_anim_meta(it.value()));
        }
    }
    if (root.contains("hitboxes")) {
        for (const auto& hb_node : root.at("hitboxes")) {
            Hitbox hb;
            hb.name = hb_node.contains("name") ? hb_node.at("name").get_value<std::string>() : std::string();
            if (hb_node.contains("rect")) {
                const auto& r = hb_node.at("rect");
                float x = r.contains("x") ? r.at("x").get_value<float>() : 0.0f;
                float y = r.contains("y") ? r.at("y").get_value<float>() : 0.0f;
                float w = r.contains("w") ? r.at("w").get_value<float>() : 0.0f;
                float h = r.contains("h") ? r.at("h").get_value<float>() : 0.0f;
                hb.min = {x, y};
                hb.size = {w, h};
            }
            meta.hitboxes.push_back(hb);
        }
    }
    if (root.contains("anchors") && root.at("anchors").is_mapping()) {
        for (auto it : root.at("anchors").map_items()) {
            std::string name = it.key().get_value<std::string>();
            meta.anchors.emplace(name, detail::parse_vec2(it.value(), glm::vec2(0.0f)));
        }
    }

    return meta;
}

/// @brief Looks up an animation's sidecar metadata, or a default-constructed AnimMeta if absent.
inline AnimMeta find_anim_meta(const SpriteMeta& meta, const std::string& name) {
    auto it = meta.animations.find(name);
    return it != meta.animations.end() ? it->second : AnimMeta{};
}

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_PIX_SPRITE_META_H
