PL:Z-A Recomp (LZ&A Recomp) - Windows test build
================================================

A native Windows port of Pokemon Legends: Z-A (Nintendo Switch) by static recompilation.

NO GAME FILES ARE INCLUDED. You need your own dump of Pokemon Legends: Z-A with the 0.6.0 update, extracted to
an exefs folder (main, sdk, main.npdm) and a romfs folder. This program never asks for keys: they are only used by
the tool you extract the game with.

Requirements
------------
- Windows 10 or 11, 64-bit
- A CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Zen or newer); checked at startup
- A GPU whose driver supports Vulkan 1.3, with a current driver
- About 25 GB of free disk space during the one-time setup

Getting exefs and romfs
-----------------------
Dump the base game and the 0.6.0 update from your own Switch (.nsp / .xci), and prod.keys + title.keys
(Lockpick_RCM). The extracted files must have the update applied; other versions won't work. Plain hactool can't
apply Z-A's update: use hac2l or hactoolnet.
1. Unpack both .nsp files (e.g. hactoolnet -t pfs0 base.nsp --outdir base). The program NCA is the biggest .nca.
2. hac2l -k prod.keys --titlekeys=title.keys --basenca=base\<base program>.nca
         --exefsdir=game\exefs --romfsdir=game\romfs update\<update program>.nca
   (hactoolnet takes the same --basenca, --exefsdir and --romfsdir options.)
Packed files (.xci, .nsp, .nsz, .nca) are not accepted by the start screen: extract them first.

How to play
-----------
1. Unzip this folder anywhere you can write to (e.g. D:\Games\LZA-Recomp-ZA). Not inside Program Files.
2. Open ZARecomp.exe. It is not code-signed: if Windows says "Windows protected your PC", click "More info" and then
   "Run anyway".
3. Click "Select game folder..." and pick the folder that holds your extracted exefs and romfs (or any folder above
   it).
4. Wait while the game files are copied and the game code is generated and compiled for your PC. This happens only
   once and takes a while (from several minutes on a big CPU to much longer on a small one). A console window shows
   the log.
5. Click Play. The first start then prepares the shaders once (a progress bar).

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
- "That's not a supported game": pick the folder of your own extracted Pokemon Legends: Z-A.
- "Those are packed game files": extract them first (above).
- The game code didn't build: check the free disk space; details are in lasrecomp.log. An antivirus may also have
  quarantined part of zig\ or python\: restore it or add this folder as an exception.
- The game closes or doesn't start: send lasrecomp.log (or the console output) and say what happened.

Legal
-----
Fan project, not affiliated with or endorsed by Nintendo, The Pokemon Company, Game Freak or Creatures.
MIT licensed (LICENSE.txt), built on Let's Go Recomp and LAS Recomp (licenses\LICENSE-LASRecomp.txt). Bundled
third-party software keeps its own license: licenses\THIRD-PARTY-NOTICES.txt lists each part, its license and where
its source code is.
