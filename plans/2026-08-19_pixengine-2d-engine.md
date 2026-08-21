# pixengine — A 2D Pixel-Art Game Engine Framework on Vulkan

## Context

`/home/coopa/git/pixengine` is an empty repo. The goal is a **2D-only** game engine framework
targeting a Stardew Valley-like game, built on the existing coopa stack rather than starting
from scratch:

- **`libcoopa`** — scene graph (`coopa::scene`), asset system (`coopa::asset`), jobs, events.
- **`gfxcoopa`** — Vulkan RAII layer. Edits allowed but **additive only**; its 3D renderer must
  keep working.
- **`uicoopa`** — UI. Its `DrawList`/`UiPass` quad batcher is the reference implementation for
  our sprite renderer.
- **`caml`** — the container `.pix` files are written in (zstd + AES-256-GCM around a YAML doc).
- **`coopixel`** — the Python editor that authors `.pix` sprites.

Assets are `.pix` sprites, TTF fonts, and `.yaml` for config/scenes.

**Scope: engine core only.** The deliverable ends at *"an animated sprite walks around a tilemap
with a UI overlay, streaming its art in and out of GPU memory."* No inventory, crops, NPCs,
dialogue, save system, pathfinding, or game clock.

### Locked design decisions

1. **Pixel model** — world renders into a fixed-resolution `OffscreenTarget` (480×270, nearest),
   then an **integer-scale + letterbox blit** to the swapchain. UI draws at **native** resolution
   on top so text stays crisp.
2. **`.pix` pipeline** — decode + composite + atlas-pack on the `AssetManager` worker thread;
   GPU upload on the main thread. **Usage-driven residency**: assets stream in when a sprite
   comes into range and are freed from GPU memory when it leaves. The main thread never parses
   sprite data.
3. **Sprite metadata** — a YAML **sidecar** (`player.pix` + `player.sprite.yaml`) carries pivot,
   PPU, loop mode, frame events, hitboxes, anchors. Zero coopixel changes.
4. **Transform** — a new `Transform2D` component, scenes use `auto_transform: false`
   (the `uicoopa::RectTransform` precedent). Rationale in Phase 5.

### Three verified constraints that shape everything

Confirmed by reading the source, not assumed:

1. **`Renderer::begin_frame()` opens the swapchain render pass immediately after `cmd.begin()`**
   (`gfxcoopa/presentation/renderer.h:160-163`). There is no hook to record *before* it. blendy
   works around this with a private command buffer + `vkQueueWaitIdle` every frame — a full
   device stall.
2. **`RenderPass` has only an *entry* subpass dependency** (`EXTERNAL → 0`,
   `pipeline/render_pass.h:129-139`). There is no *exit* dependency, so the automatic transition
   to `SHADER_READ_ONLY_OPTIMAL` is unsynchronized against a later sampled read. blendy's
   `vkQueueWaitIdle` masks this today; recording offscreen + swapchain into one command buffer
   exposes it as a real hazard.
3. **`RenderPass` always uses `VK_ATTACHMENT_LOAD_OP_CLEAR` on color.** You can never open a
   second pass over the swapchain image without erasing it — everything composites inside the
   one pass `Renderer` opens.

---

## Repo Layout

Namespace: **`coopa::pix`**. Header-only, matching every sibling.

```
/home/coopa/git/pixengine/
├── CMakeLists.txt              project(pixengine_demo)  ← cplay runs the first project()
├── configuration/root_directory.h.in    copied verbatim from blendy
├── demo.cpp                    windowed demo  → target `pixengine_demo`
├── test.cpp                    headless suite → target `pixengine`, registered with ctest
├── assets/
│   ├── shaders/                sprite.vert/.frag, upscale.vert/.frag (+ committed .spv)
│   ├── sprites/                player.pix + player.sprite.yaml, tree.pix, shadow.pix
│   ├── tilesets/               farm_tiles.pix
│   ├── tilemaps/               farm.tilemap.yaml
│   ├── fonts/                  DejaVuSans.ttf
│   └── scenes/{farm,hud}/scene.yaml
└── pixengine/
    ├── pix/          ── PURE CPU, ZERO VULKAN, 100% headless-testable
    │   ├── color.h              "#RRGGBBAA" parse, RGBA8 pack, source-over, premultiply
    │   ├── pix_document.h       PixDocument / PixAnimation / PixFrame / PixLayer / PixEffect
    │   ├── pix_decoder.h        fkyaml::node → PixDocument
    │   ├── pix_composite.h      layer compositing, opacity, stroke effect, trim
    │   ├── atlas_packer.h       shelf packer
    │   └── sprite_meta.h        sidecar schema + parse + merge
    ├── math/pixel_math.h        compute_letterbox, snap_to_pixel, tile_rect_from_world_rect
    ├── data/{sprite_asset,tilemap_asset}.h
    ├── loaders/{pix_loader,tilemap_loader}.h
    ├── render/
    │   ├── sprite_vertex.h      layout-identical to ui::UiVertex
    │   ├── sprite_draw_list.h   world-space quads, sort keys, batching
    │   ├── sprite_pass.h        pipeline, per-frame ring buffers, descriptor cache
    │   ├── upscale_pass.h       integer-scale letterbox blit
    │   ├── chunk_cache.h        (Phase 8) baked static tilemap chunks
    │   └── pixel_renderer.h     owns target + passes, drives the frame
    ├── scene/
    │   ├── transform2d.h        Transform2D + resolve_transforms(scene)
    │   ├── {sprite_renderer,sprite_animator,camera2d,tilemap_renderer}.h
    │   ├── residency.h          decide_residency() (pure) + SpriteResidencySystem
    │   ├── scene_view.h         cached component gather
    │   └── register.h           register_pix_components(device, allocator, cmd_pool, assets)
    └── input/input_map.h        named actions over Window keys/buttons/gamepad
```

**Invariant worth enforcing with a grep:** `pixengine/pix/` and `pixengine/math/` must never
include `<volk/volk.h>`. That single property is what makes the test suite headless.

