// pixengine/test.cpp — headless test suite (no window, no swapchain).
//
// Every test in this file must be runnable without a display: pixengine/pix/
// and pixengine/math/ never include <volk/volk.h>, which is what keeps this
// suite safe for `ctest`/CI. GPU-touching behavior (SpritePass, UiPass,
// AssetManager streaming, resize, real visual output) is validated by
// demo.cpp instead, which is deliberately excluded from ctest.
//
// Build: cbuild --vulkan   Run: ctest --test-dir build --output-on-failure

#include <algorithm>
#include <iostream>
#include <string>
#include <stdexcept>
#include <cmath>
#include <vector>

#include <filesystem>

#include <pixengine/math/pixel_math.h>
#include <pixengine/pix/color.h>
#include <pixengine/pix/pix_document.h>
#include <pixengine/pix/pix_decoder.h>
#include <pixengine/pix/pix_composite.h>
#include <pixengine/pix/atlas_packer.h>
#include <pixengine/pix/sprite_meta.h>
#include <caml/caml.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <pixengine/scene/camera2d.h>
#include <pixengine/scene/transform2d.h>
#include <pixengine/render/sprite_draw_list.h>
#include <pixengine/data/tilemap_asset.h>
#include <pixengine/data/tilemap_decoder.h>
#include <pixengine/scene/residency.h>
#include <pixengine/input/input_map.h>

// ANSI Colors for nice test output
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_RESET   "\x1b[0m"

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define RUN_TEST(test_func) \
    do { \
        std::cout << ANSI_COLOR_BLUE << "[ RUN      ] " << ANSI_COLOR_RESET << #test_func << std::endl; \
        g_tests_run++; \
        try { \
            test_func(); \
            std::cout << ANSI_COLOR_GREEN << "[       OK ] " << ANSI_COLOR_RESET << #test_func << std::endl; \
        } catch (const std::exception& e) { \
            std::cerr << ANSI_COLOR_RED << "[  FAILED  ] " << ANSI_COLOR_RESET << #test_func << " (Exception: " << e.what() << ")" << std::endl; \
            g_tests_failed++; \
        } catch (...) { \
            std::cerr << ANSI_COLOR_RED << "[  FAILED  ] " << ANSI_COLOR_RESET << #test_func << " (Unknown Exception)" << std::endl; \
            g_tests_failed++; \
        } \
    } while (0)

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << ANSI_COLOR_RED << "  Assertion failed: " << #condition << " at " << __FILE__ << ":" << __LINE__ << ANSI_COLOR_RESET << std::endl; \
            throw std::runtime_error("Assertion failed: " #condition); \
        } \
    } while (0)

