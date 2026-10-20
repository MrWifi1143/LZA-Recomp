#!/bin/bash
# build_exe.sh <exe name>: the Windows runtime cross-built with zig (objects through the Makefile), then relinked with
# zig's own lld-link line plus -export-all-symbols and an import library (zig cc drops those flags; the player's game
# code library imports the runtime's functions from the exe through lasrecomp.dll.a). Run from the repository root,
# with LZA_WINBUILD set (tools/windows/README.md). Output: build/win/<exe>.exe + build/win/lasrecomp.dll.a.
set -euo pipefail
W=${LZA_WINBUILD:?set LZA_WINBUILD (tools/windows/README.md)} EXE=${1:?exe name, e.g. ZARecomp}
export LZA_WINBUILD
T=$PWD/tools/windows
make -j"$(nproc)" PLATFORM=windows B=build/win CC=$T/zcc CXX=$T/zcxx WINDRES=$T/zwindres DEPS=$W/deps EXE_NAME=$EXE \
    build/win/$EXE.exe > $W/build_$EXE.log 2>&1 || { grep -E "error" $W/build_$EXE.log | head -20; exit 1; }
LINK=$(grep "zcxx -o build/win/link/$EXE.exe" $W/build_$EXE.log | tail -1 || true)
[ -n "$LINK" ] || LINK=$(cat $W/link_$EXE.cmd)  # (exe up to date: the last link line)
echo "$LINK" > $W/link_$EXE.cmd
eval "$(echo "$LINK" | sed 's|-Wl,--export-all-symbols -Wl,--out-implib,build/win/lasrecomp.dll.a||') -v" 2> $W/zlink_$EXE.log
L=$(grep "lld-link" $W/zlink_$EXE.log | head -1 | sed 's|^.*lld-link |lld-link |')
L=$(echo "$L" | sed -E "s| -DEBUG||; s| -PDB:[^ ]*||; s| -PDBALTPATH:[^ ]*||; s| -IMPLIB:[^ ]*| -IMPLIB:build/win/lasrecomp.dll.a -export-all-symbols|; s| -OUT:[^ ]*| -OUT:build/win/$EXE.exe|")
"$W/zig-linux/zig" $L
! llvm-nm build/win/$EXE.exe 2>/dev/null | grep -q ' f_70'  # (no game code inside)
ls -la build/win/$EXE.exe build/win/lasrecomp.dll.a
