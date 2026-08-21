/**
 * @file tilemap_decoder.h
 * @brief Pure YAML -> DecodedTilemap decoding (layers, gids, tileset
 *        metadata) -- NOT the tileset image itself, which needs .pix
 *        compositing and is filled in by TilemapLoader alongside this.
 *
 * No Vulkan; headlessly testable, same split as pix_decoder.h.
 */

#ifndef PIXENGINE_DATA_TILEMAP_DECODER_H
#define PIXENGINE_DATA_TILEMAP_DECODER_H

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <fkYAML/node.hpp>

#include <pixengine/data/tilemap_asset.h>

namespace coopa {
namespace pix {

/**
 * @brief Expands coopixel-style RLE tokens ("count:gid") into a flat gid vector.
 * @throws std::invalid_argument on a malformed token (missing ':', non-numeric
 *         count/gid, or a negative count/gid) -- never UB, never a silent skip.
 */
inline std::vector<uint16_t> expand_tile_rle(const std::vector<std::string>& tokens) {
    std::vector<uint16_t> out;
    for (const std::string& tok : tokens) {
        size_t colon = tok.find(':');
        if (colon == std::string::npos) {
            throw std::invalid_argument("expand_tile_rle: malformed run '" + tok + "' (expected 'count:gid')");
        }
        int count = 0, gid = 0;
        const char* count_end = tok.data() + colon;
        const char* gid_end = tok.data() + tok.size();
        auto cr = std::from_chars(tok.data(), count_end, count);
        auto gr = std::from_chars(tok.data() + colon + 1, gid_end, gid);
        if (cr.ec != std::errc() || cr.ptr != count_end ||
            gr.ec != std::errc() || gr.ptr != gid_end ||
            count < 0 || gid < 0) {
            throw std::invalid_argument("expand_tile_rle: malformed run '" + tok + "'");
        }
        out.insert(out.end(), static_cast<size_t>(count), static_cast<uint16_t>(gid));
    }
    return out;
}

namespace detail {

inline TileLayer parse_tile_layer(const fkyaml::node& node) {
    TileLayer layer;
    layer.name = node.contains("name") ? node.at("name").get_value<std::string>() : "Layer";
    layer.sort_layer = node.contains("sort_layer") ? node.at("sort_layer").get_value<int>() : 0;
    layer.y_sort = node.contains("y_sort") ? node.at("y_sort").get_value<bool>() : false;
    layer.y_sort_offset = node.contains("y_sort_offset") ? node.at("y_sort_offset").get_value<float>() : 0.0f;
    layer.width = node.contains("width") ? node.at("width").get_value<uint32_t>() : 0;
    layer.height = node.contains("height") ? node.at("height").get_value<uint32_t>() : 0;

    if (!node.contains("data")) return layer;

    std::string encoding = node.contains("encoding") ? node.at("encoding").get_value<std::string>() : "flat";
    if (encoding == "rle") {
        std::vector<std::string> tokens;
        for (const auto& item : node.at("data")) tokens.push_back(item.get_value<std::string>());
        layer.gids = expand_tile_rle(tokens);
    } else {
        for (const auto& item : node.at("data")) {
            layer.gids.push_back(static_cast<uint16_t>(item.get_value<int>()));
        }
    }

    uint32_t expected = layer.width * layer.height;
    if (layer.gids.size() != expected) {
        throw std::invalid_argument("parse_tile_layer: layer '" + layer.name + "' gid count (" +
                                    std::to_string(layer.gids.size()) + ") != width*height (" +
                                    std::to_string(expected) + ")");
    }
    return layer;
}

} // namespace detail

/**
 * @brief Decodes a tilemap YAML tree into a DecodedTilemap. Leaves
 *        tileset_pixels/tileset_width/tileset_height at their defaults --
 *        TilemapLoader fills those in from tileset_source separately, since
 *        that requires the .pix compositing pipeline.
 * @throws std::exception on malformed input (fkyaml type errors, or
 *         std::invalid_argument from expand_tile_rle/parse_tile_layer).
 */
inline DecodedTilemap decode_tilemap_document(const fkyaml::node& root) {
    DecodedTilemap doc;

    if (root.contains("tile_size")) {
        const auto& ts = root.at("tile_size");
        if (ts.contains("x")) doc.tile_size_px.x = ts.at("x").get_value<float>();
        if (ts.contains("y")) doc.tile_size_px.y = ts.at("y").get_value<float>();
    }
    if (root.contains("pixels_per_unit")) {
        doc.pixels_per_unit = root.at("pixels_per_unit").get_value<float>();
    }

    if (root.contains("tileset")) {
        const auto& ts = root.at("tileset");
        if (ts.contains("source")) doc.tileset_source = ts.at("source").get_value<std::string>();
        if (ts.contains("columns")) doc.tileset_columns = ts.at("columns").get_value<uint32_t>();
        if (ts.contains("first_gid")) doc.tileset_first_gid = ts.at("first_gid").get_value<uint32_t>();
    }

    if (root.contains("layers")) {
        for (const auto& layer_node : root.at("layers")) {
            doc.layers.push_back(detail::parse_tile_layer(layer_node));
        }
    }

    if (root.contains("collision")) {
        for (const auto& c_node : root.at("collision")) {
            TilemapCollisionRule rule;
            if (c_node.contains("layer")) rule.layer = c_node.at("layer").get_value<std::string>();
            if (c_node.contains("solid_gids")) {
                for (const auto& g : c_node.at("solid_gids")) {
                    rule.solid_gids.push_back(static_cast<uint16_t>(g.get_value<int>()));
                }
            }
            doc.collision.push_back(std::move(rule));
        }
    }

    return doc;
}

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_DATA_TILEMAP_DECODER_H