### CMakeLists.txt

Copy `/home/coopa/git/blendy/CMakeLists.txt` — it is the only sibling carrying the zstd +
OpenSSL blocks that `caml` needs. Changes:

- `project(pixengine_demo)`.
- Include `${CMAKE_SOURCE_DIR}/` plus the four siblings' roots and their `includes/` dirs.
  Put `${GFXCOOPA_DIR}/includes/` before `${UICOOPA_DIR}/includes/` — both vendor
  `stb/stb_image.h` and first-match wins (same library, harmless, but comment it).
- `add_definitions(-DVOLK_IMPLEMENTATION -DVMA_IMPLEMENTATION -DGLM_FORCE_DEPTH_ZERO_TO_ONE
  -DSTB_IMAGE_IMPLEMENTATION -DSTB_TRUETYPE_IMPLEMENTATION -DSTB_IMAGE_WRITE_IMPLEMENTATION)`.
- Two targets, both linking `Vulkan::Vulkan glfw dl ${ZSTD_TARGET} OpenSSL::Crypto`:
  ```cmake
  add_executable(pixengine_demo demo.cpp)
  add_executable(pixengine     test.cpp)
  enable_testing()
  add_test(NAME pixengine_tests COMMAND pixengine)
  ```

---

## gfxcoopa Changes (all additive unless flagged)

| # | Change | File | Notes |
|---|---|---|---|
| **G1** | `begin_frame(record_fn, clear_color, on_resize, **pre_pass_fn = nullptr**)` — invoked between `cmd.begin()` and `cmd.begin_render_pass()`. | `presentation/renderer.h` | Trailing defaulted param; blendy (the only call site) is unaffected. |
| **G2** | Add an **exit** subpass dependency (`0 → EXTERNAL`, src `COLOR_ATTACHMENT_OUTPUT\|LATE_FRAGMENT_TESTS` / `*_WRITE`, dst `FRAGMENT_SHADER` / `SHADER_READ`) when either final layout is `SHADER_READ_ONLY_OPTIMAL`. | `pipeline/render_pass.h` | No signature change; strictly strengthens sync. **This is a latent correctness bug today**, not just a pixengine need. |
| **G3** | `OffscreenTarget::begin(cmd, **clear = {{0.05,0.05,0.05,1}}**)`. | `engine/targets/offscreen_target.h` | Defaulted param preserving the current literal. |
| **G4** | `Texture::upload(..., srgb, **VkFilter = LINEAR, VkSamplerAddressMode = REPEAT**)`. | `engine/data/texture.h` | **Required** — the hard-coded `VK_FILTER_LINEAR` blurs every pixel-art atlas. |
| **G5** | `TextureLoader(..., **VkFilter = LINEAR**)` forwarded to G4. | `engine/loaders/texture_loader.h` | So `.png` tilesets are nearest-filtered without forking the loader. |
| **G6** | `PipelineConfig`: append `enum class BlendMode { None, Alpha, PremultipliedAlpha, Additive, Multiply }` + `BlendMode blend_mode = None`. Resolve as `effective = (blend_mode != None) ? blend_mode : (blending ? Alpha : None)`. | `pipeline/pipeline.h` | Appended members; all call sites use member assignment, never positional aggregate init. Existing configs stay byte-identical. |
| **G7** | `Window`: `glfwSetMouseButtonCallback` + `glfwSetCursorPosCallback`; add `mouse_button_events()`, `cursor_delta()`, cleared in `new_frame()`. | `presentation/window.h` | Pure addition. |
| **G8** | `Window`: gamepad via `glfwGetGamepadState`, snapshotted in `new_frame()`. | `presentation/window.h` | Pure addition. |
| **G9** | `Window`: make `framebuffer_resize_callback` actually store `width_`/`height_` (it currently does `(void)width;`). Add `wait_while_minimized()`. | `presentation/window.h` | **⚠ The one non-purely-additive change.** `width()`/`height()` currently return the *constructed* size forever. Observationally inert today (no resizable windows exist) — but grep `.width()`/`.height()` on `Window` across all siblings before landing. |

**Explicitly not doing:** adding `LOAD_OP_LOAD` to `RenderPass`. It would tempt callers into a
second pass over the swapchain image and break the single-pass compositing discipline that
uicoopa and blendy both depend on. Design around it instead.

**De-risking G1/G2:** Phases 2–7 ship using blendy's existing pattern — a private
`pre_frame_cmd_` + `vkQueueSubmit` + `vkQueueWaitIdle` — which needs **zero** gfxcoopa changes
and is correct (the wait-idle hides the missing G2 dependency). Encapsulate it inside
`PixelRenderer::render()` so Phase 8's switch to `pre_pass_fn` is a ~10-line internal edit.
pixengine is never blocked on a gfxcoopa change.

---

## Implementation Phases

### Phase 1 — Skeleton + clear color

`CMakeLists.txt`, `configuration/root_directory.h.in`, the empty `pixengine/` tree, `test.cpp`
with uicoopa's `RUN_TEST`/`ASSERT_TRUE`/`ASSERT_EQ` harness plus one trivial test, and
`demo.cpp` opening a window that clears to a solid color via `Renderer::begin_frame` with an
empty record lambda.

**Runnable:** `cbuild --vulkan && cplay` → a colored window. `ctest` passes.

### Phase 2 — Low-res target + integer-scale letterbox blit

- `math/pixel_math.h`: `compute_letterbox(sw, sh, vw, vh) -> {x, y, w, h, scale}` with
  `scale = max(1, min(sw/vw, sh/vh))` (integer floor, never 0), centered.
- `render/upscale_pass.h`: a near-copy of `gfx::engine::passes::PresentPass` with two
  differences — `Sampler::nearest`, and `draw()` takes a `VkRect2D dst` for viewport+scissor.
- `render/pixel_renderer.h` owning an `OffscreenTarget(480, 270)`.
- Ship the blendy-style `pre_frame_cmd_` + `vkQueueWaitIdle` fallback.

