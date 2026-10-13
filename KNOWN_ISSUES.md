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

### KI-35: Team-member portraits in character creation show old photos

- **Status:** investigating
- **Area:** menus, skater portraits
- **Tracking:** fix session `team-portraits`
- **Symptom:** in the character creation menu, the photos of the additional
  team-member skaters show images taken earlier in the game, from earlier
  events.
- **Cause so far:** unknown. Not yet checked whether the emulated renderer (F5)
  shows it. Likely suspect: the one-shot skater-portrait render-to-texture
  passes are skipped or never resolved, so the shared target keeps the last
  photo drawn into it. PR #17 recently changed the portrait/menu suppress-mode
  overrides.
- **Next step:** reproduce, compare with F5, and take an F10 capture on the bad
  screen.

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

### Reviewed and ruled out (code review)

- Ultrawide target aspect persistence: not in the saved settings list.
- `BuildItemFromMesh` initialisation: items are always fresh.
- Photo FX MSAA latch: handled in `RenderScene`.
- Null hair PSOs: checked at the point of use.
