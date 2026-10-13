# Known Issues

The working list of known bugs in the recomp, what's being done about each one,
and where the fix lives. Player-facing bugs also get a line under "Known
issues" in the [README](README.md#native-renderer). That line comes out once
the fix ships.

## How to use this file

- Each issue gets the next free `KI-N` id. Ids are never reused.
- **Status** is one of `open`, `investigating`, `fix in review`, `fixed`
  (merged and confirmed in-game), `won't fix`, or `could not reproduce`.
- **Tracking** lists the PRs, GitHub issues and sessions working on it.
- Fixed issues move to [Resolved](#resolved) with the PR that fixed them. The
  repro and cause notes stay there for when a regression turns up.
- A PR that fixes an issue names its id (for example `Fixes KI-16`) and updates
  its entry in the same change.
- Name locations by file and function, not line numbers; line numbers drift.
- For rendering bugs, always say whether the emulated renderer (F5) shows the
  same problem. A bug that only shows on the native renderer is a native
  renderer bug.

Capture recipe for native renderer bugs: launch with
`--skate3_native_render_capture_hotkeys=true`, switch to the native renderer
with F6, toggle F5/F6 to compare with the emulated renderer, then press F10.
That dumps the draws and a full guest-memory `.gsnap` into
`<cwd>/native_render_snapshots`.

### Code review findings (KI-8 to KI-33)

KI-8 to KI-33 come from a full code review in October 2026. They were first
tracked as `BT-01` to `BT-26` in `docs/bug-tracker.md`, which this file
replaces; the KI id is the BT number plus 7 (BT-09 is KI-16). PR #19 and the
commits from #17 and #19 still use the BT ids. Most of these are code-level
risks found by reading the code, not bugs reported from play, so they have no
repro steps; the entry says where the problem is and how it would show.

**Severity** (code review entries only):
- **medium:** wrong output, stuck state or memory safety in normal play.
- **low–medium:** slow leaks, debug-only memory hazards, or features that
  silently stop working.
- **low:** cleanup, edge cases, cosmetic.

## Open

### KI-3: Park editor warehouses render wrong on the native renderer

- **Status:** open
- **Area:** native renderer, park editor
- **Tracking:** none yet
- **Symptom:** skate parks and park editing have visual parity problems on the
  native renderer. The clearest case is the park-editor warehouse interiors.
  This is the README's remaining "Skate parks and park editing" known issue.
- **Cause so far:** the warehouse interior environment shader never passes the
  world-row capture gate, so the native path never captures it as world
  geometry.
- **Next step:** take an F10 capture inside a park-editor warehouse to see what
  the interior draws look like and why the gate rejects them.

### KI-6: Tattoos don't show on the player character

- **Status:** open
- **Area:** emulation (both renderers), character customization
- **Tracking:** none yet
- **Symptom:** tattoos added in Edit Skater don't appear on the skater. This
  happens on the emulated renderer (F5) as well as the native one. Split out
  of KI-4.
- **Cause so far:** unknown. Since the emulated renderer is wrong too, the
  native port isn't missing anything here. The native renderer already follows
  the game's tattoo path (`cacstamp_skin` samples `i_decal` at tf6 through the
  `i_customGraphic` affine, c190/c191). A fix earlier in the pipeline should
  show on both renderers.
- **Next step:** take an F10 capture in Edit Skater with a tattoo applied.
  Check what the `cacstamp_skin` draw binds at tf6 (a blank or default
  texture, or the real art) and the values in c190/c191. A blank tf6 means the
  tattoo never reaches the texture. The texture-space composite pass may be
  suppressed or failing, its resolve may be lost, or the recomp may break a
  CPU-side step.

### KI-16: Upload rings reused without a GPU completion check

- **Status:** open · **Severity:** low–medium · was BT-09
- **Area:** native renderer, GPU uploads
- **Where:** `src/skate3_native_scene_gpu.cpp`, `src/skate3_native_scene_post.cpp`
- **Problem:** several upload and constant rings pick their slot with
  `frame_number % N`: the UI ring, the shadow constant buffer, the bone ring,
  the photo FX CB slots (`% 4`) and the screen FX CB. None checks that the GPU
  has finished with the slot. The comment above the ring sizes in
  `skate3_native_scene_gpu_internal.h` notes that a slot was observed being
  recycled while still in use. It would show as one-frame corruption of HUD,
  bones or constants when the GPU falls behind.
- **Fix idea:** gate each slot on `CompletedSubmission()`, or size the rings
  from the device's frames in flight.

### KI-17: Texture routing maps grow without bound

- **Status:** open · **Severity:** low–medium · was BT-10
- **Area:** native renderer, texture cache
- **Problem:** `tex_routes`, `tex_sticky`, `words_sticky` and
  `tex_pending_first` are only cleared by the debug cache flush, so they grow
  for the whole session.
- **Fix idea:** prune by `last_used_frame`, like `tex_store`.

### KI-18: Draw-distance slots never freed; LOD distances scaled in place

- **Status:** open · **Severity:** low–medium · was BT-11
- **Area:** draw distance
- **Where:** `src/skate3_draw_distance.cpp`
- **Problem:** the 16 fixed cull-threshold slots are never freed, so
  draw-distance scaling silently stops once enough cull objects have been
  seen. LOD distances are multiplied in place every frame, which relies on the
  game rewriting them each frame.

### KI-19: DLC and recipe directory scans can throw at startup

- **Status:** open · **Severity:** low–medium · was BT-12
- **Area:** app startup
- **Where:** `src/skate3_app_common.cpp`
- **Problem:** the DLC and recipe scans use the non-throwing iterator
  constructor, but `operator++` can still throw (for example on a permissions
  error partway through a folder), which would end startup with an exception.
- **Fix:** step with `increment(ec)`.

### KI-20: Depth-view caches compare textures by pointer

- **Status:** open · **Severity:** low–medium · was BT-13
- **Area:** native renderer, post effects
- **Where:** `s_pfx_depth_tex` (photo FX in `RenderScene`), and
  `ao_depth_srv_of`, `ssr_depth_srv_of`, `vol_depth_srv_of`
  (`src/skate3_native_scene_post.cpp`)
