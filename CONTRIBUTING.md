# Contributing

Bug reports, test results from other PCs and code are all welcome.

## The rule that never changes: nothing from Nintendo

Never post any of these in the repository, issues or pull requests:

- copies of the game in any format (`.xci`, `.nsp`, `.nca`...), keys (`prod.keys`, `title.keys`)
- extracted files (`extracted/`), generated code (`generated/`), the built game library (`build/lasgame.so`,
  `lasgame.dll`: it is recompiled game code), the Switch's system fonts (`.bfttf`)
- any game asset: textures, models, music, text, RomFS files, not even "just one file to test"
- saves (`save/`) and mods containing game files (`mods/`)

Screenshots and short videos of the game in bug reports are welcome.

## Reporting problems

Open an issue with:

- which game (PL:A or PL:Z-A) and which release or commit
- your PC: CPU, GPU, driver version, the resolution and frame rate you chose
- what you did, what happened, and what you expected
- a screenshot or a short video, if it's visual
- if the game closed or froze: `lasrecomp.log` (next to the exe on Windows, the data folder on Linux)
- if you were using a mod, which one

## Code

- Build and test as in the README (Linux) or `tools/windows/README.md` (the Windows release).
- Each game has its own branch (`LegendsA`, `LegendsZA`). A fix to the shared runtime belongs on both: test it on
  both games.
- The runtime came from LAS Recomp (Zelda) by way of Let's Go Recomp. Before blaming the game, check whether one of
  the runtime's assumptions holds for the Legends games (formats, return codes, timing, GPU features).
- To change the game's behavior without regenerating the game code, use the function hooks (`runtime/fnhook.c`).
- Code comments, commit messages and documentation are in English.
