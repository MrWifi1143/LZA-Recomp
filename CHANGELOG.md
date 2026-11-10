# Changelog

Releases are tagged per game: `LegendsA-v*` (PL:A Recomp, branch `LegendsA`) and `LegendsZA-v*` (PL:Z-A Recomp,
branch `LegendsZA`). The Windows zips hold no game files and no game code.

## Unreleased (branch `LegendsZA`)

## 2026-11-10: LegendsZA-v1.1.0-rc1 (release candidate)

Previous release: LegendsZA-v1.0.2. Game version: Pokemon Legends: Z-A 2.0.2. The game code is built again once on the
first start (the generator changed): a few minutes, as at setup.

- Texture packs: a mod's texpack/ folder of <key>.dds / .png replaces the game's textures at any size (make them with
  Legends Studio), read on loader threads, at most 8 MB put in a frame (no hitches) and kept under the GPU's memory.
- Mods: several on at once (mod = a,b,c - the first wins where two change the same thing).
- New app icon: Prism Tower over the night city.
- Render resolution above the window's: an exact area average (sharper than the bilinear scale).
- Steam Frame / ARM Linux: runs natively on AArch64; the shader warm-up is off there (it overheated the Frame) and the
  game code keeps registers in locals (3-8% less CPU).
- PS5 under Linux (RADV): the game's memory is GPU memory only where it's used - max settings + the HD pack no longer
  restart the session.
- Lower-end hardware: less CPU per frame (the function dispatcher, texture pool lookups).
- Steam relay: Z-A rooms and Arceus rooms never show up in each other's scans.
## 2026-10-31: LegendsZA-v0.6.2-alpha

Z-A:

