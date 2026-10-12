# Bug tracker

Known bugs and risks in the Skate 3 recomp, starting from the full code
review of October 2026. Each entry has a stable ID; reference it in commit
messages and PRs (for example `Fixes BT-09`) and update its row here in the
same PR that changes its status.

Locations name the file and function rather than line numbers, which drift.

**Status values:** `Open` · `Fixed (#PR)` · `Won't fix` (with a reason) ·
`Needs repro` (suspected, not yet confirmed).

**Severity:**
- **Medium:** wrong output, stuck state or memory safety in normal play.
- **Low–medium:** slow leaks, debug-only memory hazards, or features that silently stop working.
- **Low:** cleanup, edge cases, cosmetic.

## Summary

| ID | Sev | Status | Area | Summary |
|----|-----|--------|------|---------|
| BT-01 | Medium | Fixed (#17) | Installer | ISO path traversal on Windows; interrupted installs treated as complete |
| BT-02 | Medium | Fixed (#17) | Freecam | Guest camera not released after F5 + freecam off |
| BT-03 | Medium | Fixed (#17) | Freecam | Freecam needed smooth_camera to take over the guest camera |
| BT-04 | Medium | Fixed (#17) | Native GPU | SDK cvar overrides stuck after F5; overlapping save/restore |
| BT-05 | Medium | Fixed (#17) | Debug UI | MAX QUALITY values written to settings.toml |
| BT-06 | Medium | Fixed (#19) | Native scene | Shader classifier caches keyed by recycled guest addresses |
| BT-07 | Medium | Fixed (#19) | Native scene | Skinned palette capture read past the VS constant bank |
| BT-08 | Medium | Fixed (#19) | Native scene | Publish-gap tracker swapped a clone's capture for its twin's |
| BT-09 | Low–med | Open | Native GPU | Upload rings reused by frame index without a GPU fence check |
| BT-10 | Low–med | Open | Native GPU | Texture routing maps grow without bound |
| BT-11 | Low–med | Open | Draw distance | Fixed cull-threshold slots never freed; LOD distances scaled in place |
| BT-12 | Low–med | Open | App | DLC / recipe directory scans can throw at startup |
| BT-13 | Low–med | Open | Native GPU | Depth-SRV caches compare texture pointers (address reuse) |
| BT-14 | Low–med | Open | Native GPU | Occlusion cull silently inactive with SSAO off |
| BT-15 | Low–med | Open | Native GPU | Shadow-dump readback buffers not resized (debug only) |
| BT-16 | Low | Open | App | OnShutdown misses several UnregisterBind calls |
| BT-17 | Low | Open | Settings | ParseXuidString misreads decimal; TomlString doesn't escape control chars |
| BT-18 | Low | Open | Native scene | Vehicle retention frustum test ignores the ultrawide scale |
| BT-19 | Low | Open | Native GPU | ProcessPrewarmEntry does an unguarded raw read on a worker |
| BT-20 | Low | Open | Native GPU | DecodeMesh doesn't validate element offsets; some Map() results unchecked |
| BT-21 | Low | Open | Options menu | Modulo by an empty label list; g_restart_popup_open can go stale |
| BT-22 | Low | Open | Windows | Icon updater leaves `.icon-new` behind on a failed swap |
| BT-23 | Low | Open | Debug UI | F12 dialog rewrites the suppress cvar every frame (races the yield overrides) |
| BT-24 | Low | Open | Tests / CI | C++ tests not built, no CI, assert-only checks |
| BT-25 | Low | Open | Misc | Small items: screenshot names, blur_tex format, stale docs, missing includes |
| BT-26 | Medium? | Needs repro | Hooks | Demo-path / native hook addresses not gated on Title Update 3 |

## Fixed

**BT-01: ISO installer path traversal and partial installs** · Fixed in #17
`src/skate3_iso_installer.cpp`: `IsUnsafeIsoPath`, `IsGameInstalled`, `ExtractAll`.
Paths are checked on both separators and rejected for drive letters, NTFS
alternate data streams (`:`), control bytes and dot-only components. An
incomplete-install marker sits in the game root for the whole extraction.

**BT-02: freecam guest camera not released on early-out frames** · Fixed in #17
`src/skate3_native_scene.cpp`: `ReleaseFreecamIfOff` now runs at the top of
every `BuildFrameScene`.

**BT-03: freecam required smooth_camera** · Fixed in #17
`g_sampler_viewcam` is stored every frame; only starting the sampler thread
still depends on smoothing.

**BT-04: stuck / overlapping SDK cvar overrides** · Fixed in #17
`src/skate3_native_scene_gpu.cpp`: one owner-tracked `SdkCvarOverride` per
cvar; `RestoreAllYieldOverrides()` runs on RenderScene's scene-off return.
Not covered by testing: re-taking the overrides after F5 back to native.

**BT-05: MAX QUALITY leaking into saved settings** · Fixed in #17
`src/skate3_native_debug_dialog.cpp`, `src/skate3_app_common.cpp`: saves run
inside `WithMaxQualitySuspended`; the SDK settings overlay suspends the mode
while open.

**BT-06: shader classifier caches keyed by recycled addresses** · Fixed in #19
`src/skate3_native_scene.cpp`: `ShaderVerdictMemo` (trusted for one guest
frame) now backs `ClassifyPfxShader`, `IsCasEditorPs`,
`ClassifySplineShader`, and the env-receiver, world-family, hBlur and
outline-family classifiers in `OnDrawDone`.
Remaining gap: a shader recycled within a single frame keeps the old
verdict until the next frame.

**BT-07: skinned palette over-read** · Fixed in #19
`CaptureSkinnedState` reads only whole bones inside c0..c255; the remaining
bones are zero.

**BT-08: publish-gap clone swap** · Fixed in #19
The `BuildFrameScene` publish-gap tracker handles single-instance meshes only.
Trade-off: cloned characters no longer get the caster-bank swap or the gap
fill. A `(mesh, ctx)` key could restore both once `ctx` is confirmed stable
across frames.

## Open

**BT-09: upload rings reused without a GPU completion check**
`src/skate3_native_scene_gpu.cpp`, `src/skate3_native_scene_post.cpp`: several
upload and constant rings pick their slot with `frame_number % N`: the UI
ring, the shadow constant buffer, bone ring, photo FX CB slots (`% 4`) and
screen FX CB. None checks that the GPU has finished with the slot. The
comment above the ring sizes in `skate3_native_scene_gpu_internal.h` notes
that a slot was observed being recycled while still in use.
*Fix idea:* gate on `CompletedSubmission()` per slot, or size the rings from
the device's frames-in-flight.

**BT-10: unbounded texture routing maps**
`tex_routes`, `tex_sticky`, `words_sticky` and `tex_pending_first` are only
cleared by the debug cache flush.
*Fix idea:* prune by `last_used_frame` like `tex_store`.

**BT-11: draw-distance slots and in-place LOD scaling**
`src/skate3_draw_distance.cpp`: 16 fixed cull-threshold slots are never
freed, so scaling silently stops once enough cull objects have been seen.
LOD distances are multiplied in place every frame, which relies on the game
rewriting them each frame.

**BT-12: directory scans can throw at startup**
`src/skate3_app_common.cpp`: the DLC and recipe scans use the non-throwing
iterator constructor, but `operator++` can still throw.
*Fix:* use `increment(ec)`.

**BT-13: texture-view caches compare by pointer**
`s_pfx_depth_tex` (photo FX in `RenderScene`) and `ao_depth_srv_of`,
`ssr_depth_srv_of` and `vol_depth_srv_of` (`skate3_native_scene_post.cpp`)
detect a rebuilt depth texture by pointer. A texture freed and reallocated
at the same address keeps the old view, possibly with the wrong MSAA
dimension. The photo FX one is most exposed because it can sit idle across
several rebuilds.
*Fix idea:* a generation counter bumped on every depth rebuild.

**BT-14: occlusion cull depends on SSAO**
The occlusion grid is only produced inside `ApplySsaoPass`. With SSAO off,
the grid expires after 240 frames and the cull silently stays inactive.

**BT-15: shadow-dump readback buffers not resized** (debug only)
`src/skate3_native_scene_gpu.cpp`: the shadow dump readback buffers keep
their first size after a shadow tile change, so a later recording copies
out of bounds on the GPU.

**BT-16: OnShutdown misses UnregisterBind calls**
`src/skate3_app_common.cpp`: the screenshot (captures `this`), F5 toggle,
showcase and freecam binds are never unregistered.

**BT-17: user-settings parsing and escaping**
`src/skate3_user_settings.cpp`: `ParseXuidString` tries hex first, so it
misreads decimal XUIDs. `TomlString` does not escape control characters.

**BT-18: vehicle retention frustum test ignores the ultrawide scale**
`src/skate3_native_scene.cpp`: the vehicle retention test omits
`wide_hor_scale`, unlike the static retention test.

**BT-19: ProcessPrewarmEntry unguarded read**
`src/skate3_native_scene_gpu.cpp`: it does a guarded copy into `record` that
is never used, then a raw `REX_LOAD` on the worker thread.

**BT-20: DecodeMesh input validation**
Element offsets aren't checked against the vertex stride (a host over-read
on a malformed declaration), and several `Map()` results aren't checked for
null.

**BT-21: options-menu edge cases**
`src/skate3_options_menu.cpp`: `StepRow` takes `% count` with no guard for
an empty label list. `g_restart_popup_open` can be left set if the popup is
dismissed by a path other than its callback.

**BT-22: icon updater leftover file**
`src/skate3_win_icon.cpp`: a failed swap leaves `.icon-new` behind.

**BT-23: F12 dialog rewrites a cvar the render thread overrides**
`src/skate3_native_debug_dialog.cpp`: `native_render_suppress_emulated_draws`
is read and written back every UI frame, which can clobber the render
thread's temporary override (BT-04). Same pattern for other sliders the
render thread touches.
*Fix:* only `REXCVAR_SET` when the widget reports a change.

**BT-24: C++ tests not built, no CI**
`tests/*.cpp` (character fade, Hall of Meat, screen FX) aren't wired into
CMake, and they use `assert`, which does nothing in release builds. This
repository has no CI, so neither these nor the Python tests
(`python -m unittest discover -s tests`) run automatically.

**BT-25: small items**
- Screenshots taken within the same second get the same filename.
- `blur_tex` isn't recreated when the output format changes.
- The `kSceneRingFrames` comment says about 900, but the value is 2400.
- The header declaring `MaybeDumpSceneRing` is missing includes.

## Needs repro

**BT-26: hook addresses not TU-gated**
The demo-path and native hook addresses aren't conditioned on
`SKATE3_HAS_TITLE_UPDATE`. If any of them differ between the base XEX and
TU3, the hook lands on the wrong function in one of the two builds. Next
step: diff the hooked addresses between the base and TU3 codegen.

## Reviewed and ruled out

- Ultrawide target aspect persistence: not in the saved settings list.
- `BuildItemFromMesh` initialisation: items are always fresh.
- Photo FX MSAA latch: handled in `RenderScene`.
- Null hair PSOs: checked at the point of use.