#define ASSERT_EQ(val1, val2) \
    do { \
        if ((val1) != (val2)) { \
            std::cerr << ANSI_COLOR_RED << "  Assertion failed: " << #val1 << " == " << #val2 \
                      << " (Actual: " << (val1) << ", Expected: " << (val2) << ") at " \
                      << __FILE__ << ":" << __LINE__ << ANSI_COLOR_RESET << std::endl; \
            throw std::runtime_error("Assertion failed: " #val1 " == " #val2); \
        } \
    } while (0)

#define ASSERT_NEAR(val1, val2, eps) \
    do { \
        if (std::fabs((val1) - (val2)) > (eps)) { \
            std::cerr << ANSI_COLOR_RED << "  Assertion failed: " << #val1 << " ~= " << #val2 \
                      << " (Actual: " << (val1) << ", Expected: " << (val2) << ", eps: " << (eps) << ") at " \
                      << __FILE__ << ":" << __LINE__ << ANSI_COLOR_RESET << std::endl; \
            throw std::runtime_error("Assertion failed: " #val1 " ~= " #val2); \
        } \
    } while (0)

// --- Phase 1: harness smoke test -------------------------------------------
// Later phases add real suites here (pix decode/composite/atlas, pixel_math,
// Camera2D, tilemap culling, SpriteDrawList, decide_residency, ...).

static void test_harness_smoke() {
    ASSERT_TRUE(1 + 1 == 2);
    ASSERT_EQ(2 + 2, 4);
    ASSERT_NEAR(1.0f, 1.0001f, 1e-2f);
}

// --- pixel_math: compute_fit_viewport -------------------------------------

static void test_fit_viewport_exact_multiples() {
    using coopa::pix::compute_fit_viewport;
    // 1920x1080 window, 480x270 reference -> exact 4x, zero letterbox.
    auto vp = compute_fit_viewport(1920, 1080, 480, 270);
    ASSERT_NEAR(vp.scale, 4.0f, 1e-4f);
    ASSERT_NEAR(vp.w, 1920.0f, 1e-3f);
    ASSERT_NEAR(vp.h, 1080.0f, 1e-3f);
    ASSERT_NEAR(vp.x, 0.0f, 1e-4f);
    ASSERT_NEAR(vp.y, 0.0f, 1e-4f);
}

static void test_fit_viewport_non_exact_centers_bars() {
    using coopa::pix::compute_fit_viewport;
    // 1000x1000 window, 480x270 reference -> scale = min(1000/480, 1000/270)
    // = min(2.0833, 3.7037) = 2.0833..., height-constrained axis has zero
    // bar, width axis gets the leftover centered.
    auto vp = compute_fit_viewport(1000, 1000, 480, 270);
    float expected_scale = 1000.0f / 480.0f;
    ASSERT_NEAR(vp.scale, expected_scale, 1e-4f);
    ASSERT_NEAR(vp.w, 1000.0f, 1e-2f);       // width axis is the constraint -> fills exactly
    ASSERT_NEAR(vp.h, 270.0f * expected_scale, 1e-2f);
    ASSERT_NEAR(vp.x, 0.0f, 1e-3f);
    ASSERT_NEAR(vp.y, (1000.0f - vp.h) / 2.0f, 1e-2f);
}

static void test_fit_viewport_window_smaller_than_reference_scales_down() {
    using coopa::pix::compute_fit_viewport;
    // Unlike the old integer-only Letterbox (which floored to a minimum
    // scale of 1, forcing an oversized/cropped rect), this shrinks smoothly
    // below 1 and always fits fully inside the window -- no negative x/y.
    auto vp = compute_fit_viewport(200, 150, 480, 270);
    ASSERT_TRUE(vp.scale < 1.0f);
    ASSERT_TRUE(vp.scale > 0.0f);
    ASSERT_TRUE(vp.w <= 200.0f + 1e-2f);
    ASSERT_TRUE(vp.h <= 150.0f + 1e-2f);
    ASSERT_TRUE(vp.x >= 0.0f);
    ASSERT_TRUE(vp.y >= 0.0f);
}

static void test_fit_viewport_extreme_aspect_ratio() {
    using coopa::pix::compute_fit_viewport;
    // Ultra-wide window: height is the binding constraint.
    auto vp = compute_fit_viewport(3440, 300, 480, 270);
    float expected_scale = 300.0f / 270.0f;
    ASSERT_NEAR(vp.scale, expected_scale, 1e-4f);
    ASSERT_NEAR(vp.h, 300.0f, 1e-2f);
    ASSERT_TRUE(vp.w <= 3440.0f + 1e-2f);
    ASSERT_TRUE(vp.x >= 0.0f);
}

static void test_fit_viewport_always_preserves_aspect_ratio() {
    using coopa::pix::compute_fit_viewport;
    struct { uint32_t sw, sh; } windows[] = {
        {1920, 1080}, {1000, 1000}, {200, 150}, {3440, 300}, {800, 2000}, {2000, 800},
    };
    float ref_aspect = 480.0f / 270.0f;
    for (auto& win : windows) {
        auto vp = compute_fit_viewport(win.sw, win.sh, 480, 270);
        ASSERT_NEAR(vp.w / vp.h, ref_aspect, 1e-3f);
        // Always fully contained within the window.
        ASSERT_TRUE(vp.x >= -1e-3f);
        ASSERT_TRUE(vp.y >= -1e-3f);
        ASSERT_TRUE(vp.x + vp.w <= static_cast<float>(win.sw) + 1e-2f);
        ASSERT_TRUE(vp.y + vp.h <= static_cast<float>(win.sh) + 1e-2f);
    }
}

static void test_fit_viewport_zero_dimension_inputs_are_safe() {
    using coopa::pix::compute_fit_viewport;
    auto vp1 = compute_fit_viewport(0, 1080, 480, 270);
    ASSERT_NEAR(vp1.w, 0.0f, 1e-6f);
    ASSERT_NEAR(vp1.scale, 0.0f, 1e-6f);
    auto vp2 = compute_fit_viewport(1920, 1080, 0, 270);
    ASSERT_NEAR(vp2.w, 0.0f, 1e-6f);
}

// --- color.h ---------------------------------------------------------------

static void test_color_pack_byte_order_matches_ui_vertex() {
    using namespace coopa::pix;
    // r in the low byte, matching coopa::ui::UiVertex::pack_color()'s layout
    // (VK_FORMAT_R8G8B8A8_UNORM) so a packed atlas pixel needs no reordering.
    uint32_t c = pack_rgba8(0x11, 0x22, 0x33, 0x44);
    ASSERT_EQ(c, 0x44332211u);
    ASSERT_EQ(rgba8_r(c), 0x11);
    ASSERT_EQ(rgba8_g(c), 0x22);
    ASSERT_EQ(rgba8_b(c), 0x33);
    ASSERT_EQ(rgba8_a(c), 0x44);
}

static void test_parse_hex_color_upper_and_lower() {
    using namespace coopa::pix;
    ASSERT_EQ(parse_hex_color("#FF0000FF"), pack_rgba8(255, 0, 0, 255));
    ASSERT_EQ(parse_hex_color("#ff0000ff"), pack_rgba8(255, 0, 0, 255));
    ASSERT_EQ(parse_hex_color("#00Ff80aC"), pack_rgba8(0x00, 0xFF, 0x80, 0xAC));
}

static void test_parse_hex_color_malformed_throws() {
    using namespace coopa::pix;
    auto expect_throw = [](const std::string& s) {
        bool threw = false;
        try { parse_hex_color(s); } catch (const std::invalid_argument&) { threw = true; }
        ASSERT_TRUE(threw);
    };
    expect_throw("FF0000FF");     // missing '#'
    expect_throw("#FF00");        // too short
    expect_throw("#GG0000FF");    // invalid hex digit
    expect_throw("#FF0000FFAA");  // too long
}

static void test_source_over_blending() {
    using namespace coopa::pix;
    uint32_t opaque_red = pack_rgba8(255, 0, 0, 255);
    uint32_t opaque_blue = pack_rgba8(0, 0, 255, 255);
    uint32_t transparent_red = pack_rgba8(255, 0, 0, 0);

    // Opaque src fully replaces dst.
    ASSERT_EQ(source_over(opaque_red, opaque_blue), opaque_red);
    // Fully transparent src leaves dst unchanged.
    ASSERT_EQ(source_over(transparent_red, opaque_blue), opaque_blue);

    // Half-alpha src over opaque dst: result stays fully opaque (over an
    // opaque backdrop), with rgb pulled partway from dst toward src.
    uint32_t half_red = pack_rgba8(255, 0, 0, 128);
    uint32_t blended = source_over(half_red, opaque_blue);
    ASSERT_EQ(rgba8_a(blended), 255);
    ASSERT_TRUE(rgba8_r(blended) > 100 && rgba8_r(blended) < 150);
    ASSERT_TRUE(rgba8_b(blended) > 100 && rgba8_b(blended) < 150);
}

static void test_premultiply() {
    using namespace coopa::pix;
    uint32_t opaque = pack_rgba8(200, 100, 50, 255);
    uint32_t pm_opaque = premultiply(opaque);
    ASSERT_EQ(pm_opaque, opaque); // alpha=1 -> premultiply is a no-op

    uint32_t transparent = pack_rgba8(200, 100, 50, 0);
    uint32_t pm_transparent = premultiply(transparent);
    ASSERT_EQ(rgba8_r(pm_transparent), 0);
    ASSERT_EQ(rgba8_a(pm_transparent), 0);

    uint32_t half = pack_rgba8(255, 0, 0, 128);
    uint32_t pm_half = premultiply(half);
    ASSERT_EQ(pm_half, pack_rgba8(128, 0, 0, 128)); // 255 * (128/255) rounds to 128
}

// --- pix_decoder.h -----------------------------------------------------------

static void test_pix_decode_shape_animations() {
    using namespace coopa::pix;
    std::string yaml = R"YAML(
format: coopixel
version: "1.0"
width: 4
height: 3
animations:
  - name: idle
    fps: 12
    frames:
      - name: Frame 1
        duration_ms: 80
        layers:
          - name: Background
            visible: true
            opacity: 1.0
            pixels:
              "1,1": "#00FF00FF"
              "0,0": "#FF0000FF"
)YAML";
    PixDocument doc = decode_pix_document(yaml);
    ASSERT_EQ(doc.width, 4);
    ASSERT_EQ(doc.height, 3);
    ASSERT_EQ(doc.animations.size(), 1u);
    ASSERT_TRUE(doc.animations[0].name == "idle");
    ASSERT_EQ(doc.animations[0].fps, 12);
    ASSERT_EQ(doc.animations[0].frames.size(), 1u);
    const PixFrame& frame = doc.animations[0].frames[0];
    ASSERT_TRUE(frame.name == "Frame 1");
    ASSERT_EQ(frame.duration_ms, 80);
    ASSERT_EQ(frame.layers.size(), 1u);
    const PixLayer& layer = frame.layers[0];
    ASSERT_TRUE(layer.name == "Background");
    ASSERT_EQ(layer.pixels.size(), 2u);
    // Sorted by (y, x) ascending: (0,0) before (1,1) regardless of YAML order.
    ASSERT_EQ(layer.pixels[0].first.x, 0);
    ASSERT_EQ(layer.pixels[0].first.y, 0);
    ASSERT_EQ(layer.pixels[0].second, parse_hex_color("#FF0000FF"));
    ASSERT_EQ(layer.pixels[1].first.x, 1);
    ASSERT_EQ(layer.pixels[1].first.y, 1);
}

static void test_pix_decode_shape_legacy_frames() {
    using namespace coopa::pix;
    std::string yaml = R"YAML(
format: coopixel
width: 2
height: 2
fps: 15
frames:
  - name: Frame A
    duration_ms: 50
    layers:
      - name: L1
        pixels:
          "0,0": "#0000FFFF"
)YAML";
    PixDocument doc = decode_pix_document(yaml);
    ASSERT_EQ(doc.animations.size(), 1u);
    ASSERT_TRUE(doc.animations[0].name == "new-animation");
    ASSERT_EQ(doc.animations[0].fps, 15);
    ASSERT_EQ(doc.animations[0].frames.size(), 1u);
    ASSERT_TRUE(doc.animations[0].frames[0].name == "Frame A");
    ASSERT_EQ(doc.animations[0].frames[0].duration_ms, 50);
}

static void test_pix_decode_shape_legacy_layers() {
    using namespace coopa::pix;
    std::string yaml = R"YAML(
format: coopixel
width: 2
height: 2
layers:
  - name: OnlyLayer
    pixels:
      "1,0": "#FFFFFFFF"
)YAML";
    PixDocument doc = decode_pix_document(yaml);
    ASSERT_EQ(doc.animations.size(), 1u);
    ASSERT_EQ(doc.animations[0].frames.size(), 1u);
    ASSERT_TRUE(doc.animations[0].frames[0].name == "Frame 1");
    ASSERT_EQ(doc.animations[0].frames[0].layers.size(), 1u);
    ASSERT_TRUE(doc.animations[0].frames[0].layers[0].name == "OnlyLayer");
    ASSERT_EQ(doc.animations[0].frames[0].layers[0].pixels[0].first.x, 1);
    ASSERT_EQ(doc.animations[0].frames[0].layers[0].pixels[0].first.y, 0);
}

