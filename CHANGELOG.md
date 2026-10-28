# Changelog

Releases are tagged per game: `LegendsA-v*` (PL:A Recomp, branch `LegendsA`) and `LegendsZA-v*` (PL:Z-A Recomp,
branch `LegendsZA`). The Windows zips hold no game files and no game code.


## Unreleased (branch `LegendsA`)

Arceus:

- Fixed: the background shader warm-up could start using the GPU device while it was still being set up (it checked
  only that the device existed). Arceus under Wine crashed in its first shader on every start after a translator
  change; it now waits for the renderer to be ready (anything it reaches before that is translated without the GPU).
- Shader compiles on a first play ~33% quicker: a register's bits as a float, and a float result's bits, are now
  GLSL's own uintBitsToFloat / floatBitsToUint instead of overloaded helpers, which became real function calls in
  every shader's SPIR-V for the driver to inline (most of a shader's casts). SPIR-V 39% smaller (Z-A's shader cache on
  disk: 1.5 GB -> 0.94 GB); Z-A's whole warm-up with the driver's pipeline parts: 190 s instead of 284 s.
- Shaders with branches are translated as plain structured code (from Z-A): their blocks in order, loops as loops,
  jump tables as a switch, instead of one `switch (pc)` state machine in a loop around the whole program - the GPU's
  compiler can keep values in registers across branches and see the loops (on NVIDIA, Z-A's fragment shaders came out
  10% smaller with 3% fewer registers; AMD / Intel / mobile compilers should gain more). All of the game's ~8,600
  translated shaders; the intro's pictures identical. `LARECOMP_XLAT_NO_FWD=1`: as before. (One more shader warm-up.)
- Shaders reach the game's memory through its 64-bit GPU address (from Z-A: Vulkan buffer device address) instead of
  one storage buffer, which stops at 4 GB - and is smaller on some GPUs (ARM, integrated). The intro checked.
- Resolution: 540p (the settings and the start screen), for weak GPUs: about half the GPU time of 1080p.
- Start screen (from Z-A): this PC (GPU and its VRAM, CPU threads, RAM) and the settings recommended for it, with an
  Apply button; applied at once on a first start. Integrated GPU or under 4 GB: 720p / 30 fps; under 12 GB 1080p / 60;
  more 1440p / 60; Shader Loading "Don't Wait" with 8 or fewer CPU threads.
- Stutter (from Z-A): pipelines are linked from parts (Vulkan graphics pipeline libraries): each program's shaders are
  built once and every pipeline that uses them is a quick link. The intro with warm caches: 1 two-second span with a
  frame over 100 ms instead of 14. `LARECOMP_VK_GPL=0`: whole pipelines as before. On AMD (the Steam Deck) and Intel,
  each pipeline is also linked with link-time optimization on a worker and replaces the fast one when ready (a fast
  link runs slower shaders there; off on NVIDIA; `LARECOMP_VK_GPL_LTO=0/1`).
- The shader warm-up's translations are found in play (filed by what a shader reads, not by everything bound; 13,500
  translations checked identical). The new translator means one more warm-up at the next start.
- Screenshots are encoded on a thread of their own (each was a ~250 ms hitch).
- FPS counter: the GPU's time per frame too (from Z-A: two timestamps per submission; also in the log's stats lines).
- Settings > Shader Loading: Wait / Don't Wait (from Z-A; also on the start screen): Don't Wait draws on while a
  shader is built the first time and shows it a moment later, instead of a stutter. ini `shader_loading`.
- Start screen: In background, PKHeX Link and mouse invert, as in the game's Settings.
- PKHeX (from Z-A, the same runtime/hle/pkm_link.c): Settings > PKHeX Link (Off / This PC / Network) answers PKHeX's
  LiveHeX plugin like sys-botbase on a Switch (port 6000): it detects Arceus 1.1.1 and reads and writes the boxes
  while you play. Settings > Pokémon Files: A on Export saves every Pokémon in your boxes as a PKHeX .pa8 file
  (pokemon/export/<date>/), A on Import puts the .pa8 files in pokemon/import/ into the first empty box slots. Save
  in the game to keep them.

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
