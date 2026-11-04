# Profiles for the game code's optimiser (PGO)

`make` builds the game code library at -O2 with one of these profiles (`Makefile`, PGO): counts of which functions and
branches ran in recorded play of Legends: Arceus 1.1.1, so the compiler lays out and inlines the code that actually runs.
Arceus' Jubilife walk (1440p, uncapped, 2026-11-04, interleaved runs): 208 -> 234 fps, 1% lows 147.5 -> 160.6, frames
over 6.94 ms halved. The picture and the game's logic are the same; only the machine code's layout differs.

- `pla.clang23.profdata.zst`: for clang 23 and later (a source build on a current Linux).
- `pla.clang21.profdata.zst`: for clang 21 / 22 - zig 0.17's, which builds the game code on the players' PCs
  (`runtime/gamebuild.c`); the releases ship it as `gamelib-src/pgo.profdata`.

clang 23 identifies a function's profile differently, so each family needs its own. Both are sparse: functions the walks
never ran are left out and get plain -O2. A profile from another game version simply doesn't match (no effect).
`make PGO=0` builds without one; `LARECOMP_NO_PGO=1` does the same for the start screen's build.

## Recording them again (after a generator change that alters many functions, or a new game version)

1. Instrumented library, per family (`CC=/usr/lib/llvm21/bin/clang` for the 21 family). Pass the profiling flags as
   PGOFLAGS - overriding GENFLAGS on the command line drops the Makefile's own `-march` / `-fPIC`:

       make -j20 B=build/pgogen PGO=0 PGOFLAGS="-fprofile-instr-generate -mllvm -runtime-counter-relocation" \
            GAMELIB_LDFLAGS="-shared -fuse-ld=lld -fprofile-instr-generate" all

2. Play the walks with `LLVM_PROFILE_FILE=<dir>/walk-%p%c.profraw` (`%c`: written as it runs, so a killed game keeps
   its counts). Used: a Jubilife walk from a save (testroot-pla's) and a new game through the opening.
3. `llvm-profdata merge --sparse -o pla.profdata <dir>/*.profraw` (the 21 family: LLVM 21's llvm-profdata, which zig's
   clang 22 reads), then `zstd -19`.
