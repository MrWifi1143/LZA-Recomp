# Changelog

Releases are tagged per game: `LegendsA-v*` (PL:A Recomp, branch `LegendsA`) and `LegendsZA-v*` (PL:Z-A Recomp,
branch `LegendsZA`). The Windows zips hold no game files and no game code.


## Unreleased (branch `LegendsA`)

Arceus:

- Shaders build ~44% faster (the first start's shader warm-up, and new areas on a first visit): the translated
  GLSL now types each register and constant buffer as float or uint by how it is used, instead of uint everywhere
  with a conversion around every float operation, folds float constants, and leaves out helper functions a shader
  doesn't call. glslang over 1,613 of the game's shaders: 7.5 -> 4.2 ms each, none failing; the intro's pictures
  the same with and without. The cache is rebuilt once. `LARECOMP_XLAT_NO_RETYPE=1` turns it off.
- The on-screen keyboard is headed with the game's own text ("What is your name?") instead of always "Enter a name".
- A launcher, like an older PC game's (Fallout: New Vegas style), instead of the one-window start screen: the banner
  (Mount Coronet under the space-time rift, drawn by the program: no game art), Play, and pages for Options (Display /
  Graphics / Game tabs, with this PC's recommended settings), Controls (controller and keyboard), Game Files and Mods.
  It opens in a window; the game goes fullscreen (or stays a window) after Play. The old start screen is kept for
  console ports: `LARECOMP_START_SCREEN=classic`, or built with `-DRT_CLASSIC_START_SCREEN=1`.
- The start screen reads your dumped `.nsp` / `.xci` files directly, like the emulators: pick the folder with the base
  game and the update and the recomp applies the update over the base game itself, with your `prod.keys` (looked for
  beside the files, in `~/.switch` and in the emulators' key folders). No hac2l / hactoolnet step any more; extracted
  `exefs` / `romfs` folders still work. Written out: the game code and one RomFS image - checked byte for byte against
  hac2l's output (every file of both games).
- Button icons for Xbox / PlayStation / keyboard prompts are drawn once and kept on disk (cache/glyphs/): each redraw
  cost ~1.8 ms of the render thread (measured in Legends Z-A).
- Fewer hitches when new models appear: the VRAM vertex cache kept each stream's partial edge pages by reading them back
  from the write-combined upload buffer (~30 us a stream; 4.4 ms of one frame in Legends Z-A); they are taken from the
  game's memory now.
- Fewer hitches when new things appear (the render thread): a possible button icon (a small BC4 / BC5 texture) is put
  together in normal memory - it was hashed by reading back the write-combined upload buffer, ~0.1-0.3 ms per texture
  (up to 3.7 ms of a frame in Legends Z-A); redrawn icons are kept; block-linear textures are deswizzled a whole GOB at
  a time (1.8x faster, byte-identical over 4000 random layouts).
- Shadow Smoothing (Next Start), the game's Settings and the start screen, Enabled by default: the shadow mask's near
  cascade taps are bilinear - each tap's lit / shadowed test (depth, or the shadow map's object id = the receiver's) is
  made on the 4 texels around it and weighted by where it lies between them - so the edges of people's and Pokémon's
  shadows no longer step along the shadow map's texels. The far cascade (no id test: bilinear, sloping ground would
  shadow itself) is kept as it was. Checked on the Prelude Beach: same image through the rewritten path with one texel,
  no new dark specks on open sand.
- Frame rate: an Unlimited choice (the game's Settings and the start screen): as fast as the PC goes, paced at 360 at
  most. New Frame Timing row, Real Time by default: above 30 fps the world now steps by the time each frame really
  took instead of the frame rate's fixed period, so a dip (a hitch, a busy area) no longer slows the game down -
  walking, people, the camera and the cutscene clocks keep their speed. A step is never longer than 33.3 ms (the
  game's own at 30 fps). Fixed: the old way, each frame a fixed step. Unlimited always uses Real Time.
  `LARECOMP_DYN_DT=0/1` overrides the setting.
- GPU: the barrier before each render pass (from Z-A) now waits only for what that pass needs: which images every pass
  rendered to and every draw sampled since the last barrier are tracked, a pass that touches none of them starts
  without one, and one that does waits only for the earlier passes' render-target writes (its vertex work can overlap
  them) instead of draining the whole GPU ~60 times a frame. Anything else written on the GPU (copies, compute, storage
  images, stores to guest memory) still makes the next pass wait for everything. Z-A at 4K on an RTX 4090: ~6% less
  GPU time per frame (removing every barrier, which isn't safe, would give ~12%); AMD / Intel drivers act on these
  stage masks more finely. Pictures identical (intro routes of both games, Lumiose). `LARECOMP_VK_PASS_BARRIER=1`:
  a full barrier before every pass, as before.
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