**The letterbox bars are the swapchain's clear color.** Rather than computing bars in a shader,
set viewport *and scissor* to the centered destination rect and draw the fullscreen triangle
there — `LOAD_OP_CLEAR` paints the bars for free. The constraint becomes a feature.

`OffscreenTarget::begin()` sets a **negative-height (Y-flipped) viewport** for 3D. Viewport is
dynamic state, so just re-issue `cmd.set_viewport(0, 0, VW, VH)` after `begin()`. No change needed.

**Choose 480×270** — exactly 16:9, and upscales to 1920×1080 at ×4 with **zero letterbox** on
the most common monitor. (320×180 is the chunkier alternative; ×6 to 1080p.)

**Runnable:** a hardcoded checkerboard in the low-res target. Resize the window — it stays
crisp, integer-scaled, centered, with clean bars.

### Phase 3 — `.pix` decode, composite, atlas pack (100% headless)

No Vulkan, no GPU, no window. This is the largest pure-logic phase.

**Data model** (`pix/pix_document.h`) — pixels as a flat sorted `vector<pair<i16vec2, uint32_t>>`,
not a hash map: decode fills it once and only ever iterates it, and a 128×128 frame is ~16k entries.

**Decode** (`pix/pix_decoder.h`):
1. Sniff the 4-byte `"CAML"` magic; `caml::CAMLMap::load_caml(path)` if present, `load_yaml()` if
   not — so plain-YAML `.pix` files work as fixtures.
2. Walk `get_raw_node()`, tolerating **all three** shapes coopixel's `from_dict` accepts:
   top-level `animations:`, top-level `frames:` (wrap in one animation), top-level `layers:`
   (wrap in one frame in one animation).
3. Parse hot values by hand — `std::from_chars` around the comma for `"x,y"`, an 8-nibble hex
   table for `"#RRGGBBAA"`. **No `stoul`/`sscanf`** in a 16k-iteration loop.

**Composite** (`pix/pix_composite.h`), mirroring coopixel's `render_frame_qimage`:
skip layers where `!visible || opacity <= 0`; per layer emit effect-"below" pixels, then base,
then effect-"above"; multiply layer `opacity` into source alpha; source-over; output
**premultiplied** RGBA8. Implement the `stroke` effect (the only one coopixel has: size 1–10,
`outside`/`inside`/`center`) behind a sidecar `apply_effects` flag defaulting true.

**Trim** each frame to its opaque bbox, recording `offset_px`. coopixel frames are fixed-canvas,
so this is a large atlas-density win. An all-transparent frame trims to **1×1, never zero** —
zero-area atlas rects are a landmine.

**Pack** (`pix/atlas_packer.h`) every frame of every animation into **one** atlas: shelf packer,
frames sorted by descending height, `atlas_w = max(64, next_pow2(max_frame_w + 2))`, grow height
by powers of two. **1px transparent padding** between rects and at the border. Hard cap at
`maxImageDimension2D`; exceeding it throws, which `AssetManager` surfaces as `AssetState::Failed`.
Keep it behind an `AtlasPacker` interface so MaxRects can be swapped in later.

**Sidecar** (`pix/sprite_meta.h`) — `<stem>.sprite.yaml` resolved via `ctx.source->resolve(...)`:

```yaml
format: pixengine_sprite
version: "1.0"
pixels_per_unit: 16                 # 16 source px = 1 world unit
pivot: { x: 0.5, y: 0.0 }           # normalized on the UNTRIMMED canvas; (0.5,0) = bottom-center
apply_effects: true
premultiply: true
animations:
  walk_down:
    loop: Loop                      # Loop | Once | PingPong | Hold
    speed: 1.0
    pivot: { x: 0.5, y: 0.0 }       # per-animation override
    events: [ { frame: 1, name: footstep }, { frame: 4, name: footstep } ]
    frames: [ { index: 2, duration_ms: 60 } ]   # override the .pix's own duration
  chop:
    loop: Once
    events: [ { frame: 3, name: hit } ]
hitboxes:
  - { name: body, rect: { x: -0.3, y: 0.0, w: 0.6, h: 0.9 } }
anchors:
  hand_r: { x: 0.35, y: 0.55 }
```

Every key optional. Defaults: `ppu = 16`, `pivot = (0.5, 0)`, `loop = Loop`, `speed = 1`,
durations from the `.pix`. Merge by animation **name**, per-frame overrides by index.
Hitboxes and anchors are inert data — parsed and exposed, no system consumes them (scope).

**The fixture problem, and its structural fix.** There are no `.pix` files anywhere in the
workspace, and the format is encrypted+compressed — you cannot hand-author, diff, or review one.
So split the API:

```cpp
PixDocument decode_pix_document(const std::string& yaml_text);   // ← the testable core
PixDocument load_pix_document(const std::string& path);          // CAML sniff → the above
```

`test.cpp` embeds payload YAML as `R"YAML(...)YAML"` literals — no binary fixtures, no OpenSSL
or zstd in the test path, fully diffable. Add exactly **one** CAML round-trip test to prove the
container integration.

**Runnable:** `ctest` green. Optionally dump a packed atlas to PNG behind a `--dump` flag.

### Phase 4 — `PixLoader` + `SpriteAsset` on the GPU

`PixLoader : coopa::asset::TypedAssetLoader<SpriteAsset, DecodedPixAtlas>` — mirroring
`gfxcoopa/engine/loaders/texture_loader.h` exactly. `decode_typed()` does all of Phase 3 on the
worker thread. `finalize_typed()` on the main thread:

```cpp
auto atlas = gfx::engine::data::Texture::upload(
    device_, allocator_, cmd_pool_, d->pixels.data(), d->atlas_w, d->atlas_h,
    /*srgb=*/false, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);  // ← G4
```

**`SpriteAsset` is immutable after publish.** `AssetHandle<T>::get()` is **const-only**, so all
mutable per-instance state (current animation, frame index, elapsed time, ping-pong direction)
lives in `SpriteAnimator`, never in the asset. Ten renderers share one asset and animate
independently. State this in the header — it is the kind of rule violated once and then costing a day.

