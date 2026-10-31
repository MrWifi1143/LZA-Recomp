# The Windows release (cross-built from Linux)

The Windows exe is built on Linux with [zig](https://ziglang.org) as the C/C++ compiler for `x86_64-windows-gnu`.
The release zip holds the exe, the DLLs it loads, and what its start screen needs to build the game code on the
player's PC (a portable Python with the generator, and zig for Windows). It holds no game files and no game code.

## The build folder (`LZA_WINBUILD`)

The scripts read everything that is not in this repository from one folder, `LZA_WINBUILD`:

| Path | What |
|---|---|
| `zig-linux/` | zig 0.17.0 for Linux (`zig-x86_64-linux-0.17.0.tar.xz`, unpacked), the cross compiler |
| `zig-windows.zip` | zig 0.17.0 for Windows (`zig-x86_64-windows-0.17.0.zip`), shipped to players |
| `deps/` (`bin/`, `include/`, `lib/`) | the Windows libraries: SDL 3, shaderc (+ glslang, SPIRV-Tools), libiconv from the MSYS2 UCRT64 packages (`mingw-w64-ucrt-x86_64-{sdl3,shaderc,glslang,spirv-tools,libiconv}` and their dependencies, unpacked), plus FFmpeg 9.0.2 cross-built with zig as LGPL (configure line in `THIRD-PARTY-NOTICES.md`) and installed into the same prefix |
| `python-embed.zip` | the Python 3.12.10 embeddable package for Windows x86-64 (python.org) |
| `wheels/` | the Windows wheels the generator imports: `capstone-5.0.9-py3-none-win_amd64.whl`, `unicorn-2.1.4-cp37-abi3-win_amd64.whl`, `lz4-4.4.5-cp312-cp312-win_amd64.whl` (PyPI) |

Also needed on the build machine: `llvm-windres`, `llvm-objdump`, `llvm-nm`, `zip`, `python3`.

## Building

From the repository root:

```sh
export LZA_WINBUILD=/path/to/winbuild
make dist                 # = tools/windows/build_exe.sh $(WIN_EXE) + tools/windows/make_zip.sh $(WIN_TAG) $(WIN_EXE)
```

`WIN_EXE` / `WIN_TAG` default to this branch's game (`PLARecomp` / `PLA` or `ZARecomp` / `ZA`). The result is
`$LZA_WINBUILD/dist/LZA-Recomp-<tag>/` and `LZA-Recomp-<tag>-windows.zip`.

- `build_exe.sh` builds the runtime through the Makefile (`PLATFORM=windows B=build/win`, compilers `zcc` / `zcxx` /
  `zwindres` here), then relinks the exe with `-export-all-symbols` and an import library, `build/win/lasrecomp.dll.a`:
  the game code library the player's PC builds imports the runtime's functions from the exe through it.
- `make_zip.sh` assembles the folder: the DLLs the exe imports (followed recursively, without the Vulkan loader every
  GPU driver installs), Python trimmed to what the generator uses, zig trimmed to what compiling C for x86-64 Windows
  needs, `gamelib-src/` (the headers and entry file the game code is compiled with), the README for players
  (`docs/release-README.txt`) and every license (`licenses/`). It refuses to finish if anything from the game ended up
  in the folder.

## Building on Windows

`tools/windows/build.py` builds the same exe natively on Windows, with nothing but Python and zig (no make, MSYS2 or
bash): it compiles with the Makefile's Windows flags, works out the same game code id and translator hash the Makefile
does, and relinks with every runtime function exported plus the import library, as `build_exe.sh` does. Its output
goes to `build/win-native/` (the Linux cross build's `build/win/` stays its own).

What you need:

- Python 3.8 or later (python.org)
- the build folder (`LZA_WINBUILD`) with `deps/` and `zig-windows.zip` from the table above: it is unpacked to
  `zig-windows/` on first use (or point `--zig` / `ZIG` at a zig 0.17.0 `zig.exe`, or have it on PATH)

From the repository root, in PowerShell:

```powershell
$env:LZA_WINBUILD = 'D:\winbuild'
python tools/windows/build.py                          # build/win-native/<exe>.exe + lasrecomp.dll.a (+ plugins/)
python tools/windows/build.py --install D:\LZA-Recomp-ZA   # ... and put it into an installed release folder
```

`--install` is the quick way to play a change: unzip a release once (its start screen sets up your game files and
builds the game code), then each build drops the new exe (and the Steam relay plugin, on Z-A's branch) into that folder
- the old exe kept as `<exe>.exe.prev`; it works while the game is running too (the running one is moved aside). The
build is incremental (objects older than their source or a runtime header are rebuilt; `-j` sets the parallel jobs).
Making the release zip itself (`make_zip.sh`) still happens on Linux.

Keep zig (and so the build folder, or `--zig`'s zig) and an installed release on an NTFS drive: on a btrfs drive
mounted in Windows (WinBtrfs, e.g. a drive shared with Linux) zig fails at random under parallel compiles ("Permission
denied" / "failed to open" on its own files, "unable to open global cache directory ... Unexpected") - the build
here then fails, and so does a release's game code build on its start screen. Checked 2026-10-31: the same build on
NTFS ran clean. The repository itself can stay on the shared drive.

## Testing

Wine runs the exe and the game (with a game code library cross-built on Linux), but not the player-side game code
build: zig's cache locking uses `NtLockFile` in a way Wine doesn't implement ("checking cache failed:
manifest_create Unexpected"). That part is tested on Linux with the same generator and zig, in a folder laid out like
the release, or on real Windows.
