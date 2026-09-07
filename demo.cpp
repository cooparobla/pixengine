// pixengine/demo.cpp — Phase 8 demo: the full pipeline. Loads
// assets/scenes/farm/scene.yaml (Transform2D/SpriteRenderer/SpriteAnimator/
// Camera2D/TilemapRenderer) and assets/scenes/hud/scene.yaml (a second,
// independent uicoopa Scene -- RectTransform/Canvas/Text -- since world and
// UI coordinate systems don't mix in one tree). World sprites/tiles are
// sorted/batched through SpriteDrawList and drawn directly at native
// resolution, continuous (unsnapped) positions, into the swapchain render
// pass; the UI Canvas composites on top of that, same render pass, same
// command buffer/submit. A local TopDownMover (not part of the library --
// see pixengine/scene/register.h's doc on scope) walks the Player's
// Transform2D.position.y at a fixed timestep so a single run exercises
// y-sorting and residency streaming without interactive input.
//
// player.pix is deliberately asymmetric (four differently-colored corners +
// a diagonal) so any UV-flip, pivot, or Y-orientation bug in the .pix ->
// world-space path is immediately visible rather than hidden by symmetry.
//
// Build:   cbuild --vulkan     (compiles shaders and cmake builds)
// Run:     cplay               (runs ./build/pixengine_demo)

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <cstdlib>
#include <memory>

#include <gfxcoopa/app/context.h>
#include <coopa/input/input_map.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/command/command_buffer.h>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <uicoopa/layout/canvas.h>
#include <uicoopa/render/ui_pass.h>
#include <uicoopa/ui_yaml.h>

#include <pixengine/core/config.h>
#include <pixengine/render/sprite_pass.h>
#include <pixengine/render/sprite_draw_list.h>
#include <pixengine/data/sprite_asset.h>
#include <pixengine/data/tilemap_asset.h>
#include <pixengine/loaders/pix_loader.h>
#include <pixengine/loaders/tilemap_loader.h>
#include <pixengine/scene/register.h>
#include <pixengine/scene/residency.h>
#include <pixengine/scene/scene_view.h>
#include <pixengine/scene/tilemap_renderer.h>
#include <pixengine/scene/transform2d.h>

#include <root_directory.h>

namespace {

// Not part of the library (see pixengine/scene/register.h's doc comment on
// scope) -- bounces its owner's Transform2D.position.y between min_y/max_y
// at a constant speed, so a single deterministic, fixed-timestep run walks
// both away from and back toward anything placed along its path -- exercises
// y-sorting AND (Phase 7) residency streaming in both directions without
// needing interactive input.
class TopDownMover : public coopa::scene::Component {
public:
    float speed_y = 0.0f; // world units per second; sign flips at the bounds
    float min_y = -1000.0f;
    float max_y = 1000.0f;

    std::string type_name() const override { return "TopDownMover"; }

    void update(float dt) override {
        auto* tf = owner->get_component<coopa::pix::Transform2D>();
        if (!tf) return;
        tf->position.y += speed_y * dt;
        if (tf->position.y > max_y) { tf->position.y = max_y; speed_y = -std::fabs(speed_y); }
        if (tf->position.y < min_y) { tf->position.y = min_y; speed_y = std::fabs(speed_y); }
    }
};

} // namespace