- Cutscenes stay in step with their sound after a hitch. With Frame Timing "Real time" (the default) a frame longer
  than 33.3 ms only moved the game on by 33.3 ms, and the sound (on the audio device's clock) kept going: each hitch put
  the picture further behind - a first play's shader stalls plus a stream being encoded beside the game left the intro
  seconds out of step. The rest of a long frame is now made up over the next frames (each still at most 33.3 ms, the
  game's own step at 30 fps), up to 1 s - a longer stall is a load and isn't made up. Intro from a new game, cold caches,
  RTX 4090: 2.5 s behind by the station -> only the title card's load (0.7 s) not made up. `LARECOMP_CATCHUP_MS=0`: as before.
- AMD Radeon: shaders that use the GPU's warp operations (lane ids, ballots, votes, shuffles) in vertex and pixel shaders
  now give the Switch's results - a friend's Radeon didn't draw some trees outside the intro's train window. A Radeon
  runs those stages 64 lanes wide and AMD's driver can fix only compute shaders at 32; such stages now use the 32-lane
  slices the Steam Frame build introduced (the same picture where a stage is 32 wide: checked on the intro with them
  forced on an RTX 4090). `LARECOMP_NO_WARP_SLICES=1`: as before.
## 2026-10-31: LegendsZA-v0.6.1-alpha

Z-A:

- Fixed: a controller could do nothing in the game while the launcher's Controls page saw it - the game used only
  the first controller the system listed, and on Windows that is often a virtual one (Steam Input, DS4Windows /
  ViGEm, vJoy, Virtual Desktop) or a second receiver. Every connected controller is now opened and the game follows
  the one a button was last pressed on; the launcher's own controller navigation takes any controller too.
## 2026-10-31: LegendsZA-v0.6.0-alpha

Z-A:

- Fixed: the Mega Dimension DLC wasn't found when it was dumped beside the game rather than inside its folder (a
  `game` folder and a `dlc` folder side by side, the game folder picked on the start screen): the title showed no
  "Downloadable content ready". Setup now also looks for `dlc` in the folder picked and beside the game's own folder
  (there only with the DLC's keyfile.dat in it). Already set up? Set the game files up again from the same folder.
- Windows: no more late frame every 2 seconds. The log line the renderer writes every 2 s took ~18 ms to write (the
  C runtime writes its unbuffered log a piece at a time), on the thread that presents the frames; each log line is
  now formatted first and written in one go (~0.3 ms). Lumiose walk at a 60 cap on an RTX 4090 + i9-12900K: 1% low
  41-43 -> 59.2 fps, the longest frame 31-38 -> 17.6 ms, late frames 63-75 -> 0 (Linux: 59.7).
- Windows: vertex data is kept in video memory and copied again only when the game writes it, as on Linux - the page
  write tracking (runtime/pagewatch.c) was Linux-only, so Windows copied every vertex range at every draw. Lumiose walk
  on an RTX 4090: 1370 MB -> 214 MB uploaded per 2 s (Linux: 214), the copied ranges 138 thousand -> 0; frame times on
  that PC unchanged (its PCIe bandwidth wasn't the limit). LARECOMP_PAGEWATCH=0 turns it off.
- Fixed: on an installed copy (a release set up from your game files) the first start's shader step found nothing to
  prepare - it only looked for the game's shader packs in a romfs/ folder, and an install has the romfs.bin image
  instead. The packs are now read from inside romfs.bin. Checked on a fresh Windows install: 0 -> 23841 shader
  programs prepared up front (365 of the 376 in the manifest), 59 s on 23 threads. Same for Linux installs.
- Windows: a native build from source (tools/windows/build.py: Python + zig, no make or MSYS2; tools/windows/README.md),
  and the start screen's game code build retries a compile that failed for a moment (Windows Defender holding a freshly
  unpacked file).
- With the Steam relay on, a player reachable directly (this PC, the LAN) is joined directly: a room heard both ways
  kept the relay's address (it looked like this PC's). Checked: two copies with the relay loaded join at 127.0.1.x.
- Experimental settings (start screen, Display tab): frame rates above 60 - 120, 144 and Unlimited - are offered only
  with it ticked, in the start screen and in the game's Settings (its Frame Rate row shows 30 / 60 otherwise). Unlimited
  is back behind it (in battles, Pokémon and trainers can misbehave; its warning says so). An older larecomp.ini with
  120 / 144 in it turns the box on, so nobody's frame rate drops on update.
- The start screen's Mods page is one list: every translation (mods/) and plugin (plugins/) with a tick box, what it
  does, and Add a mod... (a .zip / .rar / .7z translation, or a plugin file) / Remove / Open the mods folder at the
  top. One translation at a time; plugins switch on or off, and a change restarts the game once when Play is pressed.
- Steam relay on Windows: the zip ships plugins/steamrelay.dll, off until ticked on the Mods page (plugins that talk
  to others start off: larecomp.ini plugins_on). Steam's own steam_api64.dll is found through the registry and every
  Steam library folder (SteamVR's copy, else any game's; an old one without the networking API is skipped). Checked: a
  Link Trade through the relay between the Windows build (Wine) and the Linux build.
- Plugin API 3: the relay's functions are handed to plugins (a Windows DLL can't look them up in the exe), and plugins'
  shutdown hooks now run when the game quits (before, nothing called them).
