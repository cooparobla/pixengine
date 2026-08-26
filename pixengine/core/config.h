/**
 * @file config.h
 * @brief Runtime configuration loaded from assets/config.yaml, mirroring
 *        blendy's blendy::core::AppConfig pattern (see
 *        blendy/src/blendy/core/config.h) -- every field has a sane in-class
 *        default, every YAML key is individually optional
 *        (`if (node.contains(...))`), unknown keys are silently ignored, and
 *        a missing or malformed file just falls back to defaults rather than
 *        failing startup.
 */

#ifndef PIXENGINE_CORE_CONFIG_H
#define PIXENGINE_CORE_CONFIG_H

#include <caml/caml.h>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

namespace coopa {
namespace pix {

/**
 * @struct SceneConfig
 * @brief Which two scenes to load at startup -- the world (Transform2D/
 *        SpriteRenderer/Camera2D/...) and the HUD (a separate uicoopa Scene;
 *        world and UI coordinate systems don't mix in one tree).
 */
struct SceneConfig {
    std::string default_scene = "assets/scenes/farm/scene.yaml";
    std::string hud_scene     = "assets/scenes/hud/scene.yaml";
};

/**
 * @struct WindowConfig
 * @brief Window and presentation settings.
 */
struct WindowConfig {
    std::string title     = "pixengine";
    uint32_t    width     = 1280;
    uint32_t    height    = 720;
    bool        vsync     = true;
    bool        resizable = true;
};

/**
 * @struct CanvasConfig
 * @brief The world's field of view: a fixed reference resolution, uniformly
 *        best-fit scaled to whatever window it's rendered into (see
 *        pixel_math.h's compute_fit_viewport()) -- resizing the window
 *        changes how big that fixed view renders, not how much world is in
 *        it. pixels_per_unit is the sprite-space scale (source .pix pixels
 *        per world unit) that reference_width/height get divided by to
 *        determine visible world extent -- see Camera2D::visible_world_rect().
 */
struct CanvasConfig {
    uint32_t reference_width  = 480;
    uint32_t reference_height = 270;
    float    pixels_per_unit  = 16.0f; // matches assets/sprites/player.sprite.yaml
};

/**
 * @struct RendererConfig
 * @brief Sprite pass tuning.
 */
struct RendererConfig {
    uint32_t max_textures = 256; // upper bound on distinct atlases drawn in a single frame
};

/**
 * @struct AssetsConfig
 * @brief AssetManager + SpriteResidencySystem streaming tuning (Phase 7).
 */
struct AssetsConfig {
    uint32_t io_threads             = 2;
    bool     idle_eviction          = true;
    uint32_t max_idle_frames        = 60;   // ~1s at 60fps; lower than the suggested
                                             // production default (180, ~3s) so a demo
                                             // run doesn't need to stay open for minutes
    uint32_t max_acquires_per_frame = 4;    // throttles the underlying upload's blocking wait
                                             // from stacking multiple stalls in one frame
    uint32_t release_delay_frames   = 30;   // hysteresis before dropping a handle that
                                             // just left the visible+margin rect
    float    residency_margin       = 4.0f; // world units of prefetch margin around
                                             // the camera's visible rect
};

/**
 * @struct AppConfig
 * @brief Aggregated runtime configuration for pixengine.
 */
struct AppConfig {
    SceneConfig    scene;
    WindowConfig   window;
    CanvasConfig   canvas;
    RendererConfig renderer;
    AssetsConfig   assets;

    /**
     * @brief Loads application configuration from a YAML or CAML file.
     * @param path Path to the configuration file (e.g., assets/config.yaml).
     * @return Loaded AppConfig struct, or default values if loading fails.
     */
    static AppConfig load(const std::string& path) {
        AppConfig config;
        try {
            if (!std::filesystem::exists(path)) {
                std::cerr << "[pixengine::core::AppConfig] Config file not found at " << path << ", using defaults.\n";
                return config;
            }

            caml::CAMLMap map = caml::CAMLMap::load_yaml(path);
            const auto& root = map.get_raw_node();

            if (root.contains("scene")) {
                const auto& s_node = root.at("scene");
                if (s_node.contains("default_scene")) {
                    config.scene.default_scene = s_node.at("default_scene").get_value<std::string>();
                }
                if (s_node.contains("hud_scene")) {
                    config.scene.hud_scene = s_node.at("hud_scene").get_value<std::string>();
                }
            }

            if (root.contains("window")) {
                const auto& w_node = root.at("window");
                if (w_node.contains("title")) {
                    config.window.title = w_node.at("title").get_value<std::string>();
                }
                if (w_node.contains("width")) {
                    config.window.width = w_node.at("width").get_value<uint32_t>();
                }
                if (w_node.contains("height")) {
                    config.window.height = w_node.at("height").get_value<uint32_t>();
                }
                if (w_node.contains("vsync")) {
                    config.window.vsync = w_node.at("vsync").get_value<bool>();
                }
                if (w_node.contains("resizable")) {
                    config.window.resizable = w_node.at("resizable").get_value<bool>();
                }
            }

            if (root.contains("canvas")) {
                const auto& c_node = root.at("canvas");
                if (c_node.contains("reference_width")) {
                    config.canvas.reference_width = c_node.at("reference_width").get_value<uint32_t>();
                }
                if (c_node.contains("reference_height")) {
                    config.canvas.reference_height = c_node.at("reference_height").get_value<uint32_t>();
                }
                if (c_node.contains("pixels_per_unit")) {
                    config.canvas.pixels_per_unit = c_node.at("pixels_per_unit").get_value<float>();
                }
            }

            if (root.contains("renderer")) {
                const auto& r_node = root.at("renderer");
                if (r_node.contains("max_textures")) {
                    config.renderer.max_textures = r_node.at("max_textures").get_value<uint32_t>();
                }
            }

            if (root.contains("assets")) {
                const auto& a_node = root.at("assets");
                if (a_node.contains("io_threads")) {
                    config.assets.io_threads = a_node.at("io_threads").get_value<uint32_t>();
                }
                if (a_node.contains("idle_eviction")) {
                    config.assets.idle_eviction = a_node.at("idle_eviction").get_value<bool>();
                }
                if (a_node.contains("max_idle_frames")) {
                    config.assets.max_idle_frames = a_node.at("max_idle_frames").get_value<uint32_t>();
                }
                if (a_node.contains("max_acquires_per_frame")) {
                    config.assets.max_acquires_per_frame = a_node.at("max_acquires_per_frame").get_value<uint32_t>();
                }
                if (a_node.contains("release_delay_frames")) {
                    config.assets.release_delay_frames = a_node.at("release_delay_frames").get_value<uint32_t>();
                }
                if (a_node.contains("residency_margin")) {
                    config.assets.residency_margin = a_node.at("residency_margin").get_value<float>();
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[pixengine::core::AppConfig] Warning: Failed to parse config file (" << e.what() << "), using defaults.\n";
        }
        return config;
    }
};

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_CORE_CONFIG_H