- **Problem:** a rebuilt depth texture is detected by comparing pointers. A
  texture freed and reallocated at the same address keeps the old view,
  possibly with the wrong MSAA dimension. The photo FX one is most exposed
  because it can sit idle across several rebuilds (for example MSAA changes
  while the photo editor is closed).
- **Fix idea:** a generation counter bumped on every depth rebuild.

### KI-21: Occlusion culling silently inactive with SSAO off

- **Status:** open · **Severity:** low–medium · was BT-14
- **Area:** native renderer, performance
- **Problem:** the occlusion grid is only produced inside `ApplySsaoPass`.
  With SSAO off, the grid expires after 240 frames and the occlusion cull
  stays off without any message.

### KI-22: Shadow-dump readback buffers not resized (debug only)

- **Status:** open · **Severity:** low–medium · was BT-15
- **Area:** native renderer, diagnostics
- **Where:** `src/skate3_native_scene_gpu.cpp`
- **Problem:** the shadow-dump readback buffers keep their first size after a
  shadow tile change, so a later recording copies out of bounds on the GPU.

### KI-23: OnShutdown misses several UnregisterBind calls

- **Status:** open · **Severity:** low · was BT-16
- **Where:** `src/skate3_app_common.cpp`
- **Problem:** the screenshot bind (which captures `this`), the F5 toggle,
  showcase and freecam binds are never unregistered on shutdown.

### KI-24: XUID parsing and TOML escaping in user settings

- **Status:** open · **Severity:** low · was BT-17
- **Where:** `src/skate3_user_settings.cpp`
- **Problem:** `ParseXuidString` tries hex first, so it misreads decimal
  XUIDs. `TomlString` doesn't escape control characters.

### KI-25: Vehicle retention frustum test ignores the ultrawide scale

- **Status:** open · **Severity:** low · was BT-18
- **Where:** `src/skate3_native_scene.cpp`
- **Problem:** the vehicle retention test omits `wide_hor_scale`, unlike the
  static retention test, so on ultrawide outputs vehicles at the screen edges
  can drop out of retention too early.

### KI-26: ProcessPrewarmEntry does an unguarded read on a worker

- **Status:** open · **Severity:** low · was BT-19
- **Where:** `src/skate3_native_scene_gpu.cpp`
- **Problem:** it does a guarded copy into `record` that is never used, then a
  raw `REX_LOAD` on the worker thread.

### KI-27: DecodeMesh input validation

- **Status:** open · **Severity:** low · was BT-20
- **Problem:** element offsets aren't checked against the vertex stride (a
  host over-read on a malformed declaration), and several `Map()` results
  aren't checked for null.

### KI-28: Options menu edge cases

- **Status:** open · **Severity:** low · was BT-21
- **Where:** `src/skate3_options_menu.cpp`
- **Problem:** `StepRow` takes `% count` with no guard for an empty label
  list. `g_restart_popup_open` can be left set if the popup closes by a path
  other than its callback.

### KI-29: Icon updater leaves `.icon-new` behind

- **Status:** open · **Severity:** low · was BT-22
- **Where:** `src/skate3_win_icon.cpp`
- **Problem:** a failed swap leaves the `.icon-new` file in place.

### KI-30: F12 dialog rewrites a cvar the render thread overrides

- **Status:** open · **Severity:** low · was BT-23
- **Where:** `src/skate3_native_debug_dialog.cpp`
- **Problem:** `native_render_suppress_emulated_draws` is read and written back
  every UI frame, which can clobber the render thread's temporary override
  (see KI-11). Other sliders the render thread touches follow the same pattern.
- **Fix:** only call `REXCVAR_SET` when the widget reports a change.

### KI-31: C++ tests not built, no CI

- **Status:** open · **Severity:** low · was BT-24
- **Problem:** `tests/*.cpp` (character fade, Hall of Meat, screen FX) aren't
  wired into CMake, and they use `assert`, which does nothing in release
  builds. This repository has no CI, so neither these nor the Python tests
  (`python -m unittest discover -s tests`) run automatically.

### KI-32: Small cleanups

- **Status:** open · **Severity:** low · was BT-25
- **Problems:**
  - Screenshots taken within the same second get the same filename.
  - `blur_tex` isn't recreated when the output format changes.
  - The `kSceneRingFrames` comment says about 900, but the value is 2400.
  - The header declaring `MaybeDumpSceneRing` is missing includes.

### KI-37: Dead code for building without Title Update 3

