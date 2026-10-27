# Changelog

Releases are tagged per game: `LegendsA-v*` (PL:A Recomp, branch `LegendsA`) and `LegendsZA-v*` (PL:Z-A Recomp,
branch `LegendsZA`). The Windows zips hold no game files and no game code.

## Unreleased (branch `LegendsZA`)

Z-A:

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