static void test_pix_decode_malformed_pixel_key_throws() {
    using namespace coopa::pix;
    std::string yaml = R"YAML(
format: coopixel
width: 2
height: 2
layers:
  - name: Bad
    pixels:
      "not_a_coord": "#FFFFFFFF"
)YAML";
    bool threw = false;
    try {
        decode_pix_document(yaml);
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

static void test_pix_decode_missing_fields_use_defaults() {
    using namespace coopa::pix;
    // No width/height/animations/frames/layers at all.
    std::string yaml = "format: coopixel\n";
    PixDocument doc = decode_pix_document(yaml);
    ASSERT_EQ(doc.width, 32);
    ASSERT_EQ(doc.height, 32);
    ASSERT_EQ(doc.animations.size(), 1u);
    ASSERT_EQ(doc.animations[0].frames.size(), 1u);
    ASSERT_EQ(doc.animations[0].frames[0].layers.size(), 1u);
    ASSERT_TRUE(doc.animations[0].frames[0].layers[0].pixels.empty());
}

// --- pix_composite.h ---------------------------------------------------------

static uint32_t canvas_at(const std::vector<uint32_t>& canvas, int width, int x, int y) {
    return canvas[static_cast<size_t>(y) * width + x];
}

static void test_composite_layer_order_later_layer_on_top() {
    using namespace coopa::pix;
    PixFrame frame;
    frame.layers.clear();
    PixLayer bottom; bottom.name = "bottom"; bottom.pixels = {{{0, 0}, pack_rgba8(255, 0, 0, 255)}};
    PixLayer top;    top.name = "top";       top.pixels    = {{{0, 0}, pack_rgba8(0, 0, 255, 255)}};
    frame.layers = {bottom, top};

    auto canvas = composite_frame(2, 2, frame, /*apply_effects=*/false);
    uint32_t px = canvas_at(canvas, 2, 0, 0);
    ASSERT_EQ(rgba8_r(px), 0);
    ASSERT_EQ(rgba8_b(px), 255);
}

static void test_composite_invisible_and_zero_opacity_layers_skipped() {
    using namespace coopa::pix;
    PixFrame frame;
    frame.layers.clear();
    PixLayer hidden; hidden.name = "hidden"; hidden.visible = false;
    hidden.pixels = {{{0, 0}, pack_rgba8(255, 0, 0, 255)}};
    PixLayer zero_opacity; zero_opacity.name = "zero"; zero_opacity.opacity = 0.0f;
    zero_opacity.pixels = {{{0, 0}, pack_rgba8(0, 255, 0, 255)}};
    frame.layers = {hidden, zero_opacity};

    auto canvas = composite_frame(2, 2, frame, false);
    ASSERT_EQ(rgba8_a(canvas_at(canvas, 2, 0, 0)), 0); // nothing drawn -> stays transparent
}

static void test_composite_fractional_opacity_math() {
    using namespace coopa::pix;
    PixFrame frame;
    frame.layers.clear();
    PixLayer layer; layer.name = "half"; layer.opacity = 0.5f;
    layer.pixels = {{{0, 0}, pack_rgba8(255, 0, 0, 255)}};
    frame.layers = {layer};

    auto canvas = composite_frame(1, 1, frame, false);
    uint32_t px = canvas_at(canvas, 1, 0, 0);
    // Straight alpha after opacity scaling is 128/255; composited over a fully
    // transparent canvas the result equals the source; the final premultiply
    // pass then scales r (255) by that same 128/255 -> 128, exactly.
    ASSERT_EQ(rgba8_r(px), 128);
    ASSERT_EQ(rgba8_a(px), 128);
    ASSERT_EQ(rgba8_g(px), 0);
}

static void test_composite_stroke_outside() {
    using namespace coopa::pix;
    PixFrame frame;
    frame.layers.clear();
    PixLayer layer;
    layer.pixels = {{{2, 2}, pack_rgba8(255, 0, 0, 255)}};
    PixEffect stroke;
    stroke.type = "stroke"; stroke.enabled = true; stroke.size = 1;
    stroke.color = pack_rgba8(0, 0, 0, 255);
    stroke.position = "outside";
    layer.effects = {stroke};
    frame.layers = {layer};

    auto canvas = composite_frame(5, 5, frame, /*apply_effects=*/true);

    // The base pixel keeps its own color -- the "below" stroke never overlaps
    // filled coordinates, proving below-then-base draw order.
    ASSERT_EQ(rgba8_r(canvas_at(canvas, 5, 2, 2)), 255);

    // All 8 neighbors (within (radius+0.5)^2 = 2.25) are painted black.
    int neighbors[8][2] = {{1,1},{2,1},{3,1},{1,2},{3,2},{1,3},{2,3},{3,3}};
    for (auto& n : neighbors) {
        uint32_t px = canvas_at(canvas, 5, n[0], n[1]);
        ASSERT_EQ(rgba8_a(px), 255);
        ASSERT_EQ(rgba8_r(px), 0);
    }
    // Out of stroke range: stays transparent.
    ASSERT_EQ(rgba8_a(canvas_at(canvas, 5, 0, 0)), 0);
}

static void test_composite_stroke_inside_and_above_ordering() {
    using namespace coopa::pix;
    PixFrame frame;
    frame.layers.clear();
    PixLayer layer;
    // An isolated single filled pixel has zero filled neighbors, so it is
    // always a "border" pixel under the inside-stroke rule -- its own
    // coordinate gets an "above" stroke entry, which must composite OVER
    // the base pixel already drawn there (below -> base -> above ordering).
    layer.pixels = {{{2, 2}, pack_rgba8(255, 0, 0, 255)}};
    PixEffect stroke;
    stroke.type = "stroke"; stroke.enabled = true; stroke.size = 1;
    stroke.color = pack_rgba8(0, 0, 0, 255);
    stroke.position = "inside";
    layer.effects = {stroke};
    frame.layers = {layer};

    auto canvas = composite_frame(5, 5, frame, true);
    uint32_t px = canvas_at(canvas, 5, 2, 2);
    ASSERT_EQ(rgba8_r(px), 0); // black stroke drawn above wins over the red base
    ASSERT_EQ(rgba8_a(px), 255);
}

static void test_composite_stroke_center_positions_both_sides() {
    using namespace coopa::pix;
    PixFrame frame;
    frame.layers.clear();
    PixLayer layer;
    layer.pixels = {{{2, 2}, pack_rgba8(255, 0, 0, 255)}};
    PixEffect stroke;
    stroke.type = "stroke"; stroke.enabled = true; stroke.size = 2; // out_radius = max(1, 2/2) = 1
    stroke.color = pack_rgba8(0, 0, 0, 255);
    stroke.position = "center";
    layer.effects = {stroke};
    frame.layers = {layer};

    auto canvas = composite_frame(5, 5, frame, true);
    // The filled coordinate itself is in the "above" set -> stroke color wins.
    ASSERT_EQ(rgba8_r(canvas_at(canvas, 5, 2, 2)), 0);
    // A neighboring, previously-empty coordinate is in the "below" set ->
    // composited straight onto transparent, so it becomes the stroke color too.
    ASSERT_EQ(rgba8_r(canvas_at(canvas, 5, 2, 1)), 0);
    ASSERT_EQ(rgba8_a(canvas_at(canvas, 5, 2, 1)), 255);
}

static void test_trim_opaque_normal_and_all_transparent() {
    using namespace coopa::pix;
    // 4x4 canvas, opaque only in the 2x2 block at (1,1)-(2,2).
    std::vector<uint32_t> canvas(16, pack_rgba8(0, 0, 0, 0));
    canvas[1 * 4 + 1] = pack_rgba8(255, 0, 0, 255);
    canvas[1 * 4 + 2] = pack_rgba8(255, 0, 0, 255);
    canvas[2 * 4 + 1] = pack_rgba8(255, 0, 0, 255);
    canvas[2 * 4 + 2] = pack_rgba8(255, 0, 0, 255);

    TrimmedImage trimmed = trim_opaque(canvas, 4, 4);
    ASSERT_EQ(trimmed.width, 2);
    ASSERT_EQ(trimmed.height, 2);
    ASSERT_EQ(trimmed.offset.x, 1);
    ASSERT_EQ(trimmed.offset.y, 1);
    ASSERT_EQ(trimmed.pixels.size(), 4u);

    std::vector<uint32_t> empty_canvas(16, pack_rgba8(0, 0, 0, 0));
    TrimmedImage empty_trimmed = trim_opaque(empty_canvas, 4, 4);
    ASSERT_EQ(empty_trimmed.width, 1);
    ASSERT_EQ(empty_trimmed.height, 1);
    ASSERT_EQ(empty_trimmed.pixels.size(), 1u);
    ASSERT_EQ(rgba8_a(empty_trimmed.pixels[0]), 0);
}

// --- atlas_packer.h -----------------------------------------------------------

static bool rects_overlap(const coopa::pix::AtlasRect& a, const coopa::pix::AtlasRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void test_atlas_packer_no_overlap_and_padding_respected() {
    using namespace coopa::pix;
    std::vector<glm::ivec2> sizes = {{10, 10}, {20, 5}, {5, 5}, {15, 15}, {8, 30}};
    AtlasPackResult result = pack_shelf(sizes, /*padding=*/1, /*max_dimension=*/4096);

    ASSERT_EQ(result.rects.size(), sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i) {
        ASSERT_EQ(result.rects[i].w, sizes[i].x);
        ASSERT_EQ(result.rects[i].h, sizes[i].y);
        ASSERT_TRUE(result.rects[i].x >= 1);
        ASSERT_TRUE(result.rects[i].y >= 1);
        ASSERT_TRUE(result.rects[i].x + result.rects[i].w + 1 <= result.atlas_width);
        ASSERT_TRUE(result.rects[i].y + result.rects[i].h + 1 <= result.atlas_height);
    }
    for (size_t i = 0; i < result.rects.size(); ++i) {
        for (size_t j = i + 1; j < result.rects.size(); ++j) {
            ASSERT_TRUE(!rects_overlap(result.rects[i], result.rects[j]));
        }
    }
}

static void test_atlas_packer_deterministic() {
    using namespace coopa::pix;
    std::vector<glm::ivec2> sizes = {{12, 7}, {7, 12}, {12, 7}, {3, 3}};
    AtlasPackResult r1 = pack_shelf(sizes);
    AtlasPackResult r2 = pack_shelf(sizes);
    ASSERT_EQ(r1.atlas_width, r2.atlas_width);
    ASSERT_EQ(r1.atlas_height, r2.atlas_height);
    for (size_t i = 0; i < sizes.size(); ++i) {
        ASSERT_EQ(r1.rects[i].x, r2.rects[i].x);
        ASSERT_EQ(r1.rects[i].y, r2.rects[i].y);
    }
}

static void test_atlas_packer_power_of_two_growth() {
    using namespace coopa::pix;
    std::vector<glm::ivec2> sizes;
    for (int i = 0; i < 40; ++i) sizes.push_back({16, 16});
    AtlasPackResult r = pack_shelf(sizes);
    ASSERT_EQ(next_pow2(r.atlas_width), r.atlas_width);
    ASSERT_EQ(next_pow2(r.atlas_height), r.atlas_height);
}

static void test_atlas_packer_over_cap_throws() {
    using namespace coopa::pix;
    std::vector<glm::ivec2> sizes = {{100, 100}, {100, 100}, {100, 100}, {100, 100}};
    bool threw = false;
    try {
        pack_shelf(sizes, /*padding=*/1, /*max_dimension=*/64);
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

static void test_atlas_packer_single_and_zero_rect_edge_cases() {
    using namespace coopa::pix;
    std::vector<glm::ivec2> one = {{13, 9}};
    AtlasPackResult r1 = pack_shelf(one);
    ASSERT_EQ(r1.rects.size(), 1u);
    ASSERT_EQ(r1.rects[0].w, 13);
    ASSERT_EQ(r1.rects[0].h, 9);

    std::vector<glm::ivec2> none;
    AtlasPackResult r0 = pack_shelf(none);
    ASSERT_EQ(r0.rects.size(), 0u);
    ASSERT_EQ(r0.atlas_width, 0);
    ASSERT_EQ(r0.atlas_height, 0);
}

// --- sprite_meta.h ------------------------------------------------------------

static void test_sprite_meta_defaults_when_minimal() {
    using namespace coopa::pix;
    SpriteMeta meta = parse_sprite_meta("format: pixengine_sprite\nversion: \"1.0\"\n");
    ASSERT_NEAR(meta.pixels_per_unit, 16.0f, 1e-4f);
    ASSERT_NEAR(meta.pivot.x, 0.5f, 1e-4f);
    ASSERT_NEAR(meta.pivot.y, 0.0f, 1e-4f);
    ASSERT_TRUE(meta.apply_effects);
    ASSERT_TRUE(meta.premultiply);
    ASSERT_TRUE(meta.animations.empty());
    ASSERT_TRUE(meta.hitboxes.empty());
    ASSERT_TRUE(meta.anchors.empty());
}

static void test_sprite_meta_overrides_and_unknown_keys_ignored() {
    using namespace coopa::pix;
    std::string yaml = R"YAML(
format: pixengine_sprite
version: "1.0"
pixels_per_unit: 32
pivot: { x: 0.25, y: 0.1 }
apply_effects: false
some_future_unrecognized_key: { nested: true }
animations:
  walk_down:
    loop: Loop
    speed: 1.5
    pivot: { x: 0.5, y: 0.0 }
    events:
      - { frame: 1, name: footstep }
      - { frame: 4, name: footstep }
    frames:
      - { index: 2, duration_ms: 60 }
  chop:
    loop: Once
    events:
      - { frame: 3, name: hit }
hitboxes:
  - { name: body, rect: { x: -0.3, y: 0.0, w: 0.6, h: 0.9 } }
anchors:
  hand_r: { x: 0.35, y: 0.55 }
)YAML";
    SpriteMeta meta = parse_sprite_meta(yaml); // unrecognized top-level key must not throw
    ASSERT_NEAR(meta.pixels_per_unit, 32.0f, 1e-4f);
    ASSERT_NEAR(meta.pivot.x, 0.25f, 1e-4f);
    ASSERT_TRUE(!meta.apply_effects);
    ASSERT_EQ(meta.animations.size(), 2u);

    AnimMeta walk = find_anim_meta(meta, "walk_down");
    ASSERT_TRUE(walk.loop == LoopMode::Loop);
    ASSERT_NEAR(walk.speed, 1.5f, 1e-4f);
    ASSERT_TRUE(walk.has_pivot);
    ASSERT_EQ(walk.events.size(), 2u);
    ASSERT_EQ(walk.events[0].frame, 1);
    ASSERT_TRUE(walk.events[0].name == "footstep");
    ASSERT_EQ(walk.frame_overrides.size(), 1u);
    ASSERT_EQ(walk.frame_overrides[0].index, 2);
    ASSERT_EQ(walk.frame_overrides[0].duration_ms, 60);

    AnimMeta chop = find_anim_meta(meta, "chop");
    ASSERT_TRUE(chop.loop == LoopMode::Once);
    ASSERT_TRUE(!chop.has_pivot);

    AnimMeta missing = find_anim_meta(meta, "nonexistent");
    ASSERT_TRUE(missing.loop == LoopMode::Loop); // default AnimMeta{}

    ASSERT_EQ(meta.hitboxes.size(), 1u);
    ASSERT_TRUE(meta.hitboxes[0].name == "body");
    ASSERT_NEAR(meta.hitboxes[0].min.x, -0.3f, 1e-4f);
    ASSERT_NEAR(meta.hitboxes[0].size.x, 0.6f, 1e-4f);

    ASSERT_EQ(meta.anchors.size(), 1u);
    ASSERT_NEAR(meta.anchors.at("hand_r").x, 0.35f, 1e-4f);
}

static void test_loop_mode_parse() {
    using namespace coopa::pix;
    ASSERT_TRUE(parse_loop_mode("Loop") == LoopMode::Loop);
    ASSERT_TRUE(parse_loop_mode("Once") == LoopMode::Once);
    ASSERT_TRUE(parse_loop_mode("PingPong") == LoopMode::PingPong);
    ASSERT_TRUE(parse_loop_mode("Hold") == LoopMode::Hold);
    ASSERT_TRUE(parse_loop_mode("Bogus") == LoopMode::Loop);
    ASSERT_TRUE(parse_loop_mode("") == LoopMode::Loop);
}

// --- caml.h round trip (proves the CAML container integration) --------------

static void test_caml_round_trip() {
    std::string original_yaml = "format: coopixel\nwidth: 4\nheight: 4\n";
    caml::CAMLMap map = caml::CAMLMap::from_yaml_string(original_yaml);

    std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "pixengine_caml_roundtrip_test.pix";
    map.save_caml(tmp.string());

    caml::CAMLMap loaded = caml::CAMLMap::load_caml(tmp.string());
    std::string loaded_yaml = loaded.to_yaml_string();
    ASSERT_TRUE(loaded_yaml.find("coopixel") != std::string::npos);
    ASSERT_TRUE(loaded_yaml.find("width: 4") != std::string::npos);

    std::filesystem::remove(tmp);
}

// --- transform2d.h: resolve_transforms -------------------------------------

static void test_resolve_transforms_hierarchy_rotation_scale() {
    using namespace coopa::pix;
    coopa::scene::Scene scene("test");
    auto* root = scene.add_root_object(std::make_unique<coopa::scene::SceneObject>("Root"));
    auto* root_t = root->add_component<Transform2D>();
    root_t->position = {10.0f, 0.0f};
    root_t->rotation = 90.0f;
    root_t->scale = {2.0f, 2.0f};

    auto child = std::make_unique<coopa::scene::SceneObject>("Child");
    auto* child_t = child->add_component<Transform2D>();
    child_t->position = {1.0f, 0.0f};
    root->add_child(std::move(child));

    resolve_transforms(scene);

    ASSERT_NEAR(root_t->world_position.x, 10.0f, 1e-4f);
    ASSERT_NEAR(root_t->world_position.y, 0.0f, 1e-4f);
    ASSERT_NEAR(root_t->world_rotation, 90.0f, 1e-4f);

    // Child local (1,0) scaled by parent's (2,2) -> (2,0), rotated 90 deg CCW -> (0,2),
    // translated by parent's world position (10,0) -> (10,2).
    ASSERT_NEAR(child_t->world_position.x, 10.0f, 1e-3f);
    ASSERT_NEAR(child_t->world_position.y, 2.0f, 1e-3f);
    ASSERT_NEAR(child_t->world_rotation, 90.0f, 1e-4f);
    ASSERT_NEAR(child_t->world_scale.x, 2.0f, 1e-4f);
}

static void test_resolve_transforms_group_node_passthrough() {
    using namespace coopa::pix;
    coopa::scene::Scene scene("test");
    auto* root = scene.add_root_object(std::make_unique<coopa::scene::SceneObject>("Root"));
    auto* root_t = root->add_component<Transform2D>();
    root_t->position = {5.0f, 3.0f};

    // "Group" carries no Transform2D -- a transparent pass-through node.
    auto group = std::make_unique<coopa::scene::SceneObject>("Group");
    coopa::scene::SceneObject* group_ptr = root->add_child(std::move(group));

    auto grandchild = std::make_unique<coopa::scene::SceneObject>("Grandchild");
    auto* gc_t = grandchild->add_component<Transform2D>();
    gc_t->position = {1.0f, 1.0f};
    group_ptr->add_child(std::move(grandchild));

    resolve_transforms(scene);

    ASSERT_NEAR(gc_t->world_position.x, 6.0f, 1e-4f);
    ASSERT_NEAR(gc_t->world_position.y, 4.0f, 1e-4f);
}

static void test_resolve_transforms_inactive_subtree_pruned() {
    using namespace coopa::pix;
    coopa::scene::Scene scene("test");
    auto* root = scene.add_root_object(std::make_unique<coopa::scene::SceneObject>("Root"));
    root->add_component<Transform2D>()->position = {1.0f, 1.0f};

    auto inactive_child = std::make_unique<coopa::scene::SceneObject>("Inactive", /*active=*/false);
    auto* inactive_t = inactive_child->add_component<Transform2D>();
    inactive_t->position = {100.0f, 100.0f};
    root->add_child(std::move(inactive_child));

    resolve_transforms(scene);

    // Never visited -- world_position stays at its untouched default, not
    // some stale-but-plausible resolved value.
    ASSERT_NEAR(inactive_t->world_position.x, 0.0f, 1e-6f);
    ASSERT_NEAR(inactive_t->world_position.y, 0.0f, 1e-6f);
}

// --- camera2d.h --------------------------------------------------------------

static void test_camera2d_visible_world_rect_at_zoom() {
    using namespace coopa::pix;
    coopa::scene::Scene scene("test");
    auto* cam_obj = scene.add_root_object(std::make_unique<coopa::scene::SceneObject>("Camera"));
    cam_obj->add_component<Transform2D>();
    auto* cam = cam_obj->add_component<Camera2D>();
    scene.start();
    resolve_transforms(scene);

    cam->zoom = 1.0f;
    auto rect1 = cam->visible_world_rect(16.0f, 480, 270);
    ASSERT_NEAR(rect1.size().x, 30.0f, 1e-3f);
    ASSERT_NEAR(rect1.size().y, 16.875f, 1e-3f);

    cam->zoom = 2.0f;
    auto rect2 = cam->visible_world_rect(16.0f, 480, 270);
    ASSERT_NEAR(rect2.size().x, 15.0f, 1e-3f);
    ASSERT_NEAR(rect2.size().y, 8.4375f, 1e-3f);
}

static void test_camera2d_deadzone() {
    using namespace coopa::pix;
    coopa::scene::Scene scene("test");
    auto* cam_obj = scene.add_root_object(std::make_unique<coopa::scene::SceneObject>("Camera"));
    auto* cam_tf = cam_obj->add_component<Transform2D>();
    auto* cam = cam_obj->add_component<Camera2D>();
    cam->follow_target_name = "Target";
    cam->follow_deadzone = {1.0f, 1.0f};
    cam->follow_lerp = 100.0f;

    auto* target_obj = scene.add_root_object(std::make_unique<coopa::scene::SceneObject>("Target"));
    auto* target_tf = target_obj->add_component<Transform2D>();
    target_tf->position = {0.5f, 0.5f}; // within the deadzone

    scene.start();
    resolve_transforms(scene);

    cam->update(1.0f / 60.0f);
    ASSERT_NEAR(cam_tf->position.x, 0.0f, 1e-4f);
    ASSERT_NEAR(cam_tf->position.y, 0.0f, 1e-4f);

    target_tf->position = {10.0f, 0.0f}; // well outside the deadzone
    resolve_transforms(scene);
    for (int i = 0; i < 200; ++i) cam->update(1.0f / 60.0f);
    ASSERT_NEAR(cam_tf->position.x, 9.0f, 0.05f); // converges to target.x - deadzone.x
    ASSERT_NEAR(cam_tf->position.y, 0.0f, 1e-3f);
}

static void test_camera2d_bounds_clamp() {
    using namespace coopa::pix;
    coopa::scene::Scene scene("test");
    auto* cam_obj = scene.add_root_object(std::make_unique<coopa::scene::SceneObject>("Camera"));
    auto* cam_tf = cam_obj->add_component<Transform2D>();
    auto* cam = cam_obj->add_component<Camera2D>();
    cam->clamp_to_bounds = true;
    cam->world_bounds = coopa::ui::Rect{{-5.0f, -5.0f}, {5.0f, 5.0f}};
    scene.start();

    cam_tf->position = {100.0f, -100.0f};
    cam->update(1.0f / 60.0f);
    ASSERT_NEAR(cam_tf->position.x, 5.0f, 1e-4f);
    ASSERT_NEAR(cam_tf->position.y, -5.0f, 1e-4f);
}

// --- sprite_draw_list.h -------------------------------------------------------

static void test_sprite_sort_key_layer_dominates() {
    using namespace coopa::pix;
    VkImageView tex = reinterpret_cast<VkImageView>(0x1000);
    uint64_t k_low_layer = pack_sprite_sort_key(-100, false, 0.0f, 0, tex);
    uint64_t k_high_layer = pack_sprite_sort_key(100, false, 0.0f, 0, tex);
    ASSERT_TRUE(k_low_layer < k_high_layer);

    // Even an extreme depth difference must never outrank a layer difference.
    uint64_t k_layer0_extreme_y = pack_sprite_sort_key(0, true, -4096.0f, 0, tex);
    uint64_t k_layer1_extreme_y = pack_sprite_sort_key(1, true, 4096.0f, 0, tex);
    ASSERT_TRUE(k_layer0_extreme_y < k_layer1_extreme_y);
}

static void test_sprite_sort_key_y_sort_depth_ordering() {
    using namespace coopa::pix;
    VkImageView tex = reinterpret_cast<VkImageView>(0x1000);
    // Larger world_y sorts FIRST (drawn further back) -- standard top-down y-sort.
    uint64_t k_high_y = pack_sprite_sort_key(0, true, 10.0f, 0, tex);
    uint64_t k_low_y  = pack_sprite_sort_key(0, true, -10.0f, 0, tex);
    ASSERT_TRUE(k_high_y < k_low_y);
}

static void test_sprite_sort_key_texture_confined_to_low_bits() {
    using namespace coopa::pix;
    VkImageView tex_a = reinterpret_cast<VkImageView>(0x1000);
    VkImageView tex_b = reinterpret_cast<VkImageView>(0x2000);
    uint64_t ka = pack_sprite_sort_key(0, false, 0.0f, 5, tex_a);
    uint64_t kb = pack_sprite_sort_key(0, false, 0.0f, 5, tex_b);
    // texture_bucket occupies bits [8,24) and sub_order bits [0,8) -- clear
    // the low 24 bits, leaving only layer (bits 48+) and depth (bits 24-47).
    uint64_t layer_depth_mask = ~static_cast<uint64_t>(0xFFFFFFu);
    ASSERT_EQ(ka & layer_depth_mask, kb & layer_depth_mask);
}

static void test_sprite_draw_list_batch_coalescing() {
    using namespace coopa::pix;
    SpriteDrawList list;
    VkImageView tex_a = reinterpret_cast<VkImageView>(0x1000);
    VkImageView tex_b = reinterpret_cast<VkImageView>(0x2000);
    coopa::ui::Rect uv{{0.0f, 0.0f}, {1.0f, 1.0f}};

    list.begin();
    // All four quads share layer/depth -- only texture differs -- so after
    // sorting, same-texture entries end up adjacent regardless of
    // interleaved insertion order, coalescing into exactly 2 batches.
    list.add_sprite(tex_a, {0, 0}, {1, 1}, {0.5f, 0.5f}, 0.0f, {1, 1}, uv, 0xFFFFFFFFu, 0, false, 0.0f, 0);
    list.add_sprite(tex_b, {1, 0}, {1, 1}, {0.5f, 0.5f}, 0.0f, {1, 1}, uv, 0xFFFFFFFFu, 0, false, 0.0f, 0);
    list.add_sprite(tex_a, {2, 0}, {1, 1}, {0.5f, 0.5f}, 0.0f, {1, 1}, uv, 0xFFFFFFFFu, 0, false, 0.0f, 0);
    list.add_sprite(tex_b, {3, 0}, {1, 1}, {0.5f, 0.5f}, 0.0f, {1, 1}, uv, 0xFFFFFFFFu, 0, false, 0.0f, 0);
    list.sort_and_flatten();

    ASSERT_EQ(list.batches().size(), 2u);
    uint32_t total_indices = 0;
    for (const auto& b : list.batches()) total_indices += b.index_count;
    ASSERT_EQ(total_indices, 24u); // 4 quads * 6 indices
}

static void test_sprite_draw_list_quad_corners() {
    using namespace coopa::pix;
    SpriteDrawList list;
    coopa::ui::Rect uv{{0.0f, 0.0f}, {1.0f, 1.0f}};

    // No rotation, no flip, pivot at bottom-left (0,0).
    list.begin();
    list.add_sprite(reinterpret_cast<VkImageView>(0x1), glm::vec2(10.0f, 20.0f), glm::vec2(4.0f, 2.0f),
                    glm::vec2(0.0f, 0.0f), 0.0f, glm::vec2(1.0f, 1.0f), uv, 0xFFFFFFFFu);
    list.sort_and_flatten();
    ASSERT_EQ(list.vertices().size(), 4u);
    ASSERT_NEAR(list.vertices()[0].x, 10.0f, 1e-4f); ASSERT_NEAR(list.vertices()[0].y, 20.0f, 1e-4f);
    ASSERT_NEAR(list.vertices()[1].x, 14.0f, 1e-4f); ASSERT_NEAR(list.vertices()[1].y, 20.0f, 1e-4f);
    ASSERT_NEAR(list.vertices()[2].x, 14.0f, 1e-4f); ASSERT_NEAR(list.vertices()[2].y, 22.0f, 1e-4f);
    ASSERT_NEAR(list.vertices()[3].x, 10.0f, 1e-4f); ASSERT_NEAR(list.vertices()[3].y, 22.0f, 1e-4f);

    // 180-degree rotation about a centered pivot mirrors every corner through it.
    list.begin();
    list.add_sprite(reinterpret_cast<VkImageView>(0x1), glm::vec2(0.0f, 0.0f), glm::vec2(2.0f, 2.0f),
                    glm::vec2(0.5f, 0.5f), 180.0f, glm::vec2(1.0f, 1.0f), uv, 0xFFFFFFFFu);
    list.sort_and_flatten();
    ASSERT_NEAR(list.vertices()[0].x, 1.0f, 1e-3f);
    ASSERT_NEAR(list.vertices()[0].y, 1.0f, 1e-3f);

    // Horizontal flip only, pivot at bottom-left: local x negates before translation.
    list.begin();
    list.add_sprite(reinterpret_cast<VkImageView>(0x1), glm::vec2(0.0f, 0.0f), glm::vec2(4.0f, 2.0f),
                    glm::vec2(0.0f, 0.0f), 0.0f, glm::vec2(-1.0f, 1.0f), uv, 0xFFFFFFFFu);
    list.sort_and_flatten();
    ASSERT_NEAR(list.vertices()[0].x, 0.0f, 1e-4f);
    ASSERT_NEAR(list.vertices()[1].x, -4.0f, 1e-4f);
}

// --- pixel_math.h: tile_rect_from_world_rect ----------------------------------

static void test_tile_rect_full_overlap() {
    using namespace coopa::pix;
    // layer_origin=(0,0): tile(0,0)'s top-left sits at world (0,0); layer
    // spans world X:[0,10], Y:[-10,0] (row increases downward in world Y).
    coopa::ui::Rect world{{0.0f, -10.0f}, {10.0f, 0.0f}};
    TileRange r = tile_rect_from_world_rect(world, glm::vec2(1.0f, 1.0f), glm::ivec2(10, 10), glm::vec2(0.0f, 0.0f));
    ASSERT_TRUE(!r.empty());
    ASSERT_EQ(r.x0, 0); ASSERT_EQ(r.x1, 9);
    ASSERT_EQ(r.y0, 0); ASSERT_EQ(r.y1, 9);
}

static void test_tile_rect_no_overlap_both_directions() {
    using namespace coopa::pix;
    glm::ivec2 size(10, 10);
    glm::vec2 tsz(1.0f, 1.0f);
    glm::vec2 origin(0.0f, 0.0f);

    // Below-left of the layer (very negative x, very negative y).
    coopa::ui::Rect below_left{{-100.0f, -100.0f}, {-50.0f, -50.0f}};
    ASSERT_TRUE(tile_rect_from_world_rect(below_left, tsz, size, origin).empty());

    // Above-right of the layer (very positive x, very positive y).
    coopa::ui::Rect above_right{{50.0f, 50.0f}, {100.0f, 100.0f}};
    ASSERT_TRUE(tile_rect_from_world_rect(above_right, tsz, size, origin).empty());
}

static void test_tile_rect_partial_overlap_and_y_inversion() {
    using namespace coopa::pix;
    glm::ivec2 size(10, 10);
    glm::vec2 tsz(1.0f, 1.0f);
    glm::vec2 origin(0.0f, 0.0f);

    // Extends past the layer's left edge (x<0) and past its top (world Y >
    // layer_origin.y=0, "above" the layer) -- both clamp to the layer's
    // row/col 0, proving the Y inversion: a LARGER world Y clamps to the
    // SMALLEST row index, not the largest.
    coopa::ui::Rect world{{-5.0f, -3.0f}, {2.0f, 5.0f}};
    TileRange r = tile_rect_from_world_rect(world, tsz, size, origin);
    ASSERT_TRUE(!r.empty());
    ASSERT_EQ(r.x0, 0);
    ASSERT_EQ(r.x1, 2);
    ASSERT_EQ(r.y0, 0); // clamped from the "above the layer" side
    ASSERT_EQ(r.y1, 3); // world_rect.min.y=-3 -> row 3
}

static void test_tile_rect_degenerate_inputs_are_safe() {
    using namespace coopa::pix;
    coopa::ui::Rect world{{0.0f, 0.0f}, {1.0f, 1.0f}};
    ASSERT_TRUE(tile_rect_from_world_rect(world, glm::vec2(0.0f, 1.0f), glm::ivec2(10, 10), glm::vec2(0.0f)).empty());
    ASSERT_TRUE(tile_rect_from_world_rect(world, glm::vec2(1.0f, 1.0f), glm::ivec2(0, 10), glm::vec2(0.0f)).empty());
}

// --- residency.h: decide_residency --------------------------------------------

static void test_decide_residency_acquire_budget_and_distance_priority() {
    using namespace coopa::pix;
    std::vector<ResidencyItem> items(6);
    // Distances deliberately out of order, so a correct implementation must sort.
    float distances[6] = {50.0f, 5.0f, 30.0f, 1.0f, 20.0f, 10.0f};
    for (size_t i = 0; i < 6; ++i) {
        items[i].wants_visible = true;
        items[i].has_handle = false;
        items[i].is_loading = false;
        items[i].distance_to_camera = distances[i];
    }
    auto actions = decide_residency(items, /*max_acquires_this_frame=*/3, /*release_delay_frames=*/30);
    ASSERT_EQ(actions.size(), 6u);

    // Only the 3 closest (indices 3,1,5 -> distances 1,5,10) get Acquire.
    ASSERT_TRUE(actions[3] == ResidencyAction::Acquire);
    ASSERT_TRUE(actions[1] == ResidencyAction::Acquire);
    ASSERT_TRUE(actions[5] == ResidencyAction::Acquire);
    ASSERT_TRUE(actions[0] == ResidencyAction::None);
    ASSERT_TRUE(actions[2] == ResidencyAction::None);
    ASSERT_TRUE(actions[4] == ResidencyAction::None);

    uint32_t acquire_count = 0;
    for (auto a : actions) if (a == ResidencyAction::Acquire) ++acquire_count;
    ASSERT_EQ(acquire_count, 3u);
}

static void test_decide_residency_release_hysteresis() {
    using namespace coopa::pix;
    ResidencyItem below;
    below.wants_visible = false; below.has_handle = true; below.is_loading = false;
    below.out_of_range_frames = 29;

    ResidencyItem at_threshold = below;
    at_threshold.out_of_range_frames = 30;

    std::vector<ResidencyItem> items = {below, at_threshold};
    auto actions = decide_residency(items, 4, 30);
    ASSERT_TRUE(actions[0] == ResidencyAction::None);    // not yet past the hysteresis window
    ASSERT_TRUE(actions[1] == ResidencyAction::Release); // exactly at the threshold releases
}

static void test_decide_residency_visible_never_releases_regardless_of_stale_counter() {
    using namespace coopa::pix;
    // wants_visible=true with a large out_of_range_frames simulates the
    // counter not yet having been reset by the caller this exact frame --
    // decide_residency's own contract must still never release something
    // the caller says is currently visible (no acquire/release thrash at
    // the boundary is enforced by the CALLER resetting the counter on
    // re-entry; this asserts the pure function's independent invariant).
    ResidencyItem item;
    item.wants_visible = true;
    item.has_handle = true;
    item.is_loading = false;
    item.out_of_range_frames = 1000;

    std::vector<ResidencyItem> items = {item};
    auto actions = decide_residency(items, 4, 30);
    ASSERT_TRUE(actions[0] != ResidencyAction::Release);
}

static void test_decide_residency_loading_items_never_reacquired() {
    using namespace coopa::pix;
    ResidencyItem loading;
    loading.wants_visible = true;
    loading.has_handle = true;
    loading.is_loading = true;
    loading.distance_to_camera = 0.0f; // closest possible -- would win the budget if considered

    std::vector<ResidencyItem> items = {loading};
    auto actions = decide_residency(items, 4, 30);
    ASSERT_TRUE(actions[0] == ResidencyAction::None);
}

// --- input_map.h ---------------------------------------------------------

static void test_input_map_action_down_with_any_bound_key() {
    using namespace coopa::pix;
    InputMap map;
    map.bind_key("jump", 32);  // e.g. GLFW_KEY_SPACE
    map.bind_key("jump", 87);  // e.g. GLFW_KEY_W

    auto only_87_down = [](int k) { return k == 87; };
    ASSERT_TRUE(map.is_action_down("jump", only_87_down));

    auto nothing_down = [](int) { return false; };
    ASSERT_TRUE(!map.is_action_down("jump", nothing_down));
}

static void test_input_map_unbound_action_never_down() {
    using namespace coopa::pix;
    InputMap map;
    auto always_true = [](int) { return true; };
    ASSERT_TRUE(!map.is_action_down("nonexistent", always_true));
    ASSERT_TRUE(map.bindings("nonexistent").empty());
}

static void test_input_map_unbind_clears_bindings() {
    using namespace coopa::pix;
    InputMap map;
    map.bind_key("fire", 1);
    ASSERT_EQ(map.bindings("fire").size(), 1u);
    map.unbind("fire");
    ASSERT_TRUE(map.bindings("fire").empty());
    auto always_true = [](int) { return true; };
    ASSERT_TRUE(!map.is_action_down("fire", always_true));
}

static void test_sprite_draw_list_uv_orientation_asymmetric() {
    using namespace coopa::pix;
    SpriteDrawList list;
    // Asymmetric UV rect: min=(0.1,0.2) is the TOP of the source sprite (.pix
    // convention), max=(0.9,0.8) is the bottom.
    coopa::ui::Rect uv{{0.1f, 0.2f}, {0.9f, 0.8f}};
    list.begin();
    list.add_sprite(reinterpret_cast<VkImageView>(0x1), glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f),
                    glm::vec2(0.0f, 0.0f), 0.0f, glm::vec2(1.0f, 1.0f), uv, 0xFFFFFFFFu);
    list.sort_and_flatten();

    // Slot 0 (bottom-left, world -Y) must sample uv.max.y -- the source's bottom.
    ASSERT_NEAR(list.vertices()[0].v, 0.8f, 1e-4f);
    ASSERT_NEAR(list.vertices()[0].u, 0.1f, 1e-4f);
    // Slot 2 (top-right, world +Y) must sample uv.min.y -- the source's top.
    ASSERT_NEAR(list.vertices()[2].v, 0.2f, 1e-4f);
    ASSERT_NEAR(list.vertices()[2].u, 0.9f, 1e-4f);
}

// --- tilemap_decoder.h / tilemap_asset.h --------------------------------------

static void test_tilemap_rle_expansion() {
    using namespace coopa::pix;
    std::vector<std::string> tokens = {"3:5", "1:9", "2:0"};
    auto gids = expand_tile_rle(tokens);
    std::vector<uint16_t> expected = {5, 5, 5, 9, 0, 0};
    ASSERT_EQ(gids.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) ASSERT_EQ(gids[i], expected[i]);
}

static void test_tilemap_rle_malformed_throws() {
    using namespace coopa::pix;
    auto expect_throw = [](const std::vector<std::string>& tokens) {
        bool threw = false;
        try { expand_tile_rle(tokens); } catch (const std::invalid_argument&) { threw = true; }
        ASSERT_TRUE(threw);
    };
    expect_throw({"3-5"});      // missing ':'
    expect_throw({"abc:5"});    // non-numeric count
    expect_throw({"3:xyz"});    // non-numeric gid
    expect_throw({"-1:5"});     // negative count
}

static void test_tilemap_decode_layers_and_tileset() {
    using namespace coopa::pix;
    std::string yaml = R"YAML(
format: pixengine_tilemap
version: "1.0"
tile_size: { x: 16, y: 16 }
pixels_per_unit: 16
tileset:
  source: ../tilesets/farm_tiles.pix
  columns: 4
  first_gid: 1
layers:
  - name: ground
    sort_layer: -100
    y_sort: false
    width: 2
    height: 2
    encoding: rle
    data: ["4:1"]
  - name: buildings
    sort_layer: 0
    y_sort: true
    y_sort_offset: 0.5
    width: 2
    height: 2
    encoding: flat
    data: [0, 5, 0, 6]
collision:
  - layer: buildings
    solid_gids: [5, 6]
)YAML";
    fkyaml::node root = fkyaml::node::deserialize(yaml);
    DecodedTilemap doc = decode_tilemap_document(root);

    ASSERT_NEAR(doc.tile_size_px.x, 16.0f, 1e-4f);
    ASSERT_NEAR(doc.pixels_per_unit, 16.0f, 1e-4f);
    ASSERT_TRUE(doc.tileset_source == "../tilesets/farm_tiles.pix");
    ASSERT_EQ(doc.tileset_columns, 4u);
    ASSERT_EQ(doc.tileset_first_gid, 1u);

    ASSERT_EQ(doc.layers.size(), 2u);
    ASSERT_TRUE(doc.layers[0].name == "ground");
    ASSERT_EQ(doc.layers[0].sort_layer, -100);
    ASSERT_TRUE(!doc.layers[0].y_sort);
    ASSERT_EQ(doc.layers[0].gids.size(), 4u);
    for (auto g : doc.layers[0].gids) ASSERT_EQ(g, 1);

    ASSERT_TRUE(doc.layers[1].name == "buildings");
    ASSERT_TRUE(doc.layers[1].y_sort);
    ASSERT_NEAR(doc.layers[1].y_sort_offset, 0.5f, 1e-4f);
    ASSERT_EQ(doc.layers[1].at(0, 0), 0);
    ASSERT_EQ(doc.layers[1].at(1, 0), 5);
    ASSERT_EQ(doc.layers[1].at(1, 1), 6);

    ASSERT_EQ(doc.collision.size(), 1u);
    ASSERT_TRUE(doc.collision[0].layer == "buildings");
    ASSERT_EQ(doc.collision[0].solid_gids.size(), 2u);
}

static void test_tilemap_decode_gid_count_mismatch_throws() {
    using namespace coopa::pix;
    std::string yaml = R"YAML(
layers:
  - name: bad
    width: 3
    height: 3
    encoding: flat
    data: [1, 2, 3]
)YAML";
    fkyaml::node root = fkyaml::node::deserialize(yaml);
    bool threw = false;
    try {
        decode_tilemap_document(root);
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE(threw); // 3 gids given, 3*3=9 expected
}

static void test_gid_to_tile_coord() {
    using namespace coopa::pix;
    // TilemapAsset itself can't be constructed headlessly (owns a GPU
    // Texture), but the grid math it delegates to is a free function and
    // fully testable directly.
    ASSERT_EQ(gid_to_tile_coord(1, 1, 4).x, 0u); // first tile: top-left
    ASSERT_EQ(gid_to_tile_coord(1, 1, 4).y, 0u);
    ASSERT_EQ(gid_to_tile_coord(6, 1, 4).x, 1u); // index=5 -> 5%4=1, 5/4=1
    ASSERT_EQ(gid_to_tile_coord(6, 1, 4).y, 1u);
    ASSERT_EQ(gid_to_tile_coord(0, 1, 4).x, UINT32_MAX); // below first_gid -> sentinel
    ASSERT_EQ(gid_to_tile_coord(5, 1, 0).x, UINT32_MAX); // columns == 0 -> sentinel
}

int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "          Running pixengine Test Suite      " << std::endl;
    std::cout << "===========================================" << std::endl;

    RUN_TEST(test_harness_smoke);

    RUN_TEST(test_fit_viewport_exact_multiples);
    RUN_TEST(test_fit_viewport_non_exact_centers_bars);
    RUN_TEST(test_fit_viewport_window_smaller_than_reference_scales_down);
    RUN_TEST(test_fit_viewport_extreme_aspect_ratio);
    RUN_TEST(test_fit_viewport_always_preserves_aspect_ratio);
    RUN_TEST(test_fit_viewport_zero_dimension_inputs_are_safe);

    RUN_TEST(test_color_pack_byte_order_matches_ui_vertex);
    RUN_TEST(test_parse_hex_color_upper_and_lower);
    RUN_TEST(test_parse_hex_color_malformed_throws);
    RUN_TEST(test_source_over_blending);
    RUN_TEST(test_premultiply);

    RUN_TEST(test_pix_decode_shape_animations);
    RUN_TEST(test_pix_decode_shape_legacy_frames);
    RUN_TEST(test_pix_decode_shape_legacy_layers);
    RUN_TEST(test_pix_decode_malformed_pixel_key_throws);
    RUN_TEST(test_pix_decode_missing_fields_use_defaults);

    RUN_TEST(test_composite_layer_order_later_layer_on_top);
    RUN_TEST(test_composite_invisible_and_zero_opacity_layers_skipped);
    RUN_TEST(test_composite_fractional_opacity_math);
    RUN_TEST(test_composite_stroke_outside);
    RUN_TEST(test_composite_stroke_inside_and_above_ordering);
    RUN_TEST(test_composite_stroke_center_positions_both_sides);
    RUN_TEST(test_trim_opaque_normal_and_all_transparent);

    RUN_TEST(test_atlas_packer_no_overlap_and_padding_respected);
    RUN_TEST(test_atlas_packer_deterministic);
    RUN_TEST(test_atlas_packer_power_of_two_growth);
    RUN_TEST(test_atlas_packer_over_cap_throws);
    RUN_TEST(test_atlas_packer_single_and_zero_rect_edge_cases);

    RUN_TEST(test_sprite_meta_defaults_when_minimal);
    RUN_TEST(test_sprite_meta_overrides_and_unknown_keys_ignored);
    RUN_TEST(test_loop_mode_parse);

    RUN_TEST(test_caml_round_trip);

    RUN_TEST(test_resolve_transforms_hierarchy_rotation_scale);
    RUN_TEST(test_resolve_transforms_group_node_passthrough);
    RUN_TEST(test_resolve_transforms_inactive_subtree_pruned);

    RUN_TEST(test_camera2d_visible_world_rect_at_zoom);
    RUN_TEST(test_camera2d_deadzone);
    RUN_TEST(test_camera2d_bounds_clamp);

    RUN_TEST(test_sprite_sort_key_layer_dominates);
    RUN_TEST(test_sprite_sort_key_y_sort_depth_ordering);
    RUN_TEST(test_sprite_sort_key_texture_confined_to_low_bits);
    RUN_TEST(test_sprite_draw_list_batch_coalescing);
    RUN_TEST(test_sprite_draw_list_quad_corners);
    RUN_TEST(test_sprite_draw_list_uv_orientation_asymmetric);

    RUN_TEST(test_tile_rect_full_overlap);
    RUN_TEST(test_tile_rect_no_overlap_both_directions);
    RUN_TEST(test_tile_rect_partial_overlap_and_y_inversion);
    RUN_TEST(test_tile_rect_degenerate_inputs_are_safe);

    RUN_TEST(test_tilemap_rle_expansion);
    RUN_TEST(test_tilemap_rle_malformed_throws);
    RUN_TEST(test_tilemap_decode_layers_and_tileset);
    RUN_TEST(test_tilemap_decode_gid_count_mismatch_throws);
    RUN_TEST(test_gid_to_tile_coord);

    RUN_TEST(test_decide_residency_acquire_budget_and_distance_priority);
    RUN_TEST(test_decide_residency_release_hysteresis);
    RUN_TEST(test_decide_residency_visible_never_releases_regardless_of_stale_counter);
    RUN_TEST(test_decide_residency_loading_items_never_reacquired);

    RUN_TEST(test_input_map_action_down_with_any_bound_key);
    RUN_TEST(test_input_map_unbound_action_never_down);
    RUN_TEST(test_input_map_unbind_clears_bindings);

    std::cout << "===========================================" << std::endl;
    std::cout << "Tests run: " << g_tests_run << ", Failed: " << g_tests_failed << std::endl;
    std::cout << "===========================================" << std::endl;

    return g_tests_failed == 0 ? 0 : 1;
}
