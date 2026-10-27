# Changelog

Releases are tagged per game: `LegendsA-v*` (PL:A Recomp, branch `LegendsA`) and `LegendsZA-v*` (PL:Z-A Recomp,
branch `LegendsZA`). The Windows zips hold no game files and no game code.


## Unreleased (branch `LegendsA`)

Arceus:

- Time zone service: ToPosixTime(rule) and ToPosixTimeWithMyRule were mixed up (Z-A's save screen showed UTC); rules
  a game loads are now kept apart from the device's own.
- Screenshots: F12 (or a controller's capture / share button) saves the game image, without the overlay, as a PNG in
  the data folder's `screenshots` (`PLA_<date>_<time>.png`); a notice in the corner says so.
- Settings > In Background: keep playing, mute, or pause while the game's window isn't focused.
- Settings > Mouse Vertical / Mouse Horizontal: the mouse camera inverted (the game's own camera options act on the
  stick only).
- Controller rumble: the game's vibration values reach the controller through SDL (`LARECOMP_RUMBLE=0`: off). Not felt
  in tests yet.

- Shaders: condition-code branches (`FSET.CC` + `BRA` on a condition) are translated, as in Z-A 0.5.0 (there, the
  blotchy face and hair shadows). The intro looks the same as before (compared shot by shot).
- `R8_UINT` (and other unsigned integer) textures are read through an integer sampler: a float sampler on a UINT
  view is undefined in Vulkan. `LARECOMP_VK_UINT_TEX_FLOAT=1`: the old float sampler.

## 2026-10-26: LegendsA-v0.5.3-alpha

Arceus:

- Texture filtering: the game's own anisotropic filtering by default. The forced 16x kept the sea's wave texture sharp
  out to the horizon, where its tiles showed as a checkerboard (the intro's beach; Eden blurs it).
  `LARECOMP_ANISO=<n>` still raises it.
- The intro (start to Prelude Beach) was compared shot by shot with Eden: it matches apart from timing.
- 120 / 144 fps: a warning on the start screen and in the in-game Settings row (scripted scenes count frames).
- Research: frame dumps on request while running (`LARECOMP_LIVE`'s `debug` file), mip level dumps
  (`LARECOMP_DUMP_TEX_ADDR`).

## 2026-10-20: LegendsA-v0.2.0-alpha, LegendsZA-v0.1.0-alpha (first Windows test builds)

Both games:

- Windows builds, cross-built from Linux with zig; the start screen sets up the player's own dump and builds the
  game code on their PC.
- Frame rate options 30 / 60 / 120 / 144 fps; "Show FPS counter" option (the game's own frames per second and
  frame time).
- Names: PL:A Recomp and PL:Z-A Recomp (window titles, start screen).
- Graphics: texture fetch fixes (TLD / TLD4S / TLDS 2D arrays: cast shadows), depth textures sampled without compare
  when the shader reads raw depth.
- Shader warm-up on first start; VRAM vertex cache; batched submits.
- License and third-party notices for everything the Windows release ships (`THIRD-PARTY-NOTICES.md`).

Z-A:

- The game code builder skips modules the game doesn't have (Z-A has no subsdk): before this, setting up Z-A from
  the start screen failed.
- Indirect draws and the base instance of instanced draws: GPU-culled and instanced props (station facade, iron
  work, trees, hedges, lamp posts) render.
- NPCs animate every frame; UI animations at the game's speed above 30 fps.

## 2026-10-19: LegendsA-v0.1.0-alpha

First source snapshot of PL:A Recomp: boot, first-boot setup, character creation, the opening cutscenes, Prelude
Beach at 30 fps on Linux / Vulkan.
