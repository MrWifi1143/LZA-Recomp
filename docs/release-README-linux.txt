PL:Z-A Recomp (LZ&A Recomp) - Linux test build
==============================================

A native Linux port of Pokemon Legends: Z-A (Nintendo Switch) by static recompilation.

NO GAME FILES ARE INCLUDED. You need your own dump of Pokemon Legends: Z-A with the 2.0.2 update, extracted to
an exefs folder (main, sdk, main.npdm) and a romfs folder. Keys are only needed if you give the start screen
packed .nsp / .xci files (below).

VERSION 2.0.2 ONLY (base game 1.0.0 + the 2.0.2 update). Other versions (2.0.0, or the base game alone) are not
supported: update the game to 2.0.2 on your Switch and dump it again. To check a dump: the update .nsp's .cnmt.xml
says <Version>393216</Version> (patch v6) for 2.0.2 (262144 / v4 is 2.0.0), and its control.nacp's display version
says 2.0.2.

Requirements
------------
- 64-bit x86 Linux (any recent distribution: SteamOS, Bazzite and other immutable ones too). Everything the game needs
  is bundled (glibc, SDL 3, FFmpeg, the compiler); only the GPU driver comes from your system
- A CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Zen or newer); checked at startup
- A GPU whose driver supports Vulkan 1.3 (Mesa RADV / ANV, or NVIDIA's driver), with a current driver
- About 25 GB of free disk space during the one-time setup

Getting exefs and romfs
-----------------------
Dump the base game and the 2.0.2 update from your own Switch (.nsp / .xci), and prod.keys + title.keys
(Lockpick_RCM). The extracted files must have the 2.0.2 update applied; other versions won't work. Plain hactool
can't apply Z-A's update: use hac2l or hactoolnet.
1. Unpack both .nsp files (e.g. hactoolnet -t pfs0 base.nsp --outdir base). The program NCA is the biggest .nca.
2. hac2l -k prod.keys --titlekeys=title.keys --basenca=base/<base program>.nca
         --exefsdir=game/exefs --romfsdir=game/romfs update/<update program>.nca
   (hactoolnet takes the same --basenca, --exefsdir and --romfsdir options.) A title key missing from your
   title.keys is in that .nsp's .tik file (rights ID at offset 0x2A0, key at 0x180): the base game's and the
   update's each have one.
The start screen can also read your .nsp / .xci files itself: put them in one folder with your prod.keys and
pick that folder.

How to play (AppImage)
----------------------
1. Make the AppImage executable (file manager: Properties > Allow executing, or chmod +x LZA-Recomp-*.AppImage)
   and open it. AppImages need FUSE; if it doesn't start, install your distribution's fuse package (libfuse2 / fuse3)
   or run it with --appimage-extract-and-run.
2. Click "Select game folder..." and pick the folder that holds your extracted exefs and romfs (or your .nsp / .xci
   files with prod.keys).
3. Wait while the game files are copied and the game code is generated and compiled for your PC. This happens only
   once and takes a while (from several minutes on a big CPU to much longer on a small one). The progress bar shows
   the time left.
4. Click Play. The first start then prepares the shaders once (a progress bar).

The AppImage itself is never written to: settings (larecomp.ini), saves (save/), the game files (extracted/), caches
(cache/), mods (mods/) and the log (lasrecomp.log) are in ~/.local/share/zarecomp/ ($XDG_DATA_HOME/zarecomp). To back up
or move the game, copy that folder. LARECOMP_DATA_ROOT=<folder> starts it with another folder.
On Steam Deck / SteamOS: add the AppImage to Steam as a non-Steam game to play it from Game Mode.

Start screen options
--------------------
Resolution, fullscreen, anti-aliasing (SMAA), frame rate (30 is the game's own; 60, 120 and 144 are experimental),
"Show FPS counter" (the game's frames per second and frame time, top right), "Button prompts" (Auto: what you
play with - the keys and mouse buttons you bound, or the controller's Xbox letters / PlayStation shapes, switching as
you change; or always one of them), mouse camera and sensitivity (Keyboard section), mods, controller test and remapping,
keyboard remapping.

Controls
--------
Keyboard and mouse (every key and mouse button can be changed on the start screen, under Keyboard):
  WASD move (hold Left Ctrl to walk), the mouse turns the camera: click in the game window to take the mouse, Esc
  lets it go. Space or Enter = A, Q or Backspace = B, Tab = X, R = Y, Z / C = L / R, right / left mouse button
  (or 1 / 3) = ZL / ZR, = and - = Plus / Minus, arrows = D-pad, F = left stick click, numpad 8/4/5/6 = camera.
  The game's menus work with the same keys (WASD or arrows to move, Space to choose, Q to go back); they can't be
  clicked with the mouse.
Controllers: any controller SDL recognizes (Xbox, PlayStation, Switch Pro...).
The button prompts show what you play with: the keys you bound, Xbox or PlayStation buttons (start screen "Button
prompts", Auto by default).
Esc or F1: the menu. F11 or Alt+Enter: fullscreen.

Everything stays in this folder: settings (larecomp.ini), saves (save\), the game files (extracted\), caches
(cache\), mods (mods\) and the log (lasrecomp.log). To move or back up the game, move or copy the whole folder.

Troubleshooting
---------------
- The game code didn't build: check the free disk space in your home folder; details are in
  ~/.local/share/zarecomp/lasrecomp.log.
- The window doesn't open / "no Vulkan": install your GPU's Vulkan driver (Mesa's vulkan-radeon / vulkan-intel, or
  NVIDIA's driver with its Vulkan support) and check with vulkaninfo.
- The game closes or doesn't start: send lasrecomp.log and say what happened. Running the AppImage from a terminal
  shows the log too.

Legal
-----
Fan project, not affiliated with or endorsed by Nintendo, The Pokemon Company, Game Freak or Creatures.
MIT licensed (LICENSE.txt), built on Let's Go Recomp and LAS Recomp (licenses/LICENSE-LASRecomp.txt). Bundled
third-party software keeps its own license: licenses/THIRD-PARTY-NOTICES.txt lists each part, its license and where
its source code is.
