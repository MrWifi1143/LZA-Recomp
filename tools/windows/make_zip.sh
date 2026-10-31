#!/bin/bash
# make_zip.sh <tag> <exe name>: the Windows release of this branch's game, from tools/windows/build_exe.sh's output
# (build/win/<exe>.exe + lasrecomp.dll.a): $LZA_WINBUILD/dist/LZA-Recomp-<tag>/ and LZA-Recomp-<tag>-windows.zip.
# Run from the repository root. No game files, no game code: the start screen generates and compiles that on the
# player's PC from their own dump (runtime/gamesetup.c, runtime/gamebuild.c).
set -euo pipefail
W=${LZA_WINBUILD:?set LZA_WINBUILD (tools/windows/README.md)} TAG=${1:?tag, e.g. ZA} EXE=${2:?exe name, e.g. ZARecomp}
R=$PWD D=$W/dist/LZA-Recomp-$TAG
rm -rf "$D" && mkdir -p "$D/licenses"
cp "build/win/$EXE.exe" build/win/lasrecomp.dll.a "$D/"
# the DLLs the exe needs, and theirs (not the Vulkan loader: every GPU driver installs it)
python3 - "$D" "$W/deps/bin" "$EXE.exe" <<'PY'
import subprocess, sys, os, shutil
D, B, exe = sys.argv[1:4]
avail = {f.lower(): f for f in os.listdir(B)}
todo, seen = [exe], set()
while todo:
    out = subprocess.run(['llvm-objdump', '-p', os.path.join(D, todo.pop())], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if 'DLL Name:' not in line: continue
        d = line.split('DLL Name:')[1].strip().lower()
        if d in seen or d not in avail or d == 'vulkan-1.dll': continue
        seen.add(d); shutil.copy(os.path.join(B, avail[d]), D); todo.append(avail[d])
PY
# the game code generator: portable Python 3.12 + capstone / unicorn / lz4 + tools/gen.py and its modules. Only the
# parts the generator uses: no network, SSL, SQLite, XML, asyncio or multiprocessing modules, no static libraries.
mkdir -p "$D/python/Lib/site-packages" "$D/gen"
unzip -q "$W/python-embed.zip" -d "$D/python"
for m in _asyncio _bz2 _decimal _elementtree _hashlib _lzma _msi _multiprocessing _overlapped pyexpat _queue select \
         _socket _sqlite3 _ssl unicodedata _uuid _wmi _zoneinfo winsound; do rm -f "$D/python/$m.pyd"; done
rm -f "$D/python"/{libcrypto-3.dll,libssl-3.dll,sqlite3.dll}
for w in "$W"/wheels/*.whl; do unzip -q -o "$w" -d "$D/python/Lib/site-packages"; done
rm -f "$D/python/Lib/site-packages/unicorn/lib/unicorn.lib"
rm -rf "$D/python/Lib/site-packages/unicorn/include" "$D/python/Lib/site-packages/capstone/include"
printf 'python312.zip\n.\nLib\\site-packages\n..\\gen\n' > "$D/python/python312._pth"
cp tools/{gen,lifter,analyze,nso,passes,patches,simd_c}.py "$D/gen/"
# the compiler: zig for Windows, only what compiling C for x86-64 Windows needs
unzip -q "$W/zig-windows.zip" -d "$D" && mv "$D"/zig-x86_64-windows-* "$D/zig"
L=$D/zig/lib
for d in "$L"/libc/include/*; do basename "$d" | grep -qE 'windows|mingw' || rm -rf "$d"; done
for d in "$L"/libc/*; do n=$(basename "$d"); [ "$n" = include ] || [ "$n" = mingw ] || rm -rf "$d"; done
rm -rf "$L/libc/mingw/lib32" "$L/libc/mingw/libarm32" "$L/libc/mingw/libarm64"
rm -rf "$L/libcxx" "$L/libcxxabi" "$L/libtsan" "$L/libunwind" "$L/compiler" "$L/init" "$L/build-web" "$L/docs" "$D/zig/doc" \
    "$D/zig/README.md"
mkdir -p "$D/gamelib-src"
cp runtime/{cpu.h,simd.h,simd_fp.h,gamelib.h} tools/gamelib_entry.c runtime/hle/hle_symbols.txt "$D/gamelib-src/"
cp docs/icon/icon_1024.png "$D/icon.png"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf "$D/"
cp data/shader_manifest.txt "$D/"  # (the warm-up's first programs: runtime/nvn/vulkan/warmup.c)
# the Steam relay plugin (contrib/steamrelay): shipped, off until switched on in the start screen's Mods page
T=$R/tools/windows
make -s PLATFORM=windows B=build/win CC=$T/zcc DEPS=$W/deps steamrelay
mkdir -p "$D/plugins" && cp build/win/plugins/steamrelay.dll "$D/plugins/"
cp docs/release-README.txt "$D/README.txt"
# licenses: this project's, then every bundled component's (THIRD-PARTY-NOTICES.md says which is which)
cp LICENSE "$D/LICENSE.txt"
cp LICENSE-LASRecomp "$D/licenses/LICENSE-LASRecomp.txt"
cp THIRD-PARTY-NOTICES.md "$D/licenses/THIRD-PARTY-NOTICES.txt"
cp third_party/licenses/* "$D/licenses/"
cp third_party/imgui/LICENSE.txt "$D/licenses/imgui-LICENSE.txt"
cp third_party/vma/LICENSE.txt "$D/licenses/VulkanMemoryAllocator-LICENSE.txt"
cp third_party/smaa/LICENSE.txt "$D/licenses/SMAA-LICENSE.txt"
cp third_party/fonts/Baloo2/OFL.txt "$D/licenses/Baloo2-OFL.txt"
cp "$D/python/LICENSE.txt" "$D/licenses/Python-LICENSE.txt"
cp "$D/zig/LICENSE" "$D/licenses/zig-LICENSE.txt"
cp "$D"/python/Lib/site-packages/capstone-*.dist-info/LICENSE.TXT "$D/licenses/capstone-LICENSE.txt"
cp "$D"/python/Lib/site-packages/lz4-*.dist-info/licenses/LICENSE "$D/licenses/python-lz4-LICENSE.txt"
cp third_party/licenses/GPL-2.0.txt "$D/licenses/unicorn-COPYING-GPL-2.0.txt"
# nothing from the game: no game code library, no game files
! find "$D" -iname 'lasgame*' -o -iname '*.nso' -o -iname '*.nca' -o -iname '*.nsp' -o -iname '*.xci' \
    -o -iname 'prod.keys' -o -iname 'title.keys' -o -name exefs -o -name romfs | grep .
# line ends for Notepad
for f in "$D"/README.txt "$D"/LICENSE.txt "$D"/licenses/*; do sed -i 's/\r*$/\r/' "$f"; done
cd "$W/dist" && rm -f "LZA-Recomp-$TAG-windows.zip" && zip -qr "LZA-Recomp-$TAG-windows.zip" "LZA-Recomp-$TAG"
du -sh "$D" "$W/dist/LZA-Recomp-$TAG-windows.zip"
