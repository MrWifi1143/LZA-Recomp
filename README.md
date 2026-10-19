# LegendsRecomp: Legends: Arceus (branch `LegendsA`)

Static recompilations of the two Pokémon Legends games, one branch each:

- **`LegendsA`** (this branch): Pokémon Legends: Arceus
- **`LegendsZA`**: Pokémon Legends: Z-A (in progress)

A native port of **Pokémon Legends: Arceus** (Nintendo Switch) by **static recompilation**: the game's ARM64 code is
translated to C and compiled into a regular Linux (or Windows) program, with a runtime standing in for the Switch's
operating system (kernel, system services, NVN graphics on Vulkan, audio, controllers).

It is built on [letsgo-recomp](https://github.com/raulzrrs/letsgo-recomp) (Pokémon: Let's Go), itself a fork of
[LAS Recomp](https://github.com/peiikai/LASRecomp) (Link's Awakening). Both are MIT licensed; their credits are kept in
`LICENSE` and `LICENSE-LASRecomp`. The Let's Go README is kept as `README-LetsGoRecomp.md`.

> **No game files are included.** You need your own legally dumped copy of the game (exefs + romfs). Nothing derived
> from the game (generated code, assets, keys) is or will be in this repository.

> This project was made largely with AI (Anthropic's Claude, through Claude Code), directed and tested by a human.

## Status

Early, but playable into the open world: boots, first-boot setup, character creation, the opening cutscenes, Prelude
Beach with text, characters and foliage, at 30 fps on Linux / Vulkan (tested on an RTX 4090), up to 4K render scale.
Expect missing effects and bugs. Known issues:

- one post-processing shader (image stores / atomics: SUST, SURED, RED) is not translated yet
- resizing the window disturbs the rendering
- the process hangs on exit instead of quitting

## What changed from letsgo-recomp (highlights)

- **Generator:** 128-bit exclusive pairs (LDXP/STXP), `setjmp`/`longjmp` (Lua), alias entry points for code pointers
  inside functions, AND-masked jump tables, faster builds (no all-label fallback in big functions, `optnone` where
  needed), PLA's module layout (67 MB main).
- **Kernel:** emulated cores (threads pinned to a core never run at the same time, as on Horizon), condition variable and
  address arbiter fixes (lost wakeups), thread core masks, the process system resource / virtual address memory.
- **Graphics:** NVN transform feedback (character skinning), shader global memory for the game's allocator, Maxwell
  LEA / VMNMX, base-instance-relative instance ids, `nvnCommandBufferDrawTexture`, 4K-correct texel fetches.
- **Services:** friends (offline), hwopus DecodeInterleaved (cmd 8).
- **Start screen:** controller test (live buttons, sticks, triggers) and remapping, keyboard remapping.

## Building (Linux)

Requirements: x86-64 with AVX2, a Vulkan 1.3 GPU, clang + lld, SDL 3, FFmpeg, shaderc, Python 3.

```sh
python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn
# your own game files: extracted/exefs (main, sdk, subsdk0, subsdk1, main.npdm) and extracted/romfs
make gen                                      # translate the game code into generated/ (about 2 minutes)
make -j$(nproc) all SHADERC=-lshaderc_shared  # build/lasrecomp + build/lasgame.so (about 6 minutes)
./build/mkromfs extracted/romfs extracted/romfs.bin   # (build it once: cc -Iruntime tools/romfs/mkromfs.c runtime/romfs_build.c)
./play.sh                                     # start screen, then the game (--play skips it)
```

(`SHADERC=-lshaderc_shared` for distributions that ship only the shared shaderc library, such as Arch.)
