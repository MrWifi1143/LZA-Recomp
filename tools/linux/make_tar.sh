#!/bin/bash
# make_tar.sh <tag> <name>: the Linux release of this branch's game, a folder like the Windows zip:
# $LZA_LINBUILD/dist/LZA-Recomp-<tag>/ and LZA-Recomp-<tag>-linux-x86_64.tar.gz. Run from the repository root.
#   <name>             the launcher (a shell script): runs lasrecomp through lib/ld-linux-x86-64.so.2 with lib/
#   lasrecomp          the runtime, built against lib/'s FFmpeg (B=build/linrel)
#   lib/               glibc, libstdc++, SDL 3, shaderc (+ glslang, SPIRV-Tools), a minimal FFmpeg (LGPL, the movies'
#                      codecs only): its own, so it runs on any recent x86-64 distro, immutable ones (SteamOS,
#                      Bazzite) too. The Vulkan loader and the GPU drivers are always the system's.
#   python/ gen/ zig/ gamelib-src/   what makes the game code on the player's PC from their own dump (as on Windows)
# No game files, no game code. $LZA_LINBUILD holds: deps/ (the FFmpeg: tools/linux/README.md), dl/ (portable Python
# 3.12 from python-build-standalone + wheels/), and zig-linux/ (or $LZA_WINBUILD/zig-linux).
set -euo pipefail
L=${LZA_LINBUILD:?set LZA_LINBUILD (tools/linux/README.md)} TAG=${1:?tag, e.g. ZA} NAME=${2:?name, e.g. ZARecomp}
R=$PWD D=$L/dist/LZA-Recomp-$TAG
ZIG=${ZIG_LINUX:-$L/zig-linux}
[ -d "$ZIG" ] || ZIG=${LZA_WINBUILD:-/nonexistent}/zig-linux
[ -x "$ZIG/zig" ] || { echo "no zig for Linux at $ZIG"; exit 1; }
PY=$(ls "$L"/dl/cpython-3.12.*-x86_64-unknown-linux-gnu-install_only.tar.gz | tail -1)

make -s -j"$(nproc)" B=build/linrel DEPS="$L/deps" SHADERC=-lshaderc_shared build/linrel/lasrecomp steamrelay
rm -rf "$D" && mkdir -p "$D/lib" "$D/licenses" "$D/plugins" "$D/gamelib-src" "$D/gen"
cp build/linrel/lasrecomp "$D/"
patchelf --force-rpath --set-rpath '$ORIGIN/lib' "$D/lasrecomp" 2>/dev/null || true
cp build/linrel/plugins/libsteamrelay.so build/linrel/plugins/steamrelay-helper "$D/plugins/"  # (the Steam relay + its helper: off until switched on in the Mods page)

# lib/: everything lasrecomp and its plugin need except the Vulkan loader (the system's: it finds the GPU drivers)
python3 - "$D" "$L/deps/lib" <<'PY'
import os, shutil, subprocess, sys
D, deps = sys.argv[1:3]
SKIP = {'libvulkan.so.1', 'linux-vdso.so.1'}
def needed(f):
    out = subprocess.run(['readelf', '-d', f], capture_output=True, text=True).stdout
    return [l.split('[')[1].split(']')[0] for l in out.splitlines() if '(NEEDED)' in l]
def find(name):
    for d in (deps, '/usr/lib', '/usr/lib64', '/lib64'):
        p = os.path.join(d, name)
        if os.path.exists(p): return os.path.realpath(p)
    raise SystemExit('missing library: ' + name)
todo, seen = [os.path.join(D, 'lasrecomp'), os.path.join(D, 'plugins', 'libsteamrelay.so')], set()
for extra in ('ld-linux-x86-64.so.2', 'libmvec.so.1', 'libpthread.so.0', 'libdl.so.2', 'librt.so.1'):
    seen.add(extra); shutil.copy(find(extra), os.path.join(D, 'lib', extra))  # (glibc's parts SDL may dlopen)
while todo:
    for n in needed(todo.pop()):
        if n in seen or n in SKIP: continue
        seen.add(n)
        dst = os.path.join(D, 'lib', n)
        shutil.copy(find(n), dst)
        todo.append(dst)
print('lib/:', ' '.join(sorted(seen)))
PY
cp -a "$L"/deps/lib/libswresample.so.* "$D/lib/" 2>/dev/null || true

# the launcher
cat > "$D/$NAME" <<EOF
#!/bin/sh
# $NAME: starts the game with its own libraries (lib/: glibc, SDL 3, FFmpeg, shaderc ...) through
# lib/ld-linux-x86-64.so.2, so it runs on any recent x86-64 Linux, immutable ones too; the GPU driver is the system's.
# This folder is where it keeps the game files it sets up, the saves and the settings: keep it somewhere you can write
# to. Extra arguments go to the game (e.g. --play skips the start screen).
HERE=\$(cd "\$(dirname "\$(readlink -f "\$0")")" && pwd)
export LARECOMP_LOADER="\$HERE/lib/ld-linux-x86-64.so.2" LARECOMP_SELF="\$HERE/lasrecomp"
export LARECOMP_DATA_ROOT="\${LARECOMP_DATA_ROOT:-\$HERE}"
exec "\$LARECOMP_LOADER" --library-path "\$HERE/lib" "\$LARECOMP_SELF" "\$@"
EOF
chmod +x "$D/$NAME"