int main() {
    std::cout << "==========================================================\n";
    std::cout << "  pixengine — Phase 8: UI overlay, pre_pass_fn frame pipeline\n";
    std::cout << "==========================================================\n";

    std::string config_path = std::string(ROOT_DIR) + "/assets/config.yaml";
    coopa::pix::AppConfig config = coopa::pix::AppConfig::load(config_path);

    coopa::gfx::app::ContextConfig gfx_config = coopa::gfx::app::ContextConfig::from_env(
        coopa::gfx::app::ContextConfig{
            .title = config.window.title, .width = config.window.width, .height = config.window.height,
            .resizable = config.window.resizable, .vsync = config.window.vsync,
        });
#ifdef NDEBUG
    gfx_config.validation = false;
#endif
    // Context's swapchain render pass is always depth-less (see its constructor's
    // comment); sprite_pass/ui_pass draw directly into it, matching demo.cpp's
    // pre-existing depth-UNDEFINED render pass exactly.
    coopa::gfx::app::Context ctx(gfx_config);

    std::string shader_dir = std::string(ROOT_DIR) + "/assets/shaders";

    // --- Sprite pass, drawing world-space sprites at native resolution
    // directly into the swapchain render pass -- the same one UiPass draws
    // into, sprites first.
    coopa::pix::SpritePass sprite_pass(ctx.device(), ctx.allocator(), ctx.command_pool(), ctx.render_pass(),
                                       shader_dir + "/sprite.vert.spv", shader_dir + "/sprite.frag.spv",
                                       config.renderer.max_textures);

    // --- UI pass, compositing on top, same render pass -- reuses uicoopa's
    // own already-compiled shaders (PROJ_DIR, sibling checkout) rather than
    // duplicating them into pixengine's own assets/shaders/.
    std::string uicoopa_shader_dir = std::string(PROJ_DIR) + "/uicoopa/assets/shaders";
    coopa::ui::UiPass ui_pass(ctx.device(), ctx.allocator(), ctx.command_pool(), ctx.render_pass(),
                              uicoopa_shader_dir + "/ui.vert.spv", uicoopa_shader_dir + "/ui.frag.spv");

    // --- Assets ---
    coopa::asset::AssetManager assets(config.assets.io_threads);
    assets.add_search_root(std::string(ROOT_DIR) + "/assets");
    assets.register_loader<coopa::pix::SpriteAsset>(
        std::make_unique<coopa::pix::PixLoader>(ctx.device(), ctx.allocator(), ctx.command_pool()));
    assets.register_loader<coopa::pix::TilemapAsset>(
        std::make_unique<coopa::pix::TilemapLoader>(ctx.device(), ctx.allocator(), ctx.command_pool()));

    // Phase 7: lean entirely on AssetManager's own eviction machinery once
    // SpriteResidencySystem drops a handle.
    assets.set_idle_eviction(config.assets.idle_eviction);
    assets.set_max_idle_frames(config.assets.max_idle_frames);

    // The scene system itself (coopa::scene) knows nothing about sprites,
    // cameras, 2D transforms, or UI widgets -- register both sets of parsers
    // (they share one process-wide SceneLoader registry keyed by distinct
    // "type:" names, so registration order between the two doesn't matter)
    // before either scene loads.
    coopa::pix::register_pix_components(ctx.device(), ctx.allocator(), ctx.command_pool(), assets);
    coopa::ui::register_ui_components(ctx.device(), ctx.allocator(), ctx.command_pool());

    std::string scene_path = std::string(ROOT_DIR) + "/" + config.scene.default_scene;
    std::string scene_dir = std::filesystem::path(scene_path).parent_path().string();
    std::cout << "[pixengine] Loading scene: " << scene_path << "\n";
    coopa::scene::Scene world = coopa::scene::SceneLoader::load(scene_path);
    std::cout << "[pixengine] Scene '" << world.name() << "' loaded.\n";

    std::string hud_path = std::string(ROOT_DIR) + "/" + config.scene.hud_scene;
    std::cout << "[pixengine] Loading HUD scene: " << hud_path << "\n";
    coopa::scene::Scene hud = coopa::scene::SceneLoader::load(hud_path);
    std::vector<coopa::ui::CanvasComponent*> canvases = coopa::ui::collect_canvases(hud);
    coopa::ui::CanvasComponent* canvas = canvases.empty() ? nullptr : canvases.front();
    coopa::ui::UIResourceCache::instance().mark_text_atlases(ui_pass);
    if (!canvas) {
        std::cerr << "[pixengine] Warning: HUD scene has no Canvas -- UI overlay disabled.\n";
    }

    // Attach the demo-only mover directly (not YAML-driven -- see its class doc).
    // Bounces between well below the wall cluster and well above the Prop
    // cluster (y=10..13), so a single fixed-timestep run walks toward the
    // props (streaming them in), past them, back down, and away again
    // (streaming them out) -- both directions of Phase 7's residency system.
    if (coopa::scene::SceneObject* player_obj = world.find_object("Player")) {
        auto* mover = player_obj->add_component<TopDownMover>();
        mover->speed_y = 4.0f;
        mover->min_y = -8.0f;
        mover->max_y = 15.0f;
    }

    coopa::pix::SceneView view(world);
    coopa::pix::SpriteDrawList draw_list;
    coopa::pix::SpriteResidencySystem residency(assets, scene_dir, config.assets.max_acquires_per_frame,
                                                config.assets.release_delay_frames, config.assets.residency_margin);
    bool debug_placeholder = std::getenv("DEBUG_PLACEHOLDER") != nullptr;

    coopa::input::InputMap input_map;
    input_map.bind("quit", coopa::input::Key::Escape);

    const float kPixelsPerUnit = config.canvas.pixels_per_unit;
    constexpr float kFixedDt = 1.0f / 60.0f;

    // The world's field of view stays fixed at this reference resolution
    // regardless of actual window size -- compute_fit_viewport() (pixel_math.h)
    // uniformly (fractionally, not integer-locked) scales it to best-fit
    // whatever window it's drawn into, centered, with letterbox/pillarbox
    // space on whichever axis has room to spare. This is NOT a return to the
    // old low-res-offscreen-buffer pipeline -- sprites still render directly
    // at native resolution, continuous positions, no pixel snapping; only
    // the world's on-screen SIZE and visible extent are governed by this
    // reference resolution now, via the sprite pass's viewport.
    const uint32_t kReferenceWidth  = config.canvas.reference_width;
    const uint32_t kReferenceHeight = config.canvas.reference_height;

    // Deterministic (MAX_FRAMES/ONESHOT) runs -- used for ctest-adjacent PNG
    // captures and headless-with-GPU verification -- always advance the world
    // by exactly kFixedDt, so a capture is reproducible regardless of how fast
    // the host machine actually rendered it. Interactive runs instead measure
    // real elapsed time: without this, EVERY dropped/late vsync frame (a GPU
    // hitch, a compositor stall, anything) reads as the world visibly freezing
    // for a frame, since the sim would still advance by exactly one 1/60s tick
    // no matter how long that frame actually took. Clamped to 0.25s so a long
    // stall (e.g. window drag) doesn't teleport the world on resume.
    const bool deterministic = gfx_config.headless_oneshot || gfx_config.max_frames != 0;

    std::cout << "[pixengine] Native-resolution rendering, " << kReferenceWidth << "x"
              << kReferenceHeight << " reference resolution best-fit scaled to the window. "
                 "Entering main loop. Press ESC to quit, resize to see it rescale.\n";

    int frame_count = 0;
    while (!ctx.should_close()) {
        ctx.poll(); // window.new_frame() + poll_events() + frame timer update.

        float dt = deterministic ? kFixedDt : std::clamp(ctx.delta_time(), 0.0f, 0.25f);

        if (input_map.is_down("quit", ctx.input())) {
            ctx.window().set_should_close(true);
        }

        // Context's resize handler already recreates the swapchain/framebuffers
        // internally (see gfxcoopa/app/context.h) -- no manual was_resized()/
        // recreate() dance needed here anymore.
        //
        // Derived from ctx.extent(), not window.framebuffer_size() -- these can
        // transiently disagree during a resize, and these numbers also drive
        // inv_half_extent and visible_world_rect(), where a mismatch would show
        // as a one-frame aspect distortion rather than a harmless clip.
        coopa::gfx::Extent2D extent = ctx.extent();
        uint32_t cur_w = extent.width, cur_h = extent.height;
        if (cur_w == 0 || cur_h == 0) {
            // Minimized: nothing to draw this iteration.
            continue;
        }

        assets.update(dt);

        world.update(dt);
        world.late_update(dt);
        coopa::pix::resolve_transforms(world);
        view.refresh_if_dirty(); // static scene this phase -- rebuilds once, on the first frame

        // HUD: Canvas::late_update() drives measure -> arrange -> emit into its
        // own DrawList (see uicoopa/layout/canvas.h) -- set_viewport/input first.
        if (canvas) {
            canvas->set_viewport(cur_w, cur_h);
            canvas->set_input(ctx.input());
        }
        hud.update(dt);
        hud.late_update(dt);

        coopa::pix::Camera2D* camera = view.active_camera();
        float zoom = camera ? camera->zoom : 1.0f;
        float pixel_grid_scale = kPixelsPerUnit * zoom;

        // Visible extent is governed by the fixed REFERENCE resolution, not
        // the actual window size -- resizing the window changes how big that
        // fixed field of view renders (via fit_viewport below), not how much
        // world is in it.
        coopa::ui::Rect visible_rect = camera
            ? camera->visible_world_rect(kPixelsPerUnit, kReferenceWidth, kReferenceHeight)
            : coopa::ui::Rect{};

        coopa::pix::FitViewport fit_viewport =
            coopa::pix::compute_fit_viewport(cur_w, cur_h, kReferenceWidth, kReferenceHeight);

        // Acquire/release handles for this frame BEFORE emitting -- a sprite
        // that just entered range this frame issues a load_async() here and
        // simply won't have a loaded handle to draw yet (see SpriteRenderer::
        // emit()'s debug_show_unloaded doc for why that's the intended
        // behavior, not a bug to work around).
        residency.tick(view.sprite_renderers(), visible_rect);

        draw_list.begin();
        uint32_t tile_quads = 0;
        for (coopa::pix::TilemapRenderer* tm : view.tilemaps()) {
            tm->emit(draw_list, visible_rect, tile_quads);
        }
        size_t sprite_quads_before = draw_list.quad_count();
        uint32_t loaded_sprites = 0;
        for (const coopa::pix::SpriteRef& ref : view.sprite_renderers()) {
            ref.emit(draw_list, debug_placeholder);
            if (ref.renderer && ref.renderer->handle.is_loaded()) ++loaded_sprites;
        }
        uint32_t sprite_quads = static_cast<uint32_t>(draw_list.quad_count() - sprite_quads_before);
        draw_list.sort_and_flatten();

        // Verification aid (Phase 6/7): confirms tilemap culling keeps the
        // per-frame quad count in the low thousands, not ~65000 (30x20x2
        // layers), and shows the residency streaming boundary moving as the
        // sprite count with a currently-loaded GPU atlas rises and falls.
        if (frame_count % 30 == 0) {
            std::cout << "[pixengine] quads: tiles=" << tile_quads << " sprites=" << sprite_quads
                      << " total=" << draw_list.quad_count()
                      << " | loaded_sprite_handles=" << loaded_sprites
                      << " | player.y=" << (world.find_object("Player")
                                                ? world.find_object("Player")->get_component<coopa::pix::Transform2D>()->position.y
                                                : 0.0f)
                      << "\n";
        }

        // MUST run before renderer.begin_frame(): DescriptorSet::bind_image()
        // updates immediately, which is unsafe once a render pass is open. Both
        // passes' textures resolve here, in the one place that's guaranteed to
        // run before ANY render pass opens this frame.
        sprite_pass.register_textures(draw_list);
        if (canvas) ui_pass.register_textures(canvas->draw_list());

        // Based on the REFERENCE resolution, not cur_w/cur_h -- this maps
        // world space to NDC as if always rendering at kReferenceWidth x
        // kReferenceHeight. The sprite pass's viewport (fit_viewport, set in
        // draw() below) then scales that fixed NDC space into the actual
        // on-screen rect, so the effective screen-px-per-world-unit works
        // out to kPixelsPerUnit * zoom * fit_viewport.scale automatically,
        // without needing to fold fit_viewport.scale in here explicitly.
        glm::vec2 camera_world_pos = camera ? camera->world_position() : glm::vec2(0.0f);
        coopa::pix::SpritePush camera_push{};
        camera_push.inv_half_extent[0] = 1.0f / (static_cast<float>(kReferenceWidth) / (2.0f * pixel_grid_scale));
        camera_push.inv_half_extent[1] = 1.0f / (static_cast<float>(kReferenceHeight) / (2.0f * pixel_grid_scale));
        camera_push.camera_pos[0] = camera_world_pos.x;
        camera_push.camera_pos[1] = camera_world_pos.y;
        camera_push.tint[0] = camera_push.tint[1] = camera_push.tint[2] = camera_push.tint[3] = 1.0f;

        coopa::gfx::app::FrameCallbacks cb;
        cb.record = [&](coopa::gfx::command::CommandBuffer& cmd) {
            sprite_pass.draw(cmd, ctx.current_frame(), camera_push, draw_list,
                             fit_viewport, cur_w, cur_h);
            if (canvas) {
                ui_pass.draw(cmd, ctx.current_frame(), cur_w, cur_h,
                            canvas->scale_factor(), canvas->draw_list());
            }
        };
        cb.clear = coopa::gfx::ClearColor{0.05f, 0.05f, 0.05f, 1.0f};
        ctx.frame(cb);

        ++frame_count; // unconditional -- the % 30 debug print above (and any
                       // future per-frame throttle) depends on this advancing
                       // every iteration, not just when MAX_FRAMES is set.
        if (gfx_config.headless_oneshot) {
            break;
        }
        if (ctx.max_frames() > 0 && static_cast<uint32_t>(frame_count) >= ctx.max_frames()) {
            break;
        }
    }

    ctx.wait_idle();

    // register_pix_components()'s parser lambdas capture assets by reference
    // in SceneLoader's function-local static registry, which would otherwise
    // only be destroyed at program exit -- after device/allocator/assets below
    // go out of scope. Clear it now, while they're still alive.
    coopa::scene::SceneLoader::clear_component_parsers();
    assets.shutdown();

    // UIResourceCache owns GPU-resident Fonts/Textures (the HUD's font atlas)
    // in static storage, which would likewise only tear down at program exit,
    // after device/allocator are already gone -- the exact same pitfall as
    // above, one level removed (see uicoopa/test_window.cpp's own teardown).
    coopa::ui::UIResourceCache::instance().clear();

    std::cout << "[pixengine] Shutting down.\n";
    return 0;
}
