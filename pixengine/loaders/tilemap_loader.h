/**
 * @file tilemap_loader.h
 * @brief coopa::asset loader for TilemapAsset: parses the tilemap YAML
 *        (off-thread) and composites its tileset .pix into one whole-canvas
 *        texture (also off-thread); finalize_typed() only touches the GPU.
 */

#ifndef PIXENGINE_LOADERS_TILEMAP_LOADER_H
#define PIXENGINE_LOADERS_TILEMAP_LOADER_H

#include <volk/volk.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

#include <coopa/asset/asset_loader.h>
#include <coopa/asset/asset_source.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/engine/data/texture.h>

#include <fkYAML/node.hpp>

#include <pixengine/data/tilemap_asset.h>
#include <pixengine/data/tilemap_decoder.h>
#include <pixengine/pix/pix_decoder.h>
#include <pixengine/pix/pix_composite.h>

namespace coopa {
namespace pix {

/**
 * @class TilemapLoader
 * @brief Registers as the coopa::asset loader for TilemapAsset.
 *
 * @code
 * assets.register_loader<coopa::pix::TilemapAsset>(
 *     std::make_unique<coopa::pix::TilemapLoader>(device, allocator, cmd_pool));
 * auto handle = assets.load<coopa::pix::TilemapAsset>("tilemaps/farm.tilemap.yaml");
 * @endcode
 */
class TilemapLoader : public coopa::asset::TypedAssetLoader<TilemapAsset, DecodedTilemap> {
public:
    TilemapLoader(coopa::gfx::core::Device& device,
                 coopa::gfx::memory::Allocator& allocator,
                 coopa::gfx::command::CommandPool& cmd_pool)
        : device_(device), allocator_(allocator), cmd_pool_(cmd_pool) {}

    std::shared_ptr<DecodedTilemap> decode_typed(const coopa::asset::AssetId& id,
                                                 const coopa::asset::LoadContext& ctx) override {
        (void)id;
        // Tilemap YAML is plain (unlike .pix, never CAML-wrapped) -- the same
        // convention as scene.yaml files.
        std::ifstream file(ctx.resolved_path);
        if (!file) {
            throw std::runtime_error("TilemapLoader: cannot open '" + ctx.resolved_path + "'");
        }
        fkyaml::node root = fkyaml::node::deserialize(file);

        auto doc = std::make_shared<DecodedTilemap>(decode_tilemap_document(root));
        if (doc->tileset_source.empty()) {
            throw std::runtime_error("TilemapLoader: '" + ctx.resolved_path + "' has no tileset.source");
        }

        // Resolve the tileset path relative to the TILEMAP file's own directory
        // (not the scene's), then load+composite it as a whole uniform grid --
        // no trim, no shelf-packing (see tilemap_asset.h's file doc for why).
        std::string tilemap_dir = std::filesystem::path(ctx.resolved_path).parent_path().string();
        std::string tileset_path = ctx.source ? ctx.source->resolve(doc->tileset_source, tilemap_dir)
                                              : (tilemap_dir + "/" + doc->tileset_source);

        PixDocument tileset_doc = load_pix_document(tileset_path);
        if (tileset_doc.animations.empty() || tileset_doc.animations[0].frames.empty()) {
            throw std::runtime_error("TilemapLoader: tileset '" + tileset_path + "' has no frames");
        }
        std::vector<uint32_t> canvas = composite_frame(
            tileset_doc.width, tileset_doc.height, tileset_doc.animations[0].frames[0], /*apply_effects=*/true);

        doc->tileset_pixels = std::move(canvas);
        doc->tileset_width = tileset_doc.width;
        doc->tileset_height = tileset_doc.height;

        return doc;
    }

    std::shared_ptr<TilemapAsset> finalize_typed(std::shared_ptr<DecodedTilemap> decoded,
                                                 const coopa::asset::AssetId&,
                                                 const coopa::asset::LoadContext&) override {
        auto tileset = coopa::gfx::engine::data::Texture::upload(
            device_, allocator_, cmd_pool_,
            reinterpret_cast<const uint8_t*>(decoded->tileset_pixels.data()),
            static_cast<uint32_t>(decoded->tileset_width), static_cast<uint32_t>(decoded->tileset_height),
            /*srgb=*/false, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        return std::make_shared<TilemapAsset>(std::move(tileset), std::move(*decoded));
    }

    const char* type_name() const override { return "TilemapAsset"; }

private:
    coopa::gfx::core::Device&         device_;
    coopa::gfx::memory::Allocator&    allocator_;
    coopa::gfx::command::CommandPool& cmd_pool_;
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_LOADERS_TILEMAP_LOADER_H
