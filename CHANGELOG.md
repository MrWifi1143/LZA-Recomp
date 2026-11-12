# Changelog

Releases are tagged per game: `LegendsA-v*` (PL:A Recomp, branch `LegendsA`) and `LegendsZA-v*` (PL:Z-A Recomp,
branch `LegendsZA`). The Windows zips hold no game files and no game code.


## Unreleased (branch `LegendsA`)

## 2026-11-11: LegendsA-v1.2.0-rc5 (release candidate)

Previous release: LegendsA-v1.2.0-rc4. Game version: Pokemon Legends: Arceus 1.1.1. Same game code as rc1: no rebuild
on start, and copies already set up can take the hotfix zip (the program only).

- Ride Pokemon jump as high at any frame rate: above 30 fps the physics step took a whole 30 fps share of gravity each
  frame, so a Wyrdeer jump at 144 fps topped out at ~0.5 m instead of ~2 m. Gravity is now scaled to each step's time
  (measured: 30 fps 2.00 m, 60 fps 1.97 m, 144 fps 2.00 m). Reported on Discord ("Physics bound to framerate?").

## 2026-11-10: LegendsA-v1.2.0-rc3 (release candidate)

Previous release: LegendsA-v1.2.0-rc1 (rc2 was withdrawn: its fix missed the case that crashed). Same game code as rc1:
no rebuild on start, and copies already set up can take the hotfix zip (the program only).

- Crash fix, checked in game: the area change right after Laventon's photo crashed every time on the same save; now
  it goes through (lasrecomp.log: "[race] area loader ..."). The area loader thread reads an object through a weak
  reference the main thread had just let go (expired, not missing - what rc2 missed).

## 2026-11-10: LegendsA-v1.2.0-rc2 (withdrawn)

Previous release: LegendsA-v1.2.0-rc1. Game version: Pokemon Legends: Arceus 1.1.1. Same game code as rc1: no rebuild
on start, and copies already set up can take the hotfix zip (the program only).

- Crash fix: the game could crash during an area change (seen right after Laventon's photo, about 24 minutes into a new
  game; "signal 11 accessing 0x118" in lasrecomp.log). The area loader read a shared object the main thread had just
  freed; that case is now handled (lasrecomp.log says "[race] area loader ..." when it happens).

## 2026-11-10: LegendsA-v1.2.0-rc1 (release candidate)

Previous release: LegendsA-v1.1.1. Game version: Pokemon Legends: Arceus 1.1.1. The game code is built again once on
the first start (the generator changed): a few minutes, as at setup.

- Link Trades: trade with another copy on this PC, on your LAN, or with Steam friends anywhere (the Steam relay plugin,
  off until switched on in the Mods page). Pick the local trade at Simona's and enter the same Link Code on both sides.
- Steam Frame / ARM Linux: Arceus runs natively on AArch64 (Adreno / Turnip), 30 fps at 720p.
- Lower-end hardware: less CPU per frame (the function dispatcher's fast path, texture pool lookups, settings read
  once instead of every draw); on ARM the shader warm-up is off (it overheated the Frame) and the game code keeps
  registers in locals (3% less CPU there).
- Texture packs stay under the GPU's memory (no GPU hang on the PS5 under Linux with the HD pack).
## 2026-11-09: LegendsA-v1.1.1

Previous release: LegendsA-v1.1.0. Game version: Pokemon Legends: Arceus 1.1.1.

- Lush Foliage now brings its trees: 19,294 more across the seven areas - the forests about half again as full, and
  forest on the hills and steep banks round the playing areas that nobody can reach. They can't be shipped (they are
  the game's own area files with trees added), so the mod carries only where each tree stands (trees.lft) and the
  recomp builds the trees from your game files the first time the game starts with the mod on (about a second, ~140 MB
  in the mod's folder; again only if the game's files or the mod change). Any mod can do the same: mod.ini tree_patch.
  Needs a decent PC (tested on an RTX 4090 at 4K: 60 fps held in Jubilife and the forests).

## 2026-11-09: LegendsA-v1.1.0

Previous release: LegendsA-v1.0.1. Game version: Pokemon Legends: Arceus 1.1.1.

Fixes:

- The game could freeze for good after crafting a batch (the most Poke Balls at once) above 30 fps - the result text's
  "print it all now" waited for the 30 fps clock inside one update (since 0.3.0).
