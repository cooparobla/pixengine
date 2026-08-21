/**
 * @file pix_decoder.h
 * @brief Decodes an fkYAML tree (the plain-YAML payload inside a .pix / CAML
 *        container, or a bare .pix.yaml test fixture) into a PixDocument.
 *
 * decode_pix_document(yaml_text) is the testable core: it never touches CAML,
 * so test.cpp can embed raw YAML string literals as fixtures instead of
 * needing an encrypted+compressed binary .pix file (there are none checked
 * into this workspace, and the format can't be hand-authored or diffed).
 * load_pix_document(path) is the one-line wrapper that sniffs the "CAML"
 * magic and routes through caml::CAMLMap for real .pix files.
 *
 * Tolerates all three shapes coopixel's PixelDocument.from_dict() accepts:
 * top-level `animations:`, top-level `frames:` (wrapped in one animation),
 * or top-level `layers:` (wrapped in one frame in one animation) — see
 * coopixel/src/coopixel/models/document.py.
 */

#ifndef PIXENGINE_PIX_PIX_DECODER_H
#define PIXENGINE_PIX_PIX_DECODER_H

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <fkYAML/node.hpp>
#include <caml/caml.h>

#include <pixengine/pix/color.h>
#include <pixengine/pix/pix_document.h>

namespace coopa {
namespace pix {

namespace detail {

template <typename T>
T get_or(const fkyaml::node& node, const std::string& key, const T& fallback) {
    if (!node.is_mapping() || !node.contains(key)) return fallback;
    return node.at(key).template get_value<T>();
}

/// @brief Parses coopixel's sparse pixel-map key "x,y" (signed, may have surrounding
///        whitespace none of coopixel's writers emit, but tolerated defensively).
inline glm::ivec2 parse_pixel_key(const std::string& key) {
    size_t comma = key.find(',');
    if (comma == std::string::npos) {
        throw std::invalid_argument("parse_pixel_key: missing ',' in '" + key + "'");
    }
    int x = 0, y = 0;
    const char* xb = key.data();
    const char* xe = key.data() + comma;
    auto xr = std::from_chars(xb, xe, x);
    const char* yb = key.data() + comma + 1;
    const char* ye = key.data() + key.size();
    auto yr = std::from_chars(yb, ye, y);
    if (xr.ec != std::errc() || xr.ptr != xe || yr.ec != std::errc() || yr.ptr != ye) {
        throw std::invalid_argument("parse_pixel_key: malformed coordinate '" + key + "'");
    }
    return glm::ivec2(x, y);
}

inline PixEffect parse_effect(const fkyaml::node& node) {
    PixEffect eff;
    eff.type = get_or<std::string>(node, "type", eff.type);
    eff.enabled = get_or<bool>(node, "enabled", eff.enabled);
    eff.size = get_or<int>(node, "size", eff.size);
    eff.position = get_or<std::string>(node, "position", eff.position);
    std::string color_str = get_or<std::string>(node, "color", std::string());
    if (!color_str.empty()) {
        eff.color = parse_hex_color(color_str);
    }
    return eff;
}

inline PixLayer parse_layer(const fkyaml::node& node) {
    PixLayer layer;
    layer.name = get_or<std::string>(node, "name", layer.name);
    layer.visible = get_or<bool>(node, "visible", layer.visible);
    layer.locked = get_or<bool>(node, "locked", layer.locked);
    layer.opacity = get_or<float>(node, "opacity", layer.opacity);

    if (node.is_mapping() && node.contains("pixels")) {
        const fkyaml::node& pixels_node = node.at("pixels");
        if (pixels_node.is_mapping()) {
            layer.pixels.reserve(pixels_node.size());
            for (auto it : pixels_node.map_items()) {
                std::string key = it.key().template get_value<std::string>();
                std::string val = it.value().template get_value<std::string>();
                layer.pixels.emplace_back(parse_pixel_key(key), parse_hex_color(val));
            }
        }
    }
    std::sort(layer.pixels.begin(), layer.pixels.end(), [](const auto& a, const auto& b) {
        if (a.first.y != b.first.y) return a.first.y < b.first.y;
        return a.first.x < b.first.x;
    });

    if (node.is_mapping() && node.contains("effects")) {
        for (const auto& eff_node : node.at("effects")) {
            layer.effects.push_back(parse_effect(eff_node));
        }
    }

    return layer;
}

inline PixFrame parse_frame(const fkyaml::node& node) {
    PixFrame frame;
    frame.name = get_or<std::string>(node, "name", frame.name);
    frame.duration_ms = get_or<int>(node, "duration_ms", frame.duration_ms);

    if (node.is_mapping() && node.contains("layers")) {
        for (const auto& layer_node : node.at("layers")) {
            frame.layers.push_back(parse_layer(layer_node));
        }
    }
    if (frame.layers.empty()) {
        frame.layers.push_back(PixLayer{}); // coopixel: AnimationFrame always has >= 1 layer ("Background")
    }
    return frame;
}

inline PixAnimation parse_animation(const fkyaml::node& node) {
    PixAnimation anim;
    anim.name = get_or<std::string>(node, "name", anim.name);
    anim.fps = get_or<int>(node, "fps", anim.fps);

    if (node.is_mapping() && node.contains("frames")) {
        for (const auto& frame_node : node.at("frames")) {
            anim.frames.push_back(parse_frame(frame_node));
        }
    }
    if (anim.frames.empty()) {
        anim.frames.push_back(PixFrame{});
    }
    return anim;
}

/**
 * @brief Enforces coopixel's invariant that every animation has >= 1 frame
 *        and every frame has >= 1 layer, regardless of which of the three
 *        top-level shapes was parsed (or none of them, for a bare
 *        width/height-only document).
 */
inline void ensure_defaults(PixDocument& doc) {
    if (doc.animations.empty()) {
        doc.animations.push_back(PixAnimation{});
    }
    for (auto& anim : doc.animations) {
        if (anim.frames.empty()) {
            anim.frames.push_back(PixFrame{});
        }
        for (auto& frame : anim.frames) {
            if (frame.layers.empty()) {
                frame.layers.push_back(PixLayer{});
            }
        }
    }
}

} // namespace detail

/**
 * @brief Decodes an already-parsed fkYAML tree into a PixDocument.
 *
 * The node-based core: load_pix_document() feeds this the tree
 * caml::CAMLMap::load_caml() already parsed, WITHOUT round-tripping it
 * through CAMLMap::to_yaml_string() first. That round-trip is unsafe here —
 * fkYAML's own dumper does not quote scalars starting with '#', and every
 * pixel color is "#RRGGBBAA": `key: #FF0000FF` re-parses as `key:` followed
 * by a comment, silently turning every color into a null node. Operating on
 * the node directly sidesteps the bug entirely (and is faster, skipping a
 * full dump+reparse of a potentially 16k-entry pixel map).
 *
 * @throws std::exception (fkyaml type errors, or std::invalid_argument from
 *         malformed pixel keys / hex colors) on malformed input — never UB.
 */
inline PixDocument decode_pix_document(const fkyaml::node& root) {
    PixDocument doc;
    doc.width = detail::get_or<int>(root, "width", doc.width);
    doc.height = detail::get_or<int>(root, "height", doc.height);

    if (root.is_mapping() && root.contains("animations") && root.at("animations").size() > 0) {
        for (const auto& anim_node : root.at("animations")) {
            doc.animations.push_back(detail::parse_animation(anim_node));
        }
    } else if (root.is_mapping() && root.contains("frames") && root.at("frames").size() > 0) {
        // Legacy: a single implicit animation wrapping a top-level frame list.
        PixAnimation anim;
        anim.name = "new-animation";
        anim.fps = detail::get_or<int>(root, "fps", anim.fps);
        for (const auto& frame_node : root.at("frames")) {
            anim.frames.push_back(detail::parse_frame(frame_node));
        }
        doc.animations.push_back(std::move(anim));
    } else if (root.is_mapping() && root.contains("layers") && root.at("layers").size() > 0) {
        // Legacy: a single implicit animation + frame wrapping a top-level layer list.
        PixFrame frame;
        frame.name = "Frame 1";
        for (const auto& layer_node : root.at("layers")) {
            frame.layers.push_back(detail::parse_layer(layer_node));
        }
        PixAnimation anim;
        anim.name = "new-animation";
        anim.fps = detail::get_or<int>(root, "fps", anim.fps);
        anim.frames.push_back(std::move(frame));
        doc.animations.push_back(std::move(anim));
    }

    detail::ensure_defaults(doc);

    return doc;
}

/**
 * @brief Decodes a plain-YAML .pix payload string into a PixDocument.
 *
 * The testable core for text fixtures: test.cpp embeds YAML string literals
 * directly rather than needing an encrypted+compressed binary .pix file (see
 * load_pix_document() for why that round-trip must be avoided for real files).
 *
 * @throws std::exception (fkyaml parse errors, or std::invalid_argument from
 *         malformed pixel keys / hex colors) on malformed input — never UB.
 */
inline PixDocument decode_pix_document(const std::string& yaml_text) {
    return decode_pix_document(fkyaml::node::deserialize(yaml_text));
}

/**
 * @brief Loads a .pix file from disk, decrypting/decompressing it via CAML
 *        when the "CAML" magic is present, or reading it as plain YAML
 *        otherwise (a convenience for hand-authored fixtures on disk).
 *
 * A real .pix's tree is decoded directly from caml::CAMLMap::get_raw_node()
 * — never via CAMLMap::to_yaml_string() (see decode_pix_document(node)'s doc
 * for why that round-trip corrupts every pixel color).
 */
inline PixDocument load_pix_document(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("load_pix_document: cannot open '" + path + "'");
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string raw = ss.str();

    bool is_caml = raw.size() >= 4 &&
                   raw[0] == 'C' && raw[1] == 'A' && raw[2] == 'M' && raw[3] == 'L';

    if (is_caml) {
        caml::CAMLMap map = caml::CAMLMap::load_caml(path);
        return decode_pix_document(map.get_raw_node());
    }
    return decode_pix_document(raw);
}

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_PIX_PIX_DECODER_H