# the game code generator: portable Python 3.12 + capstone / unicorn / lz4 + tools/gen.py and its modules
tar xzf "$PY" -C "$D"
rm -rf "$D/python/include" "$D/python/share" "$D/python/lib/python3.12/test" "$D/python/lib/python3.12/idlelib" \
    "$D/python/lib/python3.12/tkinter" "$D/python/lib/python3.12/turtledemo" "$D/python/lib/python3.12/ensurepip" \
    "$D/python/lib/python3.12/lib2to3" "$D"/python/lib/libtcl* "$D"/python/lib/libtk* "$D"/python/lib/tcl* "$D"/python/lib/tk*
find "$D/python" -name '__pycache__' -prune -exec rm -rf {} +
for w in "$L"/dl/wheels/*.whl; do unzip -q -o "$w" -d "$D/python/lib/python3.12/site-packages"; done
# (python-build-standalone ships libpython with its debug info: 114 MB -> 30 MB stripped; the wheels' .so too)
find "$D/python" -name '*.so*' -type f -exec strip --strip-unneeded {} + 2>/dev/null || true
ln -s bin/python3.12 "$D/python/python"  # (where the start screen looks: python/python)
cp tools/{gen,lifter,analyze,nso,passes,patches,simd_c}.py "$D/gen/"

# the compiler: zig for Linux, only what compiling C for x86-64 glibc Linux needs
cp -a "$ZIG" "$D/zig"
Z=$D/zig/lib
for d in "$Z"/libc/include/*; do case "$(basename "$d")" in any-linux-any|generic-glibc|x86-linux-any|x86_64-linux-any|x86_64-linux-gnu) ;; *) rm -rf "$d" ;; esac; done
for d in "$Z"/libc/*; do case "$(basename "$d")" in include|glibc) ;; *) rm -rf "$d" ;; esac; done
rm -rf "$Z/libcxx" "$Z/libcxxabi" "$Z/libtsan" "$Z/init" "$Z/build-web" "$Z/docs" "$D/zig/doc" "$D/zig/README.md"

cp runtime/{cpu.h,simd.h,simd_fp.h,gamelib.h} tools/gamelib_entry.c runtime/hle/hle_symbols.txt "$D/gamelib-src/"
for z in data/pgo/*.clang21.profdata.zst; do [ -f "$z" ] && zstd -q -d -f "$z" -o "$D/gamelib-src/pgo.profdata"; done  # (zig is clang 22: Makefile PGO)
cp docs/icon/icon_1024.png "$D/icon.png"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf "$D/"
[ -f data/shader_manifest.txt ] && cp data/shader_manifest.txt "$D/"  # (Z-A: the warm-up's first programs)
cp docs/release-README-linux.txt "$D/README.txt"
# licenses: this project's, then every bundled component's (THIRD-PARTY-NOTICES.md says which is which)
cp LICENSE "$D/LICENSE.txt"
cp LICENSE-LASRecomp "$D/licenses/LICENSE-LASRecomp.txt"
cp THIRD-PARTY-NOTICES.md "$D/licenses/THIRD-PARTY-NOTICES.txt"
cp third_party/licenses/* "$D/licenses/"
cp third_party/imgui/LICENSE.txt "$D/licenses/imgui-LICENSE.txt"
cp third_party/vma/LICENSE.txt "$D/licenses/VulkanMemoryAllocator-LICENSE.txt"
cp third_party/smaa/LICENSE.txt "$D/licenses/SMAA-LICENSE.txt"
cp third_party/fonts/Baloo2/OFL.txt "$D/licenses/Baloo2-OFL.txt"
cp "$D/zig/LICENSE" "$D/licenses/zig-LICENSE.txt"
cp "$D/python/lib/python3.12/LICENSE.txt" "$D/licenses/Python-LICENSE.txt"
cp "$D"/python/lib/python3.12/site-packages/capstone-*.dist-info/LICENSE.TXT "$D/licenses/capstone-LICENSE.txt" 2>/dev/null || true
cp "$D"/python/lib/python3.12/site-packages/lz4-*.dist-info/licenses/LICENSE "$D/licenses/python-lz4-LICENSE.txt" 2>/dev/null || true
cp third_party/licenses/GPL-2.0.txt "$D/licenses/unicorn-COPYING-GPL-2.0.txt"
# nothing from the game: no game code library, no game files
! find "$D" -iname 'lasgame*' -o -iname '*.nso' -o -iname '*.nca' -o -iname '*.nsp' -o -iname '*.xci' \
    -o -iname 'prod.keys' -o -iname 'title.keys' -o -name exefs -o -name romfs | grep .
cd "$L/dist" && rm -f "LZA-Recomp-$TAG-linux-x86_64.tar.gz" && tar czf "LZA-Recomp-$TAG-linux-x86_64.tar.gz" "LZA-Recomp-$TAG"
du -sh "$D" "$L/dist/LZA-Recomp-$TAG-linux-x86_64.tar.gz"