- No more crash after the Commander / taking a photo: the command memory pool is 7x the game's (280 MB, not 40).
- Cutscenes at 60 fps: cuts happen in one frame again (no half-way frame), and scripted waits last their real time
  (Cyllene walks out of view again).
- No pixelated outline round the player in front of water at high resolutions (the water's 480 x 270 depth copy now
  scales with the output).
- A frame-time spike gone: Arceus's exposure counter added one value per pixel on one counter (one draw from 0.1 to
  36 ms); adds are now aggregated per subgroup.

Settings and looks:

- Draw Distance (Settings, start screen): trees out to 400 / 600 / 1000 m (the Switch: 200), far trees kept as full
  models (150 / 200 / 300 m), NPCs loaded further, far Pokemon animating at a full rate further out, rocks and props
  keeping their full mesh further, the terrain mesh's detail rings moved out (40 m -> 160 m at Max), more grass around
  you. The field's GPU memory pool grows with it.
- Shadows (Next Start): Switch / High / Ultra = shadow maps of 1024 (the game's) / 2048 / 4096 - thin things (flower
  stems) keep their shadows further out.
- Shadow Color (Next Start): Natural (the default) tones down the strong purple the game gives shade at dusk; Switch
  keeps the original.
- Round bloom: bright points (the intro's sparks, lamps at night) glow round and soft instead of in squares.
  LARECOMP_ROUND_BLOOM=0 for the original.
- Sharpening setting (AMD RCAS at the window's size); glows read from small targets smoothly.
- Sharper image: the game's dynamic resolution drew the field at 0.9 (fake GPU timings); NPCs no longer vanish at the
  screen's edges while the camera turns.
- 4K: render targets under 500 px wide keep the game's size (a pixel-counting pass ran 4x the pixels: 51 -> 60 fps).

Mods:

- Several mods on at once, in a load order (the Mods page; the first wins where two change the same file).
- Texture packs: a mod's texpack/ folder of <key>.dds / .png replaces the game's textures at any size (Legends Studio
  exports and builds them; tools/texpack_upscale.py makes AI-upscaled packs). Replacements go in at most 8 MB a frame,
  and their keys are hashed in normal memory (a pack once froze loads for 6-20 s). LARECOMP_TEXPACK=<folder> for a run.
- mod.ini: a mod can turn recomp features on. The first: Lush Foliage (mods/Lush Foliage, in this release, off) - 3x
  the grass, flowers and small plants, placed on the GPU (~0.2 ms a frame on an RTX 4090). Needs a decent PC. More
  trees for it are made from your own game files with Legends Studio (see its README).

Linux: a hung game can be inspected (eu-stack / gdb -p) - LARECOMP_DEBUGGABLE=0 to keep the system's rule.

## Earlier, released in 1.0.0 / 1.0.1

- The first-start shader warm-up works on installed copies: it scanned only a `romfs/` folder (a source checkout), and
  an install - Windows included - has only the `romfs.bin` image the start screen builds, so it found 0 shaders and
  every first play compiled them as the game drew. It now reads the files straight from the image: 8,427 programs, as
  from the folder (Z-A got the same fix for its shader packs in 0.6.0).

## 2026-10-31: LegendsA-v0.6.2-alpha

Arceus:

- Cutscenes stay in step with their sound after a hitch (Frame Timing "Real time", the default): the rest of a frame
  longer than 33.3 ms is now made up over the next frames (each still at most 33.3 ms), up to 1 s - before, every hitch
  put the picture further behind the sound for good. Found in Z-A's intro (2.5 s behind by the station on a first play).
  `LARECOMP_CATCHUP_MS=0`: as before.
- AMD Radeon (and other GPUs whose shaders run wider than 32 lanes where the driver can't fix them at 32): the game's
  warp operations (lane ids, lane masks, votes, shuffles) now work on 32-lane slices, as a Switch warp - ported from
  Z-A, where a Radeon didn't draw some trees in the intro. AMD's Windows driver can fix only compute shaders at 32, so
  vertex and pixel shaders ran 64 wide. Translations made this way have their own shader cache. `LARECOMP_NO_WARP_SLICES=1`:
  as before; `LARECOMP_WARP_SLICES=1` forces them anywhere (a 32-wide subgroup is one slice: the same picture).
## 2026-10-31: LegendsA-v0.6.1-alpha

Arceus:

- Fixed: a controller could do nothing in the game while the launcher's Controls page saw it - the game used only
  the first controller the system listed, and on Windows that is often a virtual one (Steam Input, DS4Windows /
  ViGEm, vJoy, Virtual Desktop) or a second receiver. Every connected controller is now opened and the game follows
  the one a button was last pressed on; the launcher's own controller navigation takes any controller too.
## 2026-10-31: LegendsA-v0.6.0-alpha

Arceus:

- Windows: no more late frame every 2 seconds. The log line the renderer writes every 2 s took ~18 ms to write (the
  C runtime writes its unbuffered log a piece at a time), on the thread that presents the frames; each log line is
  now formatted first and written in one go (~0.3 ms). Measured in Z-A (Lumiose walk, 60 cap, RTX 4090): 1% low
  41-43 -> 59.2 fps, late frames 63-75 -> 0; the same runtime code here.
- Windows: vertex data is kept in video memory and copied again only when the game writes it, as on Linux - the page
  write tracking (runtime/pagewatch.c) was Linux-only, so Windows copied every vertex range at every draw. Lumiose walk
  on an RTX 4090: 1370 MB -> 214 MB uploaded per 2 s (Linux: 214), the copied ranges 138 thousand -> 0; frame times on
  that PC unchanged (its PCIe bandwidth wasn't the limit). LARECOMP_PAGEWATCH=0 turns it off.
- Experimental settings (start screen, Display tab), as in Z-A: frame rates above 60 - 120, 144, Unlimited - only with
  it ticked, in the start screen and the game's Settings (Frame Rate shows 30 / 60 otherwise). An older larecomp.ini
  with a rate above 60 turns it on.
- The start screen's Mods page is one list, as in Z-A: translations and plugins with a tick box each, Add a mod... /
  Remove / Open the mods folder at the top; a plugin change restarts the game once when Play is pressed.
- Plugin API 3 (as Z-A's; the local-play relay functions are empty here), plugins' shutdown hooks run when the game
  quits, opt-in plugins (larecomp.ini plugins_on).
- Shader translator safety fix (found in Z-A, where it hung the GPU during a Link Trade): typed GLSL kept integer
  values such as loop counters in float variables when a register held both kinds of data; the GPU may flush such
  values (denormal floats) and a shader loop would never end. Registers are now float only when every value written to
  them is float data. Arceus shader compiles: 11% faster than untyped GLSL (was 44%). Intro checked: shadow smoothing
  still applied, no faults.
- Steadier frame rate when capped: while waiting for the next frame's time the game thread lends its emulated core
  to the game's other threads, and taking it back at the frame's time sometimes waited milliseconds for the thread
  holding it - the frame started late. It is now taken back ahead of time and the last moment waited out. Intro capped
  at 60 on four efficiency cores (an older quad-core PC): 0.1% low 19 -> 54 fps, longest frame 64 -> 20 ms.
  LARECOMP_PACE_LOG=1 logs the take-backs.
- A thread cancelled while waiting for a lock no longer has that cancel land on its next lock or event wait: as
  on the Switch only a WaitSynchronization takes it. Arceus aborted at random in its intro (an event wait answered
  "cancelled", svcBreak 2001-0118, 1 run in 4); the game cancels threads mid-lock 7-8 times in that intro.
- Shaders build ~44% faster (the first start's shader warm-up, and new areas on a first visit): the translated
  GLSL now types each register and constant buffer as float or uint by how it is used, instead of uint everywhere
  with a conversion around every float operation, folds float constants, and leaves out helper functions a shader
  doesn't call. glslang over 1,613 of the game's shaders: 7.5 -> 4.2 ms each, none failing; the intro's pictures
  the same with and without. The cache is rebuilt once. `LARECOMP_XLAT_NO_RETYPE=1` turns it off.
- The on-screen keyboard is headed with the game's own text ("What is your name?") instead of always "Enter a name".
- GPUs whose driver can't map the game's memory (no `VK_EXT_external_memory_host`: Mesa's Turnip on Adreno, the
  Steam Frame) and AMD's Linux driver (RADV, which revalidates an imported buffer on every submit): the game's heap is
  then made of GPU memory (dma-buf chunks mapped at its addresses, bound into one sparse buffer) - ported from Z-A.
  NVIDIA, Windows and the rest keep the import. `LARECOMP_DMABUF_HEAP=0` turns it off, `=1` forces it.
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