```cpp
struct SpriteFrame { ui::Rect uv; glm::vec2 size_px, offset_px, pivot_px; float duration_s; };
struct SpriteAnimation { std::string name; uint32_t first_frame, frame_count;
                         LoopMode loop; float speed;
                         std::vector<std::pair<uint32_t, std::string>> events; };
class SpriteAsset {   // move-only; destroying it frees the VkImage
    VkImageView atlas_view() const;
    const std::vector<SpriteFrame>& frames() const;
    const SpriteAnimation* find_animation(std::string_view) const;   // nullptr if absent
    float pixels_per_unit() const;  glm::ivec2 canvas_size() const;
    const std::vector<Hitbox>& hitboxes() const;
    const std::unordered_map<std::string, glm::vec2>& anchors() const;
};
```

Also this phase: `render/sprite_vertex.h`, `render/sprite_draw_list.h` (unsorted for now),
`render/sprite_pass.h`, `assets/shaders/sprite.vert/.frag`.

**`SpriteVertex` is layout-identical to `ui::UiVertex`** (`pos vec2`, `uv vec2`, `color RGBA8`,
20 bytes) but defined in pixengine, so sprite rendering doesn't *require* the UI library.
Rotation, pivot, flip and scale bake into the four world-space corners on the CPU — negligible
at Stardew scale. **Positions are in WORLD units, not screen pixels**; the camera lives in a push
constant. This is the load-bearing decision that lets static tilemap chunks survive camera motion.

