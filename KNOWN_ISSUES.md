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
- For rendering bugs, always say whether the emulated renderer (F5) shows the
  same problem. A bug that only shows on the native renderer is a native
  renderer bug.

Capture recipe for native renderer bugs: launch with
`--skate3_native_render_capture_hotkeys=true`, switch to the native renderer
with F6, toggle F5/F6 to compare with the emulated renderer, then press F10.
That dumps the draws and a full guest-memory `.gsnap` into
`<cwd>/native_render_snapshots`.

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
