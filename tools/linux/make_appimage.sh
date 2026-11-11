#!/bin/bash
# make_appimage.sh <tag> <name> <data folder> <title>: the Linux release folder (make_tar.sh: $LZA_LINBUILD/dist/
# LZA-Recomp-<tag>) as one AppImage: $LZA_LINBUILD/dist/LZA-Recomp-<tag>-x86_64.AppImage. Run from the repository root,
# after make_tar.sh. e.g. tools/linux/make_appimage.sh ZA ZARecomp zarecomp "PL:Z-A Recomp"
# An AppImage is read-only: the game files it sets up, the saves, the settings and the caches go to
# $XDG_DATA_HOME/<data folder> (~/.local/share/<data folder>), as runtime/paths.c does for a read-only install.
# Needs appimagetool ($APPIMAGETOOL, or on PATH).
set -euo pipefail
L=${LZA_LINBUILD:?set LZA_LINBUILD (tools/linux/README.md)} TAG=${1:?tag, e.g. ZA} NAME=${2:?name, e.g. ZARecomp}
DATA=${3:?data folder, e.g. zarecomp} TITLE=${4:?title, e.g. "PL:Z-A Recomp"}
TOOL=${APPIMAGETOOL:-$(command -v appimagetool || true)}
[ -n "$TOOL" ] || { echo "no appimagetool (APPIMAGETOOL=<path> or on PATH)"; exit 1; }
S=$L/dist/LZA-Recomp-$TAG A=$L/dist/appdir-$TAG
[ -x "$S/lasrecomp" ] || { echo "no $S: run tools/linux/make_tar.sh first"; exit 1; }
rm -rf "$A" && cp -a "$S" "$A"
rm -f "$A/$NAME"  # (the folder release's launcher: AppRun takes its place)

cat > "$A/AppRun" <<EOF
#!/bin/sh
# $TITLE as an AppImage: lasrecomp through its own lib/ld-linux-x86-64.so.2 and lib/ (glibc, SDL 3, FFmpeg, shaderc),
# so it runs on any recent x86-64 Linux, immutable ones too; the GPU driver is the system's. The AppImage can't be
# written to: game files, saves, settings and caches live in \${XDG_DATA_HOME:-~/.local/share}/$DATA.
# LARECOMP_DATA_ROOT=<folder> puts them elsewhere. Extra arguments go to the game (e.g. --play skips the start screen).
HERE=\$(cd "\$(dirname "\$(readlink -f "\$0")")" && pwd)
DATA_HOME=\${XDG_DATA_HOME:-\$HOME/.local/share}
export LARECOMP_DATA_ROOT="\${LARECOMP_DATA_ROOT:-\$DATA_HOME/$DATA}"
mkdir -p "\$LARECOMP_DATA_ROOT"
export LARECOMP_LOADER="\$HERE/lib/ld-linux-x86-64.so.2" LARECOMP_SELF="\$HERE/lasrecomp"
exec "\$LARECOMP_LOADER" --library-path "\$HERE/lib" "\$LARECOMP_SELF" "\$@"
EOF
chmod +x "$A/AppRun"

cat > "$A/$DATA.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$TITLE
Comment=Pokemon Legends, recompiled for PC (bring your own game dump)
Exec=AppRun
Icon=$DATA
Categories=Game;
Terminal=false
EOF
cp "$A/icon.png" "$A/$DATA.png"
ln -sf "$DATA.png" "$A/.DirIcon"

OUT=$L/dist/LZA-Recomp-$TAG-x86_64.AppImage
rm -f "$OUT"
ARCH=x86_64 "$TOOL" --no-appstream "$A" "$OUT"
# nothing from the game in it (make_tar.sh checked the folder; the AppImage is that folder)
du -sh "$OUT"