- **Status:** open · **Severity:** low
- **Area:** build, codegen
- **Tracking:** none yet. Follow-up to KI-33 (#27).
- **Problem:** since #27, configure fails without TU3 codegen, so
  `SKATE3_HAS_TITLE_UPDATE_CODEGEN` and `SKATE3_HAS_TITLE_UPDATE` are always
  on. Everything behind the off branch is dead:
  - `CMakeLists.txt`: the default `SKATE3_FUNCTION_CONFIG_INCLUDES` pointing at
    `config/skate3_functions.toml`, the `if(SKATE3_HAS_TITLE_UPDATE_CODEGEN)`
    wrappers, and the `=0` branch of the compile definition.
  - `config/skate3_functions.toml`: the retail-only function boundaries.
    Nothing reads it any more.
  - `src/exception_compat.cpp`: the `#else` branch (the retail guard
    `sub_82F44E40` and its trampoline).
  - `src/skate3_app_common.cpp`: the `#if !SKATE3_HAS_TITLE_UPDATE`
    `Sub82EBAE4CImpl` and its `SetFunction(0x82EBAE4C)` registration, the
    non-TU `#else` of the macOS ISO-install chain, and the always-true guard
    around the title update installer check.
  - `src/skate3_guest_heap_diag.cpp`: the `#else` stub of
    `Skate3GuestHeapCategoryFreeBytes`.
  - `src/skate3_options_menu.cpp`: the always-true `SKATE3_HAS_TITLE_UPDATE`
    guards and any `#else` stubs paired with them.
  - The static_asserts #27 added to the hook blocks can go if the macro itself
    is removed. Keep them if it stays.
- **Fix:** delete the dead branches. Behaviour in TU3 builds doesn't change.
  Test with configure + build, then the unattended `--skate3_demo_path=true`
  boot.

### KI-36: Event-start 2D draws with a 32-byte vertex layout are dropped

- **Status:** open
- **Area:** HUD / UI, events (native renderer)
- **Tracking:** none yet. Found while fixing KI-34 (#25).
- **Symptom:** none seen in play yet. The logs show that at the start of an Own
  the Lot event some 2D draws are never drawn on the native renderer, so an
  intro or banner element may be missing.
- **Evidence:** temporary per-second 2D stream stats (not committed) logged
  draws with a 32-byte vertex stride as quad lists (prim 13) in the
  event-start window: 64 in one second and 83 the next. `g_draws_2d_other`
  rose from 0 to 147 at the same time. A free-roam run showed other bursts in
  `g_draws_2d_other` (+240, +176), but their stride wasn't logged.
- **Cause so far:** `Publish2dDraws` (`skate3_native_scene.cpp`) handles only
  16, 20, 24 and 28-byte vertex layouts. Any other stride is counted in
  `g_draws_2d_other` and dropped. The 32-byte layout isn't decoded yet; it
  could be pos4 + color + uv + one more field, or pos4 + uv + uv2.
- **Emulated (F5):** not checked.
- **Next step:** take an F10 capture right at event start. The `.draws.bin`
  records the 2D draws with their inline vertex data. Decode the 32-byte
  vertices, compare with F5, then add the layout to `Publish2dDraws`.

### KI-38: Frames-in-flight limit may skip frames only the native renderer draws

- **Status:** open (unconfirmed) · **Severity:** low–medium
- **Area:** SDK command processor, frame pacing
- **Tracking:** none yet. Found while fixing KI-16 (#28).
- **Where:** rexglue-sdk `src/graphics/d3d12/command_processor.cpp`,
  `EndSubmission`. It sets `is_closing_frame = is_swap && frame_open_`, and
  only a closed frame enters the `kQueueFrames` = 3 wait in
  `BeginSubmission`. The Vulkan command processor has the same pattern with
  `kMaxFramesInFlight` = 3.
- **Problem:** if the native frame's `EndSubmission(true)` runs with no frame
  open (for example with guest draws suppressed), nothing limits how far the
  GPU falls behind. Input latency would grow and the upload rings would run
  out of free regions. The #28 log line "upload ring <name>: all N regions in
  flight" would then appear.
- **Evidence:** the code path only, not checked at runtime. The bone-ring
  comment in `skate3_native_scene_gpu_internal.h` records the Vulkan GPU
  running more than 3 frames behind, which would fit.
- **Next step:** log `frame_open_` at the native `EndSubmission(true)` on both
  backends during gameplay. If it is false, open the frame in
  `NativeRhiBeginFrame`.

### KI-39: The native RHI has no way to wait on a GPU submission

- **Status:** open · **Severity:** low
- **Area:** SDK native RHI
- **Tracking:** none yet. Found while fixing KI-16 (#28).
- **Where:** rexglue-sdk `include/rex/graphics/native_rhi.h`. `nrhi::Device`
  has `CurrentSubmission()` and `CompletedSubmission()` but nothing that
  blocks.
- **Problem:** game code can't wait for the GPU. `CompletedSubmission()` is a
  cached value the command processor refreshes in `BeginSubmission`, so
  spinning on it on the render thread never advances. #28 therefore reuses
  the oldest region and logs, instead of waiting.
- **Next step:** add `Device::WaitForSubmission(uint64_t)`, backed by
  `CheckSubmissionFence` / `CheckSubmissionFenceAndDeviceLoss`. That needs an
  SDK-fork PR plus an "sdk: bump" commit. `UploadRingRegions::Acquire` can
  then wait, with a rate-limited log, instead of racing.

### KI-40: Photo FX constant slots have no bounds check

- **Status:** open · **Severity:** low
- **Area:** native renderer, photo FX
- **Tracking:** none yet. Found while fixing KI-16 (#28).
- **Where:** `src/skate3_native_scene_gpu.cpp`, `RenderScene`, the photo FX
  `fill_cb` lambda.
- **Problem:** `cb_slot_next++` is never checked against
  `RendererState::kPfxCbSlots` (10). Today at most 10 slots are used: depth
  pack, accum feed, 6 passes, accum mode 2 and the debug view. One more pass
  would silently write into the next frame's region, or past the end of the
  buffer for the last region.
- **Fix:** clamp it, or assert and log when `cb_slot_next` reaches
  `kPfxCbSlots`.

### KI-41: Shadow-pass textures can be evicted while still in use

- **Status:** open (unconfirmed) · **Severity:** low–medium
- **Area:** native renderer, shadows, texture cache
- **Tracking:** none yet. Found while fixing KI-17 (#29).
- **Where:** `LookupResolvedTexture` in `src/skate3_native_scene_gpu.cpp`,
  called by `RenderShadowAtlas` for alpha-clip casters (`item.dynobj == 2`).
  It is also called by `RenderStaticSunMap` for phase-1 `rec.diffuse_tex`,
  but that pass is experimental since KI-5.
- **Problem:** it reads the `tex_store` entry but never stamps its
  `last_used_frame`. `EvictTexStore` picks eviction candidates by
  `last_used_frame` alone, with a 5 s idle guard under byte pressure. A
  texture used only by the shadow passes, such as an off-screen or culled
  caster, looks idle and can be evicted. The caster passes never enqueue a
  decode, so it stays gone until the main pass draws that item again.
- **How it would show:** under texture-memory pressure, alpha-tested shadows
  (foliage, fences) become solid shapes for casters outside the view.
- **Evidence:** code reading only, not reproduced. #29 stamps the route in
  that function but deliberately leaves the store entry alone.
- **Next step:** after #29 lands, also set
  `sit->second.last_used_frame = frame_number` there (#29 passes
  `frame_number` in). Test with a small
  `skate3_native_render_scene_tex_store_mb` near foliage and compare shadow
  shapes with F5.

### KI-42: Perf log line repeats the texture count

- **Status:** open · **Severity:** low
- **Area:** native renderer, diagnostics
- **Tracking:** none yet. Found while fixing KI-17 (#29).
- **Where:** `LogFrameStats` in `src/skate3_native_scene_gpu.cpp`, the
  `native-scene: frame N ...` line.
- **Problem:** `g_r.tex_store.size()` is logged three times: as
  `2d[... textures=]`, as `textures=` and as `store[n=]`. The 2D
  `textures=` field looks like a leftover from a separate 2D cache that no
  longer exists, so it misleads.
- **Fix:** drop the 2D `textures=` field, or make it count something
  2D-specific.

### KI-43: A script/debug getter reads the scaled cull threshold

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** draw distance
- **Tracking:** none yet. Found while fixing KI-18 (#30).
- **Where:** `sub_825B5730` (`generated/skate3_recomp.29.cpp`). It has no
  direct callers and is reached only through a registered function pointer.
  It reads the global render context's cull object
  (`[[0x820CFD94]+292]+6064`) and returns
  `clamp(int((threshold - c0) * c1), 0, 10)` through `sub_82E86550`.
- **Problem:** it sees the draw-distance scaled value (0.2 becomes 0.05), so
  any script or menu that reads this level gets a shifted number. If a
  matching setter writes a level back through a path
  `src/skate3_draw_distance.cpp` doesn't hook, the scaled value becomes the
  new base and the scale compounds.
- **Next step:** find who registers or calls `sub_825B5730`, and any setter for
  that level, then watch it during a test.

### KI-44: Live draw-distance changes skip setter-only cull objects

- **Status:** open · **Severity:** low
- **Area:** draw distance
- **Tracking:** none yet. Found while fixing KI-18 (#30).
- **Where:** `src/skate3_draw_distance.cpp`.
- **Problem:** a hot change of `skate3_draw_distance_scale` reaches only the
  `view+8` cull objects, which are rescaled every frame. Objects seen only
  through the setter hook keep the old scale until the next config apply.
  Low impact: no setter calls showed up in a boot to gameplay.
- **Next step:** when the cvar value changes, rescale every load-bearing slot
  from the per-frame hook.

### KI-45: XMA context input offset exceeds its buffer

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** audio (SDK APU)
- **Tracking:** none yet. Seen in a normal boot log while testing KI-18 (#30).
- **Symptom:** the log repeats
  `[apu] XmaContext 31 old: input offset 16416 exceeds buffer size 16384`.
  Whether any sound is affected is unknown.
- **Next step:** find the check in the rexglue SDK's XMA decoder and which
  game sound uses context 31. Compare with upstream rexglue-sdk, then listen
  for a missing or clipped sound at the moment the line is logged.

### KI-46: Long user-data paths silently drop a VFS overlay alias

- **Status:** open · **Severity:** low–medium
- **Area:** app startup, file system overlay
- **Tracking:** none yet. Found while fixing KI-19 (#31).
- **Where:** `src/skate3_app_common.cpp`, `Skate3BaseApp::InstallRecipeOverlay`
  / `CreateOverlayDirectory`.
- **Problem:** with a long user-data path, the overlay folder
  `<cache_root>\vfs_big_directory_aliases\data\state\livingworldentities\pedestrian\plugin`
  passes CreateDirectoryW's 248-character limit and is never created. The
  failure is only counted, never logged. A user with a long AppData or
  portable path would silently lose the pedestrian plugin alias.
- **Evidence:** a test run from a long scratch path logged "overlay with 10
  aliases"; the same build from a short path logged 11.
- **Next step:** log which alias failed. Then use `\\?\` long paths or a
  shorter overlay root.

### KI-47: Other startup filesystem calls can throw

- **Status:** open · **Severity:** low
- **Area:** app startup
- **Tracking:** none yet. Found while fixing KI-19 (#31). Code reading only,
  untested.
- **Where:**
  - `src/skate3_app_common.cpp`: `std::filesystem::exists` at the
    portable.txt check, the config/settings paths, `FirstExistingFontPath`,
    the JP font path, and the profiles/config checks in the defaults setup.
    Also `is_directory(defaults.game_data_root)`, and
    `is_directory(portable_saves)` in the post-launch setup.
  - `src/skate3_user_settings.cpp`: `exists(profiles_path)`.
  - `src/skate3_screenshot.cpp`, `ScreenshotPath`: `absolute(...)`.
- **Problem:** the same failure as KI-19. A permissions error on one of these
  paths becomes an uncaught exception.
- **Fix:** one sweep to the `error_code` overloads.

### KI-48: Guest-output view can go stale after an RHI mailbox eviction

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** native renderer, SDK native RHI
- **Tracking:** none yet. Found while fixing KI-20 (#32).
- **Where:** `EnsureOutputSrv` in `src/skate3_native_scene_gpu.cpp` (added in
  #32), and the SDK's `guest_outputs_` map in `native_rhi_d3d12.cpp` and
  `native_rhi_vulkan.cpp`.
- **Problem:** once the map holds more than 6 entries (after several resizes),
  the RHI deletes old texture wrappers and allocates new ones with no serial.
  A new wrapper at the same address and size, for example after resizing back
  to an earlier size, still matches the pointer-plus-size key, so the view
  keeps pointing at the old texture.
- **Evidence:** code reading only, not reproduced.
- **Next step:** give `nrhi::Texture` a creation serial in the SDK (SDK-fork PR
  plus an "sdk: bump" commit) and key the view on it.

### KI-49: Guest-output view may be rebuilt every frame

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** native renderer, performance
- **Tracking:** none yet. Found while fixing KI-20 (#32).
- **Where:** `EnsureOutputSrv` in `src/skate3_native_scene_gpu.cpp`.
- **Problem:** the SDK comments describe a "small guest-output mailbox" in the
  presenter. If it rotates images, `context.guest_output` changes every frame,
  so the view is destroyed and recreated every frame.
- **Next step:** log how many times per second the view is rebuilt. If it's
  every frame, keep one view per mailbox image.

### KI-50: Photo FX depth view isn't checked for null

- **Status:** open · **Severity:** low
- **Area:** native renderer, photo FX
- **Tracking:** none yet. Found while fixing KI-20 (#32).
- **Where:** `RenderScene` in `src/skate3_native_scene_gpu.cpp`.
- **Problem:** the result of `CreateTextureView` for `pfx_srv[7]` is passed to
  `pfx_bind_all(..., g_r.pfx_srv[7])` without a null check. A failed create
  binds null for that frame, and it retries the next frame.
- **Fix:** fall back to `g_r.white.srv`, or skip the depth pack when the view
  is null.

### KI-51: Occlusion grid can miss thin gaps at high render scales

- **Status:** open (unconfirmed) · **Severity:** low–medium
- **Area:** native renderer, occlusion culling
- **Tracking:** none yet. Found while fixing KI-21 (#33).
- **Where:** `ps_depth_max` in `src/native/shaders/ssao.hlsl`.
- **Problem:** the grid reduce takes only 8x8 point samples per tile. Its
  comment accepts that a gap under 3 px can be missed at 4K because "the
  consumer is telemetry, not culling". That's no longer true: the occlusion
  cull reads this grid, so a missed gap can hide a distant object that is
  visible through railings or fence slats. The 1 m depth margin doesn't help
  with gaps. At `resolution_scale = 3` (the user's setting), tiles reach
  about 24 px and the samples are about 3 px apart.
- **Next step:** sample every texel of the tile's footprint, or reduce through
  a two-level chain, and update the comment. Regenerate the SPIR-V. Test by
  looking through fences and railings at render scale 3x, with the occlusion
  cull on and then off.

### KI-52: Demo-path map-switch inputs may not work

- **Status:** open (unconfirmed) · **Severity:** low
- **Area:** test tooling
- **Tracking:** none yet. Found while testing KI-21 (#33).
- **Where:** the input sequence in the help text of
  `skate3_demo_path_gameplay_inputs` (`src/skate3_demo_path.cpp`).
- **Problem:** in a signed-out demo boot all 14 inputs were injected, but
  nothing loaded and the item counts stayed the same, so the documented
  map-switch sequence seems to do nothing. Unattended tests that rely on it
  would silently stay in one area.
- **Next step:** check whether it needs `--skate3_demo_path_signed_in=true`, or
  whether the menu layout changed. Then fix the sequence or its help text.

### KI-53: Render targets used before their first clear (D3D12 debug layer)

- **Status:** open · **Severity:** low
- **Area:** native renderer, shadows and startup
- **Tracking:** none yet. Found while fixing KI-22 (#34).
- **Where:** `EnsureShadowResources` / `RenderShadowAtlas` in
  `src/skate3_native_scene_gpu.cpp`, plus unidentified resources at boot.
- **Problem:** the D3D12 debug layer reports
  `RENDER_TARGET_OR_DEPTH_STENCIL_RESOUCE_NOT_INITIALIZED` (#1422) twice after
  every shadow atlas recreation. That fits `shadow_mid` and `shadow_final`:
  the blur pass draws into them, but only `shadow_raw` is ever cleared. It's
  probably harmless, since the blur writes every texel of each tile. The same
  error also appears 4 times at boot and 17 times when gameplay starts, from
  resources not yet identified.
- **Next step:** clear or discard `shadow_mid` and `shadow_final` once after
  creation. Then run with the debug layer and find the boot-time ones (the
  KI-22 agent's `dbwin.ps1` captures debug-layer output without a debugger).

### KI-54: Shadow-dump readback buffers are never freed (debug only)

- **Status:** open · **Severity:** low
- **Area:** native renderer, diagnostics
- **Tracking:** none yet. Found while fixing KI-22 (#34).
- **Where:** `RenderShadowAtlas` in `src/skate3_native_scene_gpu.cpp`.
- **Problems:**
  - After one shadow recording, the four readback buffers stay allocated for
    the rest of the session. At tile 4096 that is about 400 MB each, about
    1.6 GB in all. The only release is the size-mismatch path #34 added.
  - Dump file names use a seconds timestamp plus the frame index, so two
    recordings in the same second overwrite each other.
  - A recording usually yields 3 dumps, not `kShadowDumpFrames` = 4, because
    the recording window is shorter than 4 shadow frames.
- **Next step:** release the buffers once a session's dumps are written, and
  add a counter to the file name.

### KI-55: App callbacks that capture `this` are never cleared on shutdown

- **Status:** open · **Severity:** low
- **Area:** app lifetime
- **Tracking:** none yet. Found while fixing KI-23 (#35).
- **Where:** `OnPostSetup` / `OnShutdown` in `src/skate3_app_common.cpp`.
- **Problem:** six callbacks capture `this` and are never cleared:
  `input_system->SetActiveCallback`, `SetMenuChordCallback`,
  `demo_path::SetUiInputProvider` and the three `options_menu::Set*Callback`.
  The active callback also reads `simple_settings_dialog_` on the input path
  while `OnShutdown` resets it on the UI thread. That's a data race or
  use-after-free on any path where `OnShutdown` runs while the guest is
  still alive. Today that is mainly Close Game on macOS (`QuitFromUIThread`,
  which ends the guest after `OnShutdown`).
- **Fix:** clear these callbacks (null or no-op) at the top of `OnShutdown`.

### KI-56: SDK key binds: duplicate registration and callbacks under the lock

- **Status:** open · **Severity:** low
- **Area:** SDK input
- **Tracking:** none yet. Found while fixing KI-23 (#35).
- **Where:** `RegisterBind` / `UnregisterBind` / `ProcessKeyEvent` in
  rexglue-sdk `src/ui/keybinds.cpp`.
- **Problem:** re-registering a name appends a duplicate entry and registers
  the cvar again, while `UnregisterBind` only nulls the first match. Bind
  callbacks also run under the non-recursive `g_binds_mutex`, so a bind that
  registers or unregisters a bind would deadlock. Neither happens today,
  because `RestartGame` starts a new process instead of re-registering.
- **Next step:** make `RegisterBind` replace by name, and call callbacks
  outside the lock. That needs an SDK-fork PR plus an "sdk: bump" commit.

### KI-57: SDK `fullscreen` change callback is never unregistered

- **Status:** open · **Severity:** low
- **Area:** SDK app lifetime
- **Tracking:** none yet. Found while fixing KI-23 (#35).
- **Where:** rexglue-sdk `src/ui/rex_app.cpp`. `SetupPresentation` registers a
  `fullscreen` change callback that captures `this`, and `OnDestroy` only
  unregisters `show_fps_counter`.
- **Problem:** a `fullscreen` change after the app object is destroyed would
  call into it. Unlikely, since the process exits right after.
- **Fix:** add `UnregisterChangeCallbacks("fullscreen")` to `OnDestroy`.

### KI-58: SDK `ParseXuid` misreads decimal XUIDs

- **Status:** open · **Severity:** low
- **Area:** SDK user profile
- **Tracking:** none yet. Found while fixing KI-24 (#36).
- **Where:** `ParseXuid` in rexglue-sdk `src/system/xam/user_profile.cpp`, used
  by `UserProfile::UserProfile`.
- **Problem:** it's a copy of the game's old parser and still tries hex
  first, so a decimal `--user_profile_xuid=12345` passed by hand becomes XUID
  0x12345. The game itself always passes hex from `FormatXuid`, so normal
  play is unaffected.
- **Fix:** apply #36's rules in the SDK fork (a `0x` prefix or 16 hex digits
  is hex; other all-digit strings are decimal), or have the SDK call one
  shared parser.

### KI-59: A malformed profiles.toml silently wipes all profiles

- **Status:** open · **Severity:** low–medium
- **Area:** user settings, profiles
- **Tracking:** none yet. Found while fixing KI-24 (#36).
- **Where:** `LoadProfiles` in `src/skate3_user_settings.cpp`, and the
  `save_profile` lambda in `src/skate3_app_common.cpp`.
- **Problem:** `LoadProfiles` catches `toml::parse_error`, returns an empty
  store and logs nothing. The next profile save then calls
  `EnsureUsableProfileStore`, which adds a default "Player" profile, and
  overwrites the user's file with it. Any malformed `profiles.toml` (a hand
  edit, invalid UTF-8, see KI-60) destroys every profile on the next save.
- **Next step:** log the parse error, and back up or refuse to overwrite a file
  that didn't parse.

### KI-60: Gamertag truncation can produce invalid UTF-8

- **Status:** open · **Severity:** low
- **Area:** SDK settings overlay, profiles
- **Tracking:** none yet. Found while fixing KI-24 (#36).
- **Where:** `CopyToBuffer` in rexglue-sdk
  `src/ui/overlay/simple_settings_overlay.cpp`.
- **Problem:** the gamertag is copied into `gamertag_buf_[32]` byte by byte,
  which can cut a multi-byte UTF-8 character in half. `SaveProfile` then
  writes invalid UTF-8, toml++ rejects the file, and KI-59 wipes the
  profiles. It needs a gamertag over 31 bytes with a non-ASCII character at
  the cut, so in practice only hand-edited names hit it.
- **Fix:** truncate at a UTF-8 character boundary.

## Resolved

### KI-1: Texture pop-in and flicker while streaming

- **Status:** fixed in rubensteezy/skate3recomp#3 (README entry removed in #5)
- **Symptom:** streamed surfaces briefly showed solid colours or other texture
  layers.
- **Fix:** in-place texture payload changes are confirmed before the texture
  is decoded again. Confirmed in-game on 2026-10-12.

### KI-2: Skater rendering in Edit Skater

- **Status:** fixed in rubensteezy/skate3recomp#4, #6 and #8
- **Symptoms:**
  - After skin-tone edits, the native skater showed red patterned arms,
    see-through eyes and the wrong hair for a few seconds (#4).
  - The skater's hair in Edit Skater looked darker than on the emulated
    renderer (#6 and #8).
- **Fix:**
  - #4 keys the texture hold and sticky sites by piece signature, so rebuilt
    pieces no longer pick up each other's textures.
  - #6 ports the `cac_hair` Kajiya-Kay strand highlight.
  - #8 shades `cac_hair` with the world sun shadow.

### KI-4: Bruises and dirt don't show on the player character

- **Status:** fixed in rubensteezy/skate3recomp#20. Bruises and dirt confirmed
  in-game on 2026-10-12. Tattoos were split out as KI-6.
- **Symptom:** bruising and dirt didn't appear on the skater on the native
  renderer. The emulated renderer showed them.
- **Cause:** the CAC pixel shaders (`cacstamp_skin`, `cac_face`,
  `cacstamp_cloth`, `cac_cloth`, `cacstamp_shift`) blend up to two extra layers
  over the tinted diffuse before lighting:
  - `i_decal` is the tattoo stamp, placed with the vertex shader's
    `i_customGraphic` affine of texcoord 1.
  - `i_decal2` is bruise/dirt at uv0, scaled by `i_params.z` on skin or `.y`
    on cloth.

  The native fam-2 branch only drew the diffuse.
- **Fix:** `CaptureCacDecals` captures the layers' fetch words, the affine and
  the amounts into `char_rows`. The renderer binds them at t8/t9, and
  `scene_char.hlsli` composites them. Cvar:
  `skate3_native_render_scene_char_decals`.

### KI-5: Enhanced shadows show artifacts

- **Status:** won't fix (shelved) in rubensteezy/skate3recomp#21. The cause
  was never found. The options were removed instead.
- **Symptom:** with Enhanced Shadows (the native static sun-shadow map) and
  PCSS on, shadows looked blocky or the wrong shape, not better than stock.
  Repro: D3D12, render scale 3x, MSAA 4x/8x.
- **What #21 did:** removed the options from Settings > Video, Options >
  Graphics and F12 MAX QUALITY, and turned them off by default. The code is
  kept behind `--skate3_native_render_scene_experimental_static_shadows=true`
  and `--skate3_native_render_scene_experimental_pcss=true`.
- **Ruled out:** the static caster cache holding `proxyworld.default`
  stand-in meshes. Dropping them made no visible difference.
- **Main suspect if revisited:** the map casts along the material sun (c6),
  which is about 43 degrees off the game's own stylized shadow axis and may
  disagree with the baked lightmaps.

### KI-7: Dev builds hang on the splash screen with the native renderer on at boot

- **Status:** could not reproduce on `main` (c152af0). No code change. Reopen
  if it happens again, and save the log right away, because only the last 5
  logs are kept.
- **Report:** one dev run on 2026-10-12 (D3D12, KI-5's experimental build,
  33cd9b7 plus an uncommitted shadow change). After language select the intro
  video never started. Frames kept running but slowed, and the window was
  closed after a minute. That log has since rotated away.
- **Misread evidence:** the main-checkout logs that seemed to show the hang
  (`skate3_014`..`018`, c8ba8ac) were shader-disassembly runs. They are killed
  on purpose about 2 s after launch, which is why they stop with no mem lines
  and no "Title terminated" line. `skate3_019`, from the same build with the
  scene on from boot, booted normally. No Vulkan hang was ever seen.
- **Checked:** 20 automated boots of `main` with the native renderer on from
  boot, on D3D12 and Vulkan. That includes 8 with `--skate3_demo_path=true`,
  which injects a pad A at language select. All of them reached the intro
  video or gameplay. The user then booted it with a controller on both
  backends and reached the menus. Notes: KI-7-splash-hang.md in the session
  notes folder.

### KI-8: ISO installer path traversal and partial installs

- **Status:** fixed in rubensteezy/skate3recomp#17, tested in-game · was BT-01
- **Problem:** `IsUnsafeIsoPath` only split on `/`, so `\`, drive letters and
  NTFS alternate data streams could escape the install folder on Windows.
  `IsGameInstalled` only checked for `default.xex`, so an interrupted
  extraction counted as installed.
- **Fix:** paths are checked on both separators and rejected for drive
  letters, `:`, control bytes and dot-only components, before anything is
  written. A `.skate3_install_incomplete` marker sits in the game root for
  the whole extraction (`src/skate3_iso_installer.cpp`).

### KI-9: Freecam kept the guest camera after F5 + freecam off

- **Status:** fixed in rubensteezy/skate3recomp#17, tested in-game · was BT-02
- **Problem:** `g_freecam_guest_active` was only cleared in `UpdateFreecam`,
  which `BuildFrameScene` skips on its early returns (scene off, no perspective
  view), so the guest camera rewrite and draw-distance recentring stayed on.
- **Fix:** `ReleaseFreecamIfOff` runs at the top of every `BuildFrameScene`.

### KI-10: Freecam needed smooth_camera to take over the guest camera

- **Status:** fixed in rubensteezy/skate3recomp#17, tested in-game · was BT-03
- **Problem:** `g_sampler_viewcam` was only stored inside the smooth_camera
  block, so with smoothing off the SetViewMatrix override never matched and
  the game culled around the real camera.
- **Fix:** it is stored every frame; only starting the sampler thread still
  depends on smoothing.

### KI-11: SDK cvar overrides stuck after F5; overlapping save/restore

- **Status:** fixed in rubensteezy/skate3recomp#17, tested in-game · was BT-04
- **Problem:** the menu yield, photo-grab window and photo-display yield only
  restored their SDK cvar overrides from RenderScene's normal path, which is
  skipped when the scene is off. Overlapping windows on the same cvar saved
  each other's forced values as the user's.
- **Fix:** one owner-tracked `SdkCvarOverride` per cvar
  (`src/skate3_native_scene_gpu.cpp`); `RestoreAllYieldOverrides()` runs on
  RenderScene's scene-off return.
- **Not tested:** re-taking the overrides after F5 back to native.

### KI-12: MAX QUALITY values written to settings.toml

- **Status:** fixed in rubensteezy/skate3recomp#17, tested in-game · was BT-05
- **Problem:** any settings save while F12 MAX QUALITY was on wrote its maxed
  values (MSAA, shadows, draw distance and so on) into `settings.toml`.
- **Fix:** saves run inside `WithMaxQualitySuspended`, and the SDK settings
  overlay suspends the mode while it is open.

### KI-13: Shader classifier caches keyed by recycled guest addresses

- **Status:** fixed in rubensteezy/skate3recomp#19, tested in-game · was BT-06
- **Problem:** seven classifiers cached their shader debug-path verdict by
  object address forever, but the game recycles shader objects while
  streaming. The env-receiver cache could let an unrelated shader win the
  receiver-row capture, the same failure as the flowingwater dark-world bug.
- **Fix:** `ShaderVerdictMemo` (`src/skate3_native_scene.cpp`) trusts a
  verdict only within the guest frame that computed it.
- **Remaining gap:** a shader recycled within a single frame keeps the old
  verdict until the next frame.

### KI-14: Skinned palette capture read past the VS constant bank

- **Status:** fixed in rubensteezy/skate3recomp#19, tested in-game · was BT-07
- **Problem:** `CaptureSkinnedState` copied 84 bones from palette bases up to
  c8, reading up to 4 registers past c255.
- **Fix:** only whole bones inside c0..c255 are read; the rest are zero.

### KI-15: Publish-gap tracker swapped a clone's capture for its twin's

- **Status:** fixed in rubensteezy/skate3recomp#19, tested in-game · was BT-08
- **Problem:** the `BuildFrameScene` publish-gap tracker was keyed by mesh, so
  a cloned caster-bank instance was replaced by its twin's same-frame capture
  and drawn at the twin's pose.
- **Fix:** the tracker handles single-instance meshes only.
- **Trade-off:** cloned characters no longer get the caster-bank swap or the
  2-frame gap fill. A `(mesh, ctx)` key could restore both once `ctx` is
  confirmed stable across frames.

### KI-33: Hook addresses not gated on Title Update 3

- **Status:** fixed in rubensteezy/skate3recomp#27, tested in-game · was BT-26
- **Problem:** the game hooks use hard-coded guest addresses that are only
  correct in the Title Update 3 image, but the build still allowed codegen
  from the unpatched retail image. Release builds were never affected: they
  always use TU3.
- **Checked:** base-XEX codegen (retail `default.xex`,
  `config/skate3_functions.toml`) against the TU3 `generated/`, comparing each
  function's instruction stream with branch targets masked. TU3 shifts most
  code by about 0x24000 to 0x2B000 (for example the front-end state setter is
  `0x82CDFFB8` in retail, `0x82D0AFA0` in TU3). `src/` hooks or calls 90
  TU3 function addresses. 88 are not function starts in retail. The other
  two, `sub_82782818` and `sub_82B78FF0`, are different functions there,
  which is the silent wrong-hook case. The options menu, heap diagnostics and
  exception guard were already gated. The other 61 addresses were not: the
  native renderer, scene, draw-distance and demo-path hooks. Guest data
  addresses (globals, vtables) were not diffed. They sit behind the same
  gate now.
  The demo-path intro-movie codegen patch (`ApplySkate3CodegenPatches.cmake`)
  also anchors on TU3 addresses. The ultrawide frustum and FOV patches match
  instruction patterns instead.
- **Effect:** a retail build failed to link with
  `undefined reference to __imp__sub_...`, because each hook calls the
  original through a symbol the retail codegen doesn't emit. A reconfigure
  that lost the game root (and with it the TU3 fallback) produced the same
  error.
- **Fix:** configure now stops with an error that says how to provide TU3
  when no TU3 codegen input is found. A `static_assert` on
  `SKATE3_HAS_TITLE_UPDATE` at the top of each ungated hook block marks the
  addresses as TU3-only.
- **Leftover:** the retail-only branches (`#if !SKATE3_HAS_TITLE_UPDATE` in
  `src/skate3_app_common.cpp` and `src/exception_compat.cpp`, the
  retail `config/skate3_functions.toml`) are now unreachable.

### KI-34: Own the Spot / Own the Lot UI pops in and out or draws wrong

- **Status:** fixed in rubensteezy/skate3recomp#25. Confirmed in-game on
  D3D12 and Vulkan on 2026-10-12.
- **Symptom:** during Own the Lot (and Own the Spot) events the score and
  timer, objective text, challenge list and spot markers flickered in and out
  or drew incompletely. Native renderer only: the cause is in the native
  renderer's own vertex upload, so the emulated renderer (F5) is not affected.
- **Cause:** the in-world spline pass (the neon boundary/marker strips) and the
  HUD/2D pass share one per-frame upload region for their vertices, splines
  first. An Own the Lot boundary is up to 54 strips, about 1.03 MB, and the
  region was 1 MB. With the whole outline in view the spline pass filled it,
  so its last strip and every HUD draw after it were skipped without a log
  line (7 HUD draws skipped per frame in a measured run). The camera moving
  the outline in and out of view made the HUD pop in and out.
- **Fix:** the region (`RendererState::kUiRegionSize`,
  `skate3_native_scene_gpu_internal.h`) is now 4 MB, and the spline pass in
  `RenderScene` stops at `kUiSplineBudget`, 1 MB short of the end, so the HUD
  always has room. A skipped spline or 2D draw now logs a rolling-capped
  "UI vertex region full" warning.

### KI-35: Team-member portraits in character creation show old photos

- **Status:** fixed in rubensteezy/skate3recomp#26. Confirmed in-game on D3D12 and Vulkan on
  2026-10-12.
- **Symptom:** in Pause > Create Skater, the boxes of the extra team members
  showed photos from earlier events instead of each skater's portrait, a
  different old photo per member. The emulated renderer (F5) showed the same
  images because the portrait is saved with the team member. The bad members
  came from the career "new team member" prompt on the native renderer
  (v2.1.0).
- **Cause:** `SkaterPresentationPhoto` (request `sub_8278EE78`, render and
  extract step `sub_8278E960`) renders the skater into the postfx manager's
  1152x640 screenshot target, and `PhotoEffects::GeneratePhoto` CPU-reads that
  resolve from guest memory at 0x04911000 into the 128x256 portrait. That's the
  same surface the photo grab fills. On the native renderer the resolve only
  reached CPU memory when the pause-menu portrait window (suppress mode 3,
  picked by FE-stack heuristics in `YieldForMenus`) and the SDK's targeted
  0x04911000 copy (non-gameplay contexts only) both applied. The career prompt
  fell outside them, so the portrait was cut from whatever the last photo grab
  left there. Reproduced on main with
  `--skate3_native_render_scene_menu_rtt_passes=false`: take a photo, edit a
  member, and its portrait becomes that photo.
- **Fix:** hooks on the request and the step (`skate3_native_render.cpp`)
  drive `UpdateSkaterPhotoWindow` (`skate3_native_scene_gpu.cpp`). From the
  request until the step reports done, it holds suppress mode 3, synchronous
  shaders and the forced readback of resolves up to 0x2D0000, in any presence
  context. The native photo grab also stops writing 0x04911000 for that time.
  The log shows "skater photo window OPEN" and "skater photo generated (...
  window armed)". Existing bad portraits heal by editing the member once.
- **Emulated (F5), not fixed here:** in a gameplay presence context with
  `readback_resolve=none`, the emulated renderer would read the same stale
  memory (the SDK's targeted copy skips gameplay contexts). Not seen in play.

### Reviewed and ruled out (code review)

- Ultrawide target aspect persistence: not in the saved settings list.
- `BuildItemFromMesh` initialisation: items are always fresh.
- Photo FX MSAA latch: handled in `RenderScene`.
- Null hair PSOs: checked at the point of use.