**`SpritePass` is a near-clone of `uicoopa::UiPass`** with the same hard-won constraints —
`kFrames = 2` ring of host-visible buffers with doubling growth, a
`unordered_map<VkImageView, DescriptorSet>` cache, and a `register_textures()` that **must run
before `Renderer::begin_frame()`**. Differences: `Sampler::nearest` (non-negotiable, and the
reason `UiPass` can't be reused as-is), `blend_mode = PremultipliedAlpha` (G6), built against
the **offscreen** render pass, and a 32-byte push constant:

```cpp
struct SpritePush { float inv_half_extent[2]; float camera_pos[2]; float tint[4]; };
```
```glsl
// sprite.vert
vec2 ndc = (in_pos - pc.camera_pos) * pc.inv_half_extent;
gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);   // world +Y up, matching ui.vert
```

Missing-registration fallback binds a 1×1 **magenta** texture rather than crashing (UiPass binds
white; magenta is louder, and this is a dev engine).

**UV orientation trap.** `.pix` is top-left origin / +Y down (Qt); the atlas stores row 0 = top;
world space is +Y **up**. So `add_sprite` must map `uv.min.y` to the quad's **+Y** corner. This is
invisible in symmetric test art and obvious in asymmetric art — test it with an asymmetric fixture,
and make the first demo sprite asymmetric. Store every UV **half-texel inset**
(`u = (px + 0.5) / atlas_w`); combined with the 1px padding this eliminates all bleed.

Author `assets/sprites/player.pix` in coopixel (`uv run coopixel`) — the first phase needing a
real fixture.

**Runnable:** a pixel-perfect, unblurred, correctly-oriented sprite in the low-res target.
**Requires G4.**

### Phase 5 — Scene: `Transform2D`, `Camera2D`, `SpriteAnimator`, sorting

**`Transform2D` rather than reusing `TransformComponent`.** The 3D transform is `glm::vec3`
position / **Euler-degrees** `vec3` rotation / `vec3` scale / `glm::mat4` world matrix. Reusing it
means decomposing a `mat4` per sprite per frame to recover world x/y and 2D rotation — slow and
lossy. Y-sorting wants world-Y as a cheap float, thousands of times a frame. Pixel snapping and
pivot are properties of a *2D* transform with no home on the 3D one. And z-as-sort-order collides
with running `depth_test = false` — z would be a CPU sort key masquerading as a coordinate.
`uicoopa::RectTransform` is exact precedent.

```cpp
struct Transform2D : coopa::scene::Component {
    glm::vec2 position{0.0f};  float rotation = 0.0f;  glm::vec2 scale{1.0f};
    bool pixel_snap = true;
    glm::vec2 world_position{0.0f};  float world_rotation = 0.0f;  glm::vec2 world_scale{1.0f};
    std::string type_name() const override { return "Transform2D"; }
};
void resolve_transforms(coopa::scene::Scene&);   // ONE top-down DFS per frame
```

With `auto_transform: false`, `SceneObject::add_child()` does no transform linking for us. Rather
than reinvent parent pointers and dirty flags, mirror uicoopa's Canvas: one O(n) DFS,
`world = parent_world ∘ local` with a 2×2 rotate/scale. No matrices stored, no dirty bits, no
atomics, deterministic, trivially unit-testable. Objects without a `Transform2D` are transparent
group nodes.

**Sort keys** — a `uint64_t` so comparison is one integer compare:

```
bits 63..48 (16)  sort_layer + 32768         Ground=-100 … Overlay=+100
bits 47..24 (24)  depth   — y_sort ? quantize(-world_y) : (order_in_layer + 2^23)
bits 23..8  (16)  texture_bucket             small hash of VkImageView — coalesces batches
bits  7..0  ( 8)  sub_order                  stable tiebreak within one entity
```

`sort_and_flatten()` uses `std::stable_sort` over an **index array** (4 bytes moved, not 96).
The texture bucket means that among sprites at the same layer and depth, same-texture quads
coalesce — the main batch-count win.

**`Camera2D`** with `zoom`, `follow_target_name`, `follow_lerp`, `follow_deadzone`,
`clamp_to_bounds`/`world_bounds`, `pixel_snap`:

```
hx = VW / (2 * PPU * zoom)        // half-extents in world units
hy = VH / (2 * PPU * zoom)
push.inv_half_extent = { 1/hx, 1/hy }
visible_world_rect() = { cam - (hx,hy), cam + (hx,hy) }   // culling AND residency
```

**Snap in render-pixel units**, so `zoom != 1` still lands on target pixels:
`cam_snapped = round(cam * PPU * zoom) / (PPU * zoom)`. Do **not** use `gfx::data::CameraUBO` —
it snaps a `vec3` and uploads two `mat4`s per frame, pure overhead here; a 32-byte push constant
needs no descriptor set, no buffer, no ring.

**Sprites must snap too** — a sprite at a fractional offset from a snapped camera still shimmers.
Snap the sprite's *pivot origin* to the same grid, then build corners from it. Never snap corners
individually or the sprite deforms by a pixel as it moves. Gate on `Transform2D::pixel_snap`.

Also: `scene/scene_view.h` (see the main loop), `scene/register.h`, and a local `TopDownMover` in
`demo.cpp` — deliberately *not* in the library, keeping the scope line visible.

**Runnable:** an animated sprite walks around, camera follows with deadzone and bounds clamping,
no shimmer. Add a tree to confirm y-sorting works walking in front of and behind it.

### Phase 6 — Tilemap (dynamic batching)

```yaml
format: pixengine_tilemap
version: "1.0"
tile_size: { x: 16, y: 16 }
pixels_per_unit: 16                 # → 1 tile == 1 world unit
tileset: { source: ../tilesets/farm_tiles.pix, columns: 16, first_gid: 1 }   # gid 0 = empty
layers:
  - name: ground
    sort_layer: -100
    y_sort: false
    width: 64
    height: 64
    encoding: rle                   # "rle" ("count:gid") or "flat" (plain int list)
    data: [ "512:5", "1:9", "3:5" ]
  - name: buildings
    sort_layer: 0
    y_sort: true                    # tall tiles that occlude / are occluded by the player
collision:                          # parsed and exposed; NO collision system (scope)
  - { layer: buildings, solid_gids: [9, 10, 11] }
```

`y_sort: true` emits those tiles through the **same** `SpriteDrawList` as characters, so a player
walking behind a house is correctly occluded. That one flag is what makes it feel Stardew-like.
Keeping the `collision:` schema now means adding physics later isn't a format break.

Storage is a dense `std::vector<uint16_t>` per layer, row-major, **origin top-left** — a 256×256
layer is 128 KB, and sparse storage would cost a hash lookup in the innermost loop. RLE expansion
runs off-thread in `TilemapLoader : TypedAssetLoader<TilemapAsset, DecodedTilemap>`.

**Dynamic batching is the right call here, not baked chunks.** At 480×270 with 16px tiles the
viewport is 30×17 tiles; culled to `visible_world_rect()` plus one tile of margin, a layer emits
**≤ ~540 quads**. Four layers ≈ 2200 quads ≈ 8800 vertices — comfortably under UiPass's *initial*
8192-vertex buffer. Simple, handles animated tiles for free, and interleaves y-sorted layers with
characters automatically. (Baked chunks are Phase 8, only if a profile justifies it.)

Culling is pure math and gets its own test block — note the **Y inversion**, since tile row 0 is
the *top* (largest world Y). Off-by-one and flip errors here are the classic tilemap bug:

```cpp
struct TileRange { int x0, y0, x1, y1; bool empty() const; };
TileRange tile_rect_from_world_rect(const ui::Rect& world, glm::vec2 tile_size_units,
                                    glm::ivec2 layer_size, glm::vec2 layer_origin);
```

**Runnable:** the sprite walks around a tilemap; a `y_sort: true` buildings layer occludes it.
Log the per-frame quad count to confirm culling (~2000, not ~65000).

### Phase 7 — Residency + streaming

This is decision 3, and it leans **entirely** on `AssetManager`'s existing machinery — no new
eviction system.

```cpp
assets.set_idle_eviction(true);
assets.set_max_idle_frames(180);   // ~3s at 60fps before an unreferenced payload retires
```

The chain: `ref_count` hits 0 → `sweep_idle_()` increments `idle_frames` each `update()` → at 180
it calls `retire_payload_()` (plus `k_payload_grace_frames` before the `VkImage` is actually
destroyed, so no in-flight GPU work is yanked) **and erases the slot**. Re-entering range creates
a fresh slot and re-decodes from disk. Exactly the requested behavior.

**Who holds handles:** `SpriteRenderer` owns `AssetHandle<SpriteAsset> handle_` and nothing else
does — so refcount means exactly "how many live renderers want this sprite."

**Who decides:** a per-frame system split into a **pure, headless-testable** decision function and
a thin applier:

```cpp
struct ResidencyItem { uint32_t index; bool wants_visible, has_handle, is_loading;
                       float distance_to_camera; uint16_t out_of_range_frames; };
enum class ResidencyAction { None, Acquire, Release };
std::vector<ResidencyAction> decide_residency(const std::vector<ResidencyItem>&,
                                              uint32_t max_acquires_this_frame,
                                              uint16_t release_delay_frames);
```

1. `keep_rect = camera.visible_world_rect()` expanded by a `residency_margin` (default one extra
   screen per side — a two-screen prefetch).
2. `wants_visible = owner->active() && enabled && aabb_intersects(estimated_aabb, keep_rect)`.
   **Chicken-and-egg:** you can't know a sprite's size before its asset loads. `SpriteRenderer`
   carries an authored `cull_size` from YAML (default `{2,2}`, conservative); once loaded, use the
   real trimmed extent. Log a one-time warning if the real size exceeds `cull_size` by >20% —
   that's a pop-in bug waiting to happen.
3. **Acquire budget:** at most **4/frame**, prioritized by ascending distance. This is the throttle
   that stops `upload_image_2d`'s `vkQueueWaitIdle` from stacking (see Risk 2).
4. **Release hysteresis:** release only after **30 consecutive** out-of-range frames. Without it, a
   sprite pacing the camera edge releases/re-acquires every other frame, which also resets
   `idle_frames` to 0 forever and defeats `max_idle_frames` entirely.

Two deliberate layers: ours prevents *acquire/release churn*, `max_idle_frames` prevents *GPU
thrash* for a sprite that leaves and returns within a few seconds.

**While `Loading`: draw nothing.** With a two-screen prefetch and a 4/frame budget a sprite is
essentially never simultaneously visible and unloaded, and a flashing placeholder would be strictly
worse than a one-frame pop. `is_failed()` also draws nothing and logs `error()` once per asset id.
`SpritePass::set_debug_placeholder(true)` draws a magenta quad at `cull_size` — that exists
precisely to *prove* the invariant during development.

> **THE TRAP: never cache a `VkImageView` across a residency gap.** Eviction erases the slot and
> destroys the `VkImage`; a cached view becomes a dangling handle Vulkan will happily use.
> **Hard rule: `SpriteRenderer` re-reads `handle_->atlas_view()` every frame** (a two-pointer
> chase). Any future caching must guard on `handle_.revision()`. Put this in the header in capitals.

**Hot reload** comes free: `poll_for_reloads_()` skips slots with `ref_count == 0`, so it reloads
exactly the sprites currently on screen. Save in coopixel, see it live. `on_reloaded` needs no
subscriber because nothing caches derived state.

**Runnable:** walk away from a sprite cluster; GPU memory drops after ~3s and recovers on return
with no hitch and no validation errors.

### Phase 8 — UI overlay, resize, input map, (optional) chunk cache

- **UI**: a **separate** uicoopa `Scene` with its own `Canvas` root and `.yaml`.
  `RectTransform` and `Transform2D` are different coordinate systems; mixing them in one tree
  invites confusion. Two `Scene` objects, both updated in the main loop.
- **Resize** end-to-end (G9): the virtual target *never* resizes — only the letterbox recomputes.
- **`InputMap`**: named actions over keys/buttons/gamepad (G7, G8).
- **Switch from the `vkQueueWaitIdle` fallback to `pre_pass_fn`** (G1) and land G2.
- **Optional, only if profiled:** static tilemap chunk cache. Bake per-chunk (16×16 tiles ≈ 20 KB)
  world-space vertex buffers for `y_sort: false` layers, lazily on first visibility, in an LRU
  keyed by `(layer, chunk_x, chunk_y)`. One shared static index buffer serves every chunk. A
  480×270 viewport touches ≤ 3×2 chunks per layer — 6 draw calls instead of 540 quads of vertex
  writes. Chunk vertices are in **world space**, which is why camera movement invalidates nothing.
  This is a pure optimization behind an unchanged `TilemapRenderer` API.

**Requires G1, G2, G3, G6, G7, G8, G9.**

---

## The Frame

```cpp
float dt = clock.tick();

window.new_frame();
window.poll_events();

// Renderer::handle_resize() only rebuilds framebuffers — the caller owns the swapchain recreate.
if (window.was_resized()) {
    window.wait_while_minimized();                  // G9
    auto [w, h] = window.framebuffer_size();
    device.wait_idle();
    swapchain.recreate(w, h);
    renderer.recreate_framebuffers();
    pixels.on_swapchain_resized(w, h);              // recomputes the letterbox ONLY
    window.reset_resized();
}

// GPU upload (finalize) happens HERE — main thread, no pass open, before anything records.
// Also ages retired payloads and sweeps idle slots.
assets.update(dt);

input.update(window);
world.update(dt);  hud.update(dt);

canvas->set_viewport(sw, sh);
canvas->set_window_input(window);
world.late_update(dt);  hud.late_update(dt);        // Canvas does measure→arrange→emit here

resolve_transforms(world);                          // one top-down DFS
view.refresh_if_dirty();                            // cached component gather — see below
residency.tick(view, camera->visible_world_rect(ppu, VW, VH));

auto& list = pixels.draw_list();
list.begin();
for (auto* tm : view.tilemaps())         tm->emit(list, *camera, ppu);
for (auto& r  : view.sprite_renderers())  r->emit(list, ppu);
list.sort_and_flatten();

// MUST be before begin_frame(): DescriptorSet::bind_image() calls vkUpdateDescriptorSets
// immediately, which is illegal once a render pass is open (ui_pass.h:11-14).
pixels.register_textures(list, canvas->draw_list());

pixels.render(renderer, *camera, list, canvas);     // one command buffer, one submit
```

`PixelRenderer::render()` internally — the exact GPU ordering:

```
cmd.begin()
  ├─ pre_pass_fn (G1):
  │     offscreen.begin(cmd, world_clear_color)     // LOAD_OP_CLEAR on the 480x270 target (G3)
  │     cmd.set_viewport(0, 0, VW, VH)              // override OffscreenTarget's 3D Y-flip
  │     cmd.set_scissor (0, 0, VW, VH)
  │     sprite_pass.draw(cmd, frame, push, chunk_draws, list)
  │     offscreen.end(cmd)                          // → SHADER_READ_ONLY_OPTIMAL (needs G2!)
  │
  ├─ begin_render_pass(swapchain fb, clear = bar_color)   // ← bars painted for free
  │     upscale_pass.draw(cmd, letterbox_rect)      // viewport+scissor = dst rect, nearest, draw(3)
  │     ui_pass.draw(cmd, frame, sw, sh, canvas->scale_factor(), canvas->draw_list())
  └─ end_render_pass()
cmd.end()  →  single vkQueueSubmit  →  present
```

`UpscalePass::set_source_image()` calls `bind_image` (immediate descriptor update), so it must
only be called **outside** the render pass — i.e. only when the offscreen view actually changes.
Guard it on a cached-view comparison.

### `SceneView` — why it exists

`Scene::get_components<T>()` walks every node, `dynamic_cast`s each component, and heap-allocates
a fresh `std::vector<T*>` on **every call** (`scene.h:169-180`). gfxcoopa's `gather_renderables()`
tolerates that because it runs once per bake; we'd call it three times a frame across thousands of
nodes. So cache it:

```cpp
class SceneView {
    void invalidate();          // call on spawn / destroy / reparent
    void refresh_if_dirty();    // ONE DFS resolving all four component pointers per object
    const std::vector<SpriteRef>&        sprite_renderers() const;  // {obj, tf, sr, anim}
    const std::vector<TilemapRenderer*>& tilemaps() const;
    Camera2D*                            active_camera() const;
};
```

`invalidate()` is explicit because `Scene` has no structural-change signal — a contract the demo
and any future spawner must honor. **A stale `SceneView` after a destroy is a use-after-free.**
Loud header comment; consider re-validating against the tree in debug builds.

---

## Scene YAML

Registered via `register_pix_components(device, allocator, cmd_pool, assets)` — same shape and
same teardown contract as `gfx::engine::components::register_render_components`: **call
`SceneLoader::clear_component_parsers()` before the `Device` dies**, since parsers capture GPU
objects by reference.

| `type:` | Purpose |
|---|---|
| `Transform2D` | position / rotation / scale / pixel_snap |
| `SpriteRenderer` | sprite path, color, flip, sort_layer, order_in_layer, y_sort, cull_size |
| `SpriteAnimator` | default animation, speed, autoplay |
| `Camera2D` | zoom, follow target, deadzone, bounds |
| `TilemapRenderer` | tilemap path, per-layer sort overrides |
| `Transform` | **warning stub** — fires if the scene forgot `auto_transform: false` |

```yaml
format: pixengine
scene:
  scene_name: farm
  auto_transform: false          # REQUIRED — pixengine uses Transform2D, not the 3D Transform
  root_objects:
    - name: Camera
      components:
        - type: Transform2D
          position: { x: 0.0, y: 0.0 }
        - type: Camera2D
          zoom: 1.0
          follow_target: Player
          follow_lerp: 8.0
          follow_deadzone: { x: 1.0, y: 0.75 }
          clamp_to_bounds: true
          world_bounds: { min: { x: 0.0, y: -64.0 }, max: { x: 64.0, y: 0.0 } }
          pixel_snap: true

    - name: Farm
      components:
        - type: Transform2D
          position: { x: 0.0, y: 0.0 }
        - type: TilemapRenderer
          tilemap: ../../tilemaps/farm.tilemap.yaml

    - name: Player
      components:
        - type: Transform2D
          position: { x: 12.0, y: -8.0 }
          pixel_snap: true
        - type: SpriteRenderer
          sprite: ../../sprites/player.pix     # sidecar player.sprite.yaml auto-merged
          color: { r: 1.0, g: 1.0, b: 1.0, a: 1.0 }
          sort_layer: 0
          y_sort: true
          cull_size: { x: 1.0, y: 2.0 }        # residency estimate before the asset resolves
          flip_x: false
        - type: SpriteAnimator
          default_animation: idle_down
          speed: 1.0
          autoplay: true
      children:
        - name: Shadow
          components:
            - type: Transform2D
              position: { x: 0.0, y: -0.05 }
            - type: SpriteRenderer
              sprite: ../../sprites/shadow.pix
              color: { r: 1.0, g: 1.0, b: 1.0, a: 0.5 }
              sort_layer: 0
              y_sort: true
              y_sort_offset: -0.01             # always just under the player
              cull_size: { x: 1.0, y: 0.5 }
```

**`type:` everywhere, never `!Tag`** — the vendored fkYAML 0.4.2 fails on a block-style `!Tag` as
the 2nd+ sequence item, and every object here has 2–3 components. Zero exceptions.

---

## Testing and Validation

Copy uicoopa's harness verbatim: `RUN_TEST` / `ASSERT_TRUE` / `ASSERT_EQ` / `ASSERT_NEAR` with
ANSI coloring, `g_tests_run` / `g_tests_failed`, `main()` returning nonzero on failure.

```bash
cd /home/coopa/git/pixengine
cbuild --vulkan        # glslc-compiles assets/shaders/** to .spv, then cmake + make
ctest --test-dir build --output-on-failure     # headless suite
cplay                  # runs ./build/pixengine_demo
```

### Headless (`test.cpp`, run by ctest) — ~60–80 assertions, zero Vulkan symbols

| Area | Cases |
|---|---|
| `color` | `"#RRGGBBAA"` parse (upper/lower/malformed); RGBA8 byte order **matching `UiVertex::pack_color`**; source-over; premultiply round-trip |
| `pix_decoder` | all three legacy shapes; `"x,y"` parse incl. negatives and whitespace; missing/extra fields; malformed input throws rather than UB |
| `pix_composite` | layer order; `visible:false` and `opacity:0` skipped; fractional opacity; stroke at sizes 1/2/10 × outside/inside/center; below→base→above ordering; trim bbox incl. all-transparent → 1×1 |
| `atlas_packer` | no overlaps; 1px padding; deterministic across runs; power-of-two growth; over-cap throws; single-rect and zero-rect edges |
| `sprite_meta` | defaults with no sidecar; per-animation and per-frame overrides; unknown keys ignored; loop-mode parse |
| `pixel_math` | `compute_letterbox` (exact multiples, window < virtual, extreme aspects, scale never 0); `snap_to_pixel` at zoom 1/2/3 |
| `Camera2D` | `visible_world_rect` at several zooms; deadzone; bounds clamp; **snapping stability** — a slowly-moving camera must produce a monotonic snapped sequence, never oscillate |
| `tilemap` | RLE expansion incl. malformed runs; `TileLayer::at` bounds; `tile_rect_from_world_rect` incl. the **Y inversion**, full/partial/no overlap |
| `SpriteDrawList` | corner positions under pivot × rotation × flip × scale vs. hand-computed values; **UV orientation with an asymmetric fixture**; sort-key packing round-trip and ordering; batch coalescing count |
| `decide_residency` | acquire budget respected and distance-prioritized; release only after N frames; no thrash when an item oscillates across the boundary; already-loading items not re-acquired |
| `resolve_transforms` | 3-deep hierarchy with rotation + scale; missing-`Transform2D` group nodes pass through; inactive subtrees |
| `caml` | exactly one round-trip: build → `save_caml` → `load_caml` → compare |

### Live-window validation (`demo.cpp`, deliberately outside ctest)

Run everything below with `VK_LAYER_KHRONOS_validation` **plus synchronization validation** —
zero sync errors is the regression test that proves G2 was needed.

| Phase | Manual check |
|---|---|
| 1 | Window opens, clears to a solid color |
| 2 | Resize → checkerboard stays crisp, integer-scaled, centered, clean bars. Pixel-count the squares to confirm the scale factor. |
| 3 | `--dump` the packed atlas to PNG and eyeball it |
| 4 | Sprite is pixel-perfect, unblurred, **correctly oriented** (asymmetric art!) |
| 5 | Sprite walks, camera follows with deadzone + clamp, **no shimmer**; walk in front of and behind a tree to confirm y-sorting |
| 6 | Tilemap renders; `y_sort: true` buildings occlude the player. Log per-frame quad count → ~2000, not ~65000. |
| 7 | Walk away from a sprite cluster → GPU memory drops after ~3s, recovers on return, no hitch, no validation errors. Toggle the debug placeholder to see the streaming boundary. |
| 8 | Crisp native-res UI text over the upscaled world; smooth resize; controller input; hot-reload a `.pix` from coopixel and watch it change live |

---

## Risks, Ranked

1. **Missing exit subpass dependency (G2).** Recording offscreen + swapchain into one command
   buffer exposes a real hazard that blendy's `vkQueueWaitIdle` currently hides.
   *Mitigation:* ship Phase 2 with the wait-idle fallback; land G2 before Phase 8 flips to
   `pre_pass_fn`; validate with sync-validation on.
2. **`upload_image_2d`'s `vkQueueWaitIdle` × unbudgeted `AssetManager::update()`.** `update()`
   finalizes *every* ready load in one call, with no per-frame budget knob; each finalize stalls
   the device. Three sprites finalizing on one frame = three stalls = a visible hitch.
   *Mitigation:* cap `load_async` issuance at 4/frame, distance-prioritized. If hitches persist,
   the next step is a libcoopa finalize budget — flagged, not designed.
3. **Dangling `VkImageView` after eviction.** `sweep_idle_()` erases the slot and the payload
   destructor frees the `VkImage`. *Mitigation:* re-read `atlas_view()` every frame; if ever
   cached, guard on `handle_.revision()`.
4. **Immediate `vkUpdateDescriptorSets`.** `SpritePass::register_textures()`,
   `UiPass::register_textures()` and `UpscalePass::set_source_image()` must all run **before**
   `begin_frame()`. *Mitigation:* funnel all three through one `PixelRenderer::register_textures()`
   called at exactly one place; missing registration falls back to magenta rather than crashing.
5. **`Scene::get_components<T>()` per frame.** *Mitigation:* `SceneView` with explicit
   `invalidate()`. Secondary risk: a *stale* view after a destroy is a use-after-free.
6. **fkYAML cost on 16k-entry sparse maps** — a 6-frame 128×128 animation is ~100k node
   allocations. It's all on the worker thread, so it costs latency, not framerate. Measure in
   Phase 4. *Escape hatch:* an offline `.pixatlas` bake, which the `AtlasPacker`/`DecodedPixAtlas`
   split makes a drop-in later.
7. **fkYAML `!Tag` bug.** *Mitigation:* `type:` in every example and every committed scene.
8. **One virtual path → exactly one C++ type.** A `.pix` loaded as both `SpriteAsset` and a raw
   `Texture` returns an invalid handle **silently**. *Mitigation:* `.pix` is only ever
   `SpriteAsset`; tilesets wanting a plain texture use `.png`. Log loudly on an invalid handle.
9. **`auto_transform: false` is easy to forget** — silent failure. *Mitigation:* the `Transform`
   warning-stub parser.
10. **UV Y-orientation** — invisible in symmetric art. *Mitigation:* asymmetric test fixture and
    asymmetric first demo sprite.
11. **`Window::width()`/`height()` semantics change (G9)** — the one non-purely-additive change.
    *Mitigation:* grep for `.width()`/`.height()` on `Window` across all four siblings first.
12. **`AssetManager` is main-thread-only** and owns a *dedicated* `JobEngine` (never the app's
    per-frame one — `CounterPool::reset()` has no generation tag). *Mitigation:* never
    construct/copy/destroy an `AssetHandle` off the main thread. Do not move residency into a
    `JobScheduler` job.

---

## Critical Files

- `/home/coopa/git/gfxcoopa/gfxcoopa/presentation/renderer.h` — needs the `pre_pass_fn` hook (G1);
  its structure dictates the entire frame ordering
- `/home/coopa/git/gfxcoopa/gfxcoopa/pipeline/render_pass.h` — missing exit dependency (G2); the
  `LOAD_OP_CLEAR` constraint that shapes compositing
- `/home/coopa/git/uicoopa/uicoopa/render/ui_pass.h` + `draw_list.h` — the reference `SpritePass`
  is modeled on (descriptor cache, `kFrames` ring, register-before-`begin_frame`)
- `/home/coopa/git/libcoopa/coopa/asset/asset_manager.h` — `load_async` / `update` / `sweep_idle_`
  / `retire_payload_`; the entire residency design rests on these
- `/home/coopa/git/gfxcoopa/gfxcoopa/engine/loaders/texture_loader.h` — the two-stage loader
  `PixLoader` mirrors exactly
- `/home/coopa/git/gfxcoopa/gfxcoopa/engine/components/register.h` — the YAML-parser registration
  and teardown contract to copy
- `/home/coopa/git/coopixel/src/coopixel/models/document.py` — the authoritative `.pix` schema and
  `render_frame_qimage` compositing order to port
- `/home/coopa/git/blendy/CMakeLists.txt` — the CMake recipe (only sibling with the zstd + OpenSSL
  blocks caml needs)