- Link Trades and Private Battles with Steam friends anywhere (Linux): the Steam relay plugin (contrib/steamrelay,
  `make steamrelay` -> build/plugins/libsteamrelay.so, copied to plugins/ in the game's folder). With Steam running,
  Link Play's "Nearby" also finds friends who are hosting, and the game's traffic goes over Steam's relay network
  (no port forwarding, no IP addresses shared). Only friends can connect. It runs as Spacewar (app 480, the test app
  every Steam account has), so Steam shows "Playing Spacewar". The Steam API is not shipped: Steam's own copy is used
  (SteamVR's, or any installed game's; LARECOMP_STEAM_API=<path>). Verified: trades through the relay between two
  copies (a stand-in transport, LARECOMP_RELAY_FAKE) and Steam sign-in, friend scans and a clean quit on a real
  account; a trade between two Steam accounts is still to be tried.
- The start screen's Mods page lists plugins (plugins/: the Steam relay, the VR mod) with an on/off switch each
  (larecomp.ini plugins_off). Plugins load before the start screen, so a change restarts the game once when Play is
  pressed, straight into the game.
- Fixed: the GPU could hang during a Link Trade (the copy showing the trade animation stopped with "device lost"
  about 15 s after the Pokémon were exchanged). The shader translator's typed GLSL (faster shader compiles) kept
  integer values such as loop counters in float variables when a register held both kinds of data; the GPU may flush
  such values (they are denormal floats) and a shader loop never ended. Registers are now float only when every value
  written to them is float data. Shader compiles: 9% faster than untyped GLSL (was 44% - most of that gain came from
  the mixed registers). Trade A/B: 3/3 hangs before, 0/4 after.
- Steady frame rate when capped: at 60 fps the game no longer dips into the 50s. While it waits for the next frame's
  time the game thread lends its emulated core to the game's other threads; taking it back at the frame's time sometimes
  waited milliseconds for the thread holding it, and the frame started late. It is now taken back ahead of time (a
  margin that follows how long that takes) and the last moment is waited out. Lumiose walk capped at 60 on four
  efficiency cores (an older quad-core PC), 720p: 58.5 fps average / 1% low 41.5 / 0.1% low 29.5 -> 60.0 / 59.7 / 56.2.
  At 144 on an i9-12900K: unchanged. LARECOMP_PACE_LOG=1 logs the take-backs.
- ARM Linux builds (the Steam Frame) target ARMv8.2: atomics are single instructions instead of a call that checks
  the CPU each time (7% of the Frame's CPU time went to those calls; frame rate unchanged - they weren't on the critical
  path - but less work). Every ARMv8.2+ core qualifies (Cortex-A55 / A75 and later, Snapdragon 845+, Apple M1).
- A first play no longer stutters on shaders. The first start's shader step ("Preparing the game's shaders") now also
  knows what the game will draw with: data/shader_manifest.txt, recorded from play-throughs (the intro to Lumiose, a
  street, a walk with the menu, a battle) - each program's hashes and the exact variant its draws use, and the render
  states of the pipelines (hashes and settings only, nothing of the game's) - and builds those first, down to the
  pipeline parts the GPU driver compiles (the ~50-100 ms per part a fresh driver cache costs). It also unpacks every
  pack of the game data now, not only the shader packs: effects, UI and models carry shaders of their own (~20 s once
  per install). Intro to the city on a fresh install (RTX 4090, Wait): draws skipped 952 -> 0, a new pipeline ready
  after 60 ms on average -> under 1 ms, moments over 50 ms 20 -> 8 (a warm cache: 7 - the game's own scene loads).
  Each install keeps adding what it draws to its own list (cache/warm/played.txt), which the next warm-up (after an
  update) starts with. LARECOMP_WARMUP_LOG=1 logs what still compiles at a draw.
- Steam Frame (and any GPU on 32-lane warp slices): the shader cache is kept between starts. The start-up clean-up
  deleted it - it ran before the GPU was set up, when the cache's folder name (which slices change) wasn't known yet
  - so every start compiled thousands of shaders again: 132 s of shader compiling and 93 C in the first minutes. Now a
  second start compiles 3.7 s' worth and peaks at 81 C, 30 fps held in Lumiose.
- A thread cancelled while waiting for a lock no longer has that cancel land on its next lock or event wait: as
  on the Switch only a WaitSynchronization takes it. Arceus aborted at random in its intro (an event wait answered
  "cancelled", svcBreak 2001-0118, 1 run in 4); the game cancels threads mid-lock 7-8 times in that intro.
- Shaders build ~44% faster (the first start's shader warm-up, and new areas on a first visit): the translated
  GLSL now types each register and constant buffer as float or uint by how it is used, instead of uint everywhere
  with a conversion around every float operation, folds float constants, and leaves out helper functions a shader
  doesn't call. glslang over the game's 8,735 shaders: 106 s -> 59.5 s, SPIR-V 17% smaller, none failing; pictures
  the same (a saved scene, with and without, differs no more than two runs of either). The cache is rebuilt once.
  `LARECOMP_XLAT_NO_RETYPE=1` turns it off.
- The on-screen keyboard is headed with the game's own text ("What is your name?", the Link Code prompt's) instead
  of always "Enter a name".
- GPUs whose driver can't map the game's memory (no `VK_EXT_external_memory_host`: Mesa's Turnip on Adreno, the
  Steam Frame) no longer draw the world as huge stretched slabs: the game's heap is then made of GPU memory (dma-buf
  chunks mapped at its addresses, bound into one sparse buffer), so what its compute shaders write reaches the
  vertices it draws. PCs with the import (NVIDIA, AMD, Intel) are unchanged. `LARECOMP_DMABUF_HEAP=0` turns it off.
- GPUs whose subgroups are wider than 32 and can't be made 32 (Adreno: 64 / 128) run the game's warp operations
  (lane ids, ballots, votes, shuffles) on 32-lane slices, as a Switch warp. `LARECOMP_WARP_SLICES=1` forces it
  anywhere (a 32-wide subgroup is one slice: the same picture), `LARECOMP_NO_WARP_SLICES=1` turns it off.
- Local wireless play between copies of the game on one PC (Linux): Link Play's "Nearby Players" - link trades
  and private battles - with each copy as a console of its own (nn::ldn answered by the recomp, the game's Pia
  traffic over loopback UDP). Tested with two copies: a trade both ways (saved) and a private battle room, battle
  started. Sockets still only work inside such a network: the game never reaches the internet. The profanity filter
  (`ngc:u`) now answers "clean" (the names a trade brings). The Windows build does the same (Winsock): a copy
  under Wine traded with a Linux copy on the same PC. Copies on other PCs of the LAN are found by broadcast (UDP port
  39100 and the game's own ports must get through), or listed with `LARECOMP_LDN_PEERS=<ip>,<ip>`; one copy per PC
  plays over the LAN (further copies on a PC only reach that PC).
- Windows: loading a save no longer crashes a few seconds into the world ("no function at ..." from a fiber switch).
  A game fiber resumed on another thread wrote the old thread's fiber bookkeeping (the compiler kept the
  thread-local address from before the switch); that work is now done after the switch, on the thread it runs on.
  Known: with Frame Timing "Real time", the trade ends on a white screen after "... sent over ..." (fine on "Fixed").
- Faster on CPUs with 6+ fast cores: `extra_workers = auto` (the default) now gives the game up to three job threads
  more than the Switch's three (one per fast physical core past five: 8 performance cores -> 3). Lumiose walk uncapped
  (1440p, Max people, Ultra shadows, i9-12900K, 3 runs each): 167 -> 179 fps, 1% low 120 -> 126, frames over 6.94 ms
  6-8% -> 4%. Auto had meant none since 2026-10-22, when a long session with them showed props and models breaking
  apart - the vertex cache's list overflow fixed two days later; a 99,000-frame walk with them is clean.
  `extra_workers = 0` keeps the Switch's three.
- Foot IK is the game's own again: feet are placed on the ground (slopes, steps, kerbs) as on a Switch. It had been
  off since 2026-10-19 because running stretched the legs back to where the run began; on this build the run, the stop
  and standing are right with it at 30, 60 and 144 fps (the Lumiose walk too, at the same frame rate), so a fix since
  then cured that. `LARECOMP_ZA_FOOT_IK=0` turns it off again (the old workaround).
- A launcher, like an older PC game's (Fallout: New Vegas style), instead of the one-window start screen: the banner
  (a night city around a lattice tower, drawn by the program: no game art), Play, and pages for Options (Display /
  Graphics / Game tabs, with this PC's recommended settings), Controls (controller and keyboard), Game Files and Mods.
  It opens in a window; the game goes fullscreen (or stays a window) after Play. The old start screen is kept for
  console ports: `LARECOMP_START_SCREEN=classic`, or built with `-DRT_CLASSIC_START_SCREEN=1`.
- The start screen reads your dumped `.nsp` / `.xci` files directly, like the emulators: pick the folder with the base
  game and the update (and the Mega Dimension DLC, if you own it) and the recomp applies the update over the base game
  itself, with your `prod.keys` (looked for beside the files, in `~/.switch` and in the emulators' key folders). No
  hac2l / hactoolnet step any more; extracted `exefs` / `romfs` folders still work. Written out: the game code, one
  RomFS image and the DLC's data - checked byte for byte against hac2l's output (every file of both games).
- Smoother 144 fps on Linux: each emulated core's threads may now run on both hyperthreads of its physical core
  (before: one CPU each). The game's main loop and job workers share their core with its RenderWorker, audio and
  helper threads; a thread woken there used to preempt the core's owner or wait out its time slice, and the frame
  stalled behind it. Lumiose walk capped at 144 (Max people, Ultra shadows, 1440p, i9-12900K, 3 runs each): late
  presents (over 7.6 ms) 2.3% -> 0.8%, hitches over 10.4 ms ~26 -> ~10, 1% low ~117 -> ~134 fps.
  `LARECOMP_PIN_SMT=0`: one CPU per core, as before.
- Button icons for Xbox / PlayStation / keyboard prompts are drawn once and kept on disk (cache/glyphs/): each redraw
  (letters as 4x signed distance fields) cost ~1.8 ms of the render thread - 3.6 ms in the frame where two came in.
- Fewer hitches when new models appear: the VRAM vertex cache kept each stream's partial edge pages by reading them back
  from the write-combined upload buffer (~30 us a stream): 4.4 ms of the frame where 150 streams came in (Lumiose) ->
  0.5 ms; they are taken from the game's memory now.
- Fewer hitches when new things appear (the render thread, Lumiose walk at 144): a possible button icon (a small BC4 /
  BC5 texture) is put together in normal memory - it was hashed by reading back the write-combined upload buffer,
  ~0.1-0.3 ms per texture, up to 3.7 ms of one frame; redrawn icons are kept (the game uploads the same prompts
  again and again); block-linear textures are deswizzled a whole GOB at a time (1.8x faster, byte-identical over 4000
  random layouts). Research: `LARECOMP_VK_PROF=1 LARECOMP_VK_PROF_SPIKE_MS=<n>` logs each frame whose command lists
  took over n ms, by category (texture uploads by part).
- Controllers: nn::hid::GetNpadJoyAssignment answers Dual for every controller - the SDK aborts on a value it doesn't
  expect (seen once while loading stalled under a scheduling experiment).
- Research: `LARECOMP_TRACE_AT=<present>` starts the frame trace (render thread, game presents, and now the kernel's
  waits, wakes, core waits and long g_klock waits); thread dumps show each thread's host tid and core mask.
- Shadow Smoothing (Next Start), Settings > Graphics and the start screen, On by default: the shadow mask's 16
  shadow-map taps are bilinear - each tap's lit / shadowed test is made on the 4 texels around it and weighted by
  where it lies between them, as a hardware compare sampler would - so shadow edges are smooth instead of stepping
  along the shadow map's texels (most visible at Switch and High; at Ultra the last small steps go). The game's tap
  pattern, radius and "same surface" test are kept (its tolerance doubled for the neighbouring texels, else sloping
  ground went darker). Same GPU time within noise on an RTX 4090.
- Draw Distance (Detail), Settings > Graphics and the start screen: Switch / 2x / 4x / Max (10x) the distances the
  game keeps its models' levels of detail at (its trinity_ModelLodSetting entries: trees, props and buildings 50 /
  100 / 150 m, people 8 / 15 / 30 m, Pokémon 50 / 100 / 150 m) - trees down the street no longer thin out and fill in
  with leaves as you walk up to them. Applies at once. Rouge Street on an RTX 4090 at 1440p: ~2.0 ms GPU a frame at
  Switch, ~2.6 ms at 4x or Max. `LARECOMP_ZA_LODSCALE=<k>` overrides it.
- Frame rate: an Unlimited choice (Settings > Graphics and the start screen): as fast as the PC goes, paced at 360
  at most. New Frame Timing row, Real Time by default: above 30 fps the world now steps by the time each frame really
  took instead of the frame rate's fixed period, so a dip (a hitch, a busy street) no longer slows the game down -
  walking, people, animations and the menus keep their speed. A step is never longer than 33.3 ms (the game's own at
  30 fps). Fixed: the old way, each frame a fixed step. Unlimited always uses Real Time. (Unlimited is hidden from the menus for now - in battles the Pokémon ran slow and the trainer behind them twitched; a saved Unlimited loads as 144.) Lumiose at 1440p with High
  shadows and 4x people on an RTX 4090: ~150-160 fps unlimited. `LARECOMP_DYN_DT=0/1` overrides the setting.
- GPU: the barrier before each render pass now waits only for what that pass needs: which images every pass
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
- Shaders with branches are translated as plain structured code: their blocks in order, loops as loops, jump tables as
  a switch - instead of one state machine (`switch (pc)` in a loop) around the whole program, which 86% of Z-A's
  shaders were. The GPU's compiler can now keep values in registers across branches and see (and unroll) the loops.
  NVIDIA's own statistics for the shaders of a Lumiose street: fragment shaders 10% smaller and 3% fewer registers;
  AMD / Intel / mobile compilers (Mesa's NIR: every register a loop variable before) should gain more. All but 10 of
  the game's ~8,000 branching shaders; pictures identical (intro route, Lumiose). `LARECOMP_XLAT_NO_FWD=1`: the state
  machine as before. (A new translator: one more shader warm-up at the next start.)
- Resolution: 540p (the settings and the start screen), for weak GPUs: about half the GPU time of 1080p.
- Start screen: this PC (GPU and its VRAM, CPU threads, RAM) and the settings recommended for it, with an Apply button;
  on a first start (no larecomp.ini yet) they are applied at once. By the GPU, since that is what limits weak PCs
  (Z-A's CPU side held 60 fps on four efficiency cores of an i9-12900K, Steam Deck class): integrated or under 4 GB
  720p / 30 fps, under 8 GB 1080p / 60, under 12 GB 1080p / 60 with High shadows, more 1440p / 60 with High shadows
  (and draw distance 2x with 12+ CPU threads and 16+ GB RAM); Shader Loading "Don't Wait" with 8 or fewer CPU threads.
- Stutter: pipelines are now linked from parts (Vulkan graphics pipeline libraries, where the driver has them): each
  program's vertex and fragment shader is built once, on its own, and every pipeline that uses it is a quick link of
  those parts (~0.1 ms with a warm cache). A first play of the intro on a cold cache: 19 two-second spans with a frame
  over 100 ms instead of 24, 14 s of draws waiting instead of 18. With the caches warm, the intro has 1. `LARECOMP_VK_GPL=0`:
  whole pipelines as before. On AMD (the Steam Deck's RADV) and Intel, where a fast link runs slower shaders, each
  pipeline is also linked with link-time optimization on a worker and replaces the fast one when ready (as DXVK does;
  NVIDIA compiles each part fully, so it is off there; `LARECOMP_VK_GPL_LTO=0/1`).
- The first-start shader warm-up now helps the fragment shaders too: translations were filed by everything the draw
  had bound, and Z-A always binds a few textures and an image most shaders never read - 1 fragment shader in 192 found
  the warm-up's translation. They are filed by what the shader reads now (checked: 47,000 translations identical).
  Shader translation during a first play of the intro: 0.4 s in all. (The new translator means one more warm-up at
  the next start.)
- Screenshots (F12, and the test tools') are encoded on a thread of their own: each was a ~250 ms hitch.
- FPS counter: the GPU's time per frame too ("60 fps  16.7 ms  GPU 2.4 ms"): well below the frame time, the GPU isn't
  what holds the frame rate back. Measured with two timestamps per submission (always on; `LARECOMP_VK_NO_GPUTIME=1`
  off); also in the log's stats lines, and dynamic resolution (ini `resolution = dynamic`) now has it on Vulkan.
- Settings > Graphics > Shader Loading: Wait / Don't Wait (also on the start screen). The first time something is
  drawn, its shaders are built; Wait holds the frame up to 100 ms for them (a stutter), Don't Wait draws on and shows it
  a moment later. A first play of the intro on cold caches: 5 spans with a frame over 100 ms instead of 19. Once the
  caches are warm both are the same. ini `shader_loading = wait | nowait`.
- Draw Distance (People) above Switch now gets extra memory for the characters it lets in (+256 MB / +512 MB /
  +1.25 GB): Max at 1440p with Ultra shadows ran out of memory and crashed opening the menu.
- Shaders reach the game's memory through its 64-bit GPU address (Vulkan buffer device address, required by Vulkan 1.3)
  instead of one storage buffer: storage buffers stop at 4 GB (maxStorageBufferRange), and past that every text in the
  game was blank - Max draw distance with Ultra shadows at 1440p, and 4K, needed more. The 4 GB cap added for it is
  gone (Max + Ultra at 1440p: 4352 MB, text checked). Each word's address is made in 64 bits (a uint index into a
  buffer reference wrapped at 4 GB too). Also the way to GPUs whose storage buffers are smaller (ARM, integrated).
- Start screen: Draw distance (people), Shadows, In background, PKHeX Link and mouse invert, as in the game's Settings.

- PKHeX: Settings > Gameplay > PKHeX Link (Off / This PC / Network) answers PKHeX's LiveHeX plugin like sys-botbase on
  a Switch (port 6000): it detects Z-A 2.0.2 and reads and writes the boxes while you play. Settings > Gameplay >
  Pokémon Files: A on Export saves every Pokémon in your boxes as a PKHeX .pa9 file (pokemon/export/<date>/), A on
  Import puts the .pa9 files in pokemon/import/ into the first empty box slots. Save in the game to keep them.

- Network: the game's request for a local network (what it asks for before local play) is now granted, like a
  console with Wi-Fi on; internet requests still get "no connection", so nothing tries Nintendo's servers. Local
  wireless itself isn't emulated yet, so Link Play's Nearby Players still finds no one.

- The save screen's Current Time (and Last Saved) showed UTC instead of your local time. The time zone service had
  ToPosixTime(rule) and ToPosixTimeWithMyRule mixed up; rules the game loads are now kept apart from the device's own.

- Settings reorganised: the Camera tab is now Graphics and holds Frame Rate, Resolution, Screen Mode, Anti-Aliasing,
  Draw Distance (People) and FPS Counter (below the game's own three camera rows, which can't move); Gameplay holds
  the controls, mouse, In Background and Start Screen rows.

- Shadows: Settings > Graphics > Shadows (Next Start): Switch / High / Ultra. The game draws its shadows from
  1024x1024 shadow maps, so people's and Pokémon's shadows come out as soft blobs; High (2048) shows the shapes of
  legs and arms in them, Ultra (4096) is sharper still. The game itself makes the bigger maps (its own
  "ShadowResolution" value), so everything that reads them follows. It applies the next time you start the game.

- Draw distance for people: Settings > Graphics > Draw Distance (People): Switch / 2x / 4x / Max. The game keeps
  the full models of at most 25 characters (people and their Pokémon) ready and draws at most 20 of them in full,
  nearest first; the rest stay invisible until a place frees up. That's why people at the far end of the station
  platform (a kid with a Staryu, a man with a Fletchling) appear only as you walk closer, on the Switch too. The
  setting raises both numbers (x2, x4, x10).

- Screenshots: F12 (or a controller's capture / share button) saves the game image, without the overlay, as a PNG in
  the data folder's `screenshots` (`ZA_<date>_<time>.png`); a notice in the corner says so.
- Settings > Gameplay > In Background: keep playing, mute, or pause while the game's window isn't focused.
- Settings > Gameplay > Mouse Vertical / Mouse Horizontal: the mouse camera inverted. The game's own Vertical /
  Horizontal Camera Controls apply to the stick only.
- Controller rumble: the game's vibration (its Wwise "VIB" events, e.g. the player being hit) now reaches the
  controller through SDL - before, it went nowhere. The game's own Controller Rumble option switches it.
  `LARECOMP_RUMBLE=0` turns it off. (Not felt yet: no hit happened in the tests.)

## 2026-10-27: LegendsZA-v0.5.1-alpha

Z-A:

- Mega Dimension (DLC): if you own it, put its RomFS in a `dlc` folder beside `exefs` and `romfs` (see the README);
  the game then sees it as on a Switch that owns it (add-on 2, mounted from that folder). Optional - without it the
  game runs as before.
- Title screen: the "Downloadable Content Available" banner (an eShop link) is hidden and MINUS does nothing there.
  The eShop isn't emulated: pressing it froze the game. With the DLC, the "Downloadable content ready" line stays.
- Library applets the game starts (the eShop, error pages) no longer leave it waiting forever: the "applet can start"
  event is signalled and each applet ends at once. (Z-A's eShop link still stops itself after that - hence the
  hidden banner.)
- Debug aids: `LARECOMP_LOG_HANDLES=1` names the service behind each handle; the thread dump shows the handles a
  thread waits on. `LARECOMP_AOC=0` runs as without the DLC.
- Crash at startup (now and then, since 0.5.0): the shadow fix's check of one fragment shader read 0x200 bytes past
  shaders the start-up warm-up had copied - past the end of one, the game stopped with "stopped because of an error".
  It now reads only what the copy holds (the fix still applies: compared at boot).
- Hotel Z room 202 (120 / 144 fps): after the "Here's room 202" scene the player could only turn on the spot - the
  scene's scripted walk into the room never happened, so the scene never ended. Scripted scenes now run at 60 fps when
  the frame rate is set above 60 (free roam keeps the chosen rate); `LARECOMP_ZA_SCENE60=0` turns
  that off. The cause itself (something at the scene's start that goes wrong above ~100 fps) isn't found yet.

## 2026-10-26: LegendsZA-v0.5.0-alpha

Z-A (Pokémon Legends: Z-A **2.0.2** only; see the entry below):

- Shadows: the blotchy grey patches on characters' faces and hair (cutscenes, e.g. Taunie at night) are gone. The
  shader translator ignored condition-code branches (`FSET.CC` + `BRA` on a condition), so the contact-shadow test lost
  its lower bound and every ray hit its own surface. Compared against Eden (RenderDoc): the shadow mask now matches.
- Shadows: the half-resolution depth the shadow and blur passes read takes the same texel of each 2x2 block as the
  console does (it mixed them before: speckled edges).
- Texture filtering: the game's own anisotropic filtering by default. The forced 16x kept tiled detail sharp out to the
  horizon (Arceus's sea showed its wave tiles as a checkerboard). `LARECOMP_ANISO=<n>` still raises it.
- The intro (start to the station platform) was compared shot by shot with Eden: it matches apart from timing.
- Integer textures (`R8_UINT` id maps) are read as integers.

## 2026-10-25: Z-A 2.0.2 (branch `LegendsZA`)

Z-A:

- Correction: for a few hours this branch's notes and code said "2.0.0 only" and called the older build "1.x". That
  was a mistake - the versions were mixed up. Main build `B1F12FD919EAE86A` is **2.0.2** (patch v6, the newest);
  `8A1C86C437394B69` is 2.0.0 (patch v4). The base game is 1.0.0 in both.
- The recomp targets **Pokémon Legends: Z-A 2.0.2** (base game 1.0.0 + the 2.0.2 update) only. Every Z-A address in
  the runtime is 2.0.2's, as before; the 2.0.0 translation table (`za_addr`) is gone.
- Other versions are not played: the start screen says "This needs Pokémon Legends: Z-A version 2.0.2" for them
  (also for game files set up before), and `make gen` stops on a 2.0.0 `main`. 2.0.2 loads saves made with 2.0.0.
- README / release notes: the supported version, how to tell which update a dump is (the update's cnmt Version
  393216 = 2.0.2 / patch v6, or its control.nacp display version), dumping and extracting (base + update with hac2l
  `--basenca`, the title keys from the `.tik` files).
- `tools/port_addrs.py`: reads `f_700...` names too; `--rewrite` writes the ported addresses into the given sources,
  for the next update.

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
