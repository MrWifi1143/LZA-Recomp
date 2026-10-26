# Changelog

Releases are tagged per game: `LegendsA-v*` (PL:A Recomp, branch `LegendsA`) and `LegendsZA-v*` (PL:Z-A Recomp,
branch `LegendsZA`). The Windows zips hold no game files and no game code.

## 2026-10-25: Z-A 2.0.0 (branch `LegendsZA`)

Z-A:

- The recomp now targets **Pokémon Legends: Z-A 2.0.0** (base game + the 2.0.0 update, main build
  `8A1C86C437394B69`) only. Every Z-A address in the runtime (hooks, patched data, the comments that cite them) is
  2.0.0's, ported from 1.x with `tools/port_addrs.py`; the 1.x -> 2.0.0 translation table is gone.
- 1.x (main `B1F12FD919EAE86A`) is no longer supported: the start screen says "This needs Pokémon Legends: Z-A
  version 2.0.0" for those files (also for game files an earlier release set up), and `make gen` stops on a 1.x
  `main`. Saves made with 1.x load in 2.0.0.
- README / release notes: dumping and extracting 2.0.0 (base + update with hac2l `--basenca`, the title keys from
  the `.tik` files).
- `tools/port_addrs.py --rewrite`: the ported addresses written over the old ones in the given sources, for the
  next update.

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
