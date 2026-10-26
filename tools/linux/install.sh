#!/bin/bash
# tools/linux/install.sh: set this checkout up to play on Linux. Installs what the build needs with the distro's
# package manager (Arch / Manjaro / EndeavourOS, Debian / Ubuntu / Mint / Pop!_OS, Fedora; asks for sudo only when
# something is missing), makes the game code generator's Python environment (.venv: lz4, capstone, unicorn), builds
# the runtime (build/lasrecomp) and prepares extracted/ - then ./play.sh's start screen takes your own extracted game
# (exefs + romfs), makes its RomFS image and builds the game code on this PC, as on Windows. SKIP_PACKAGES=1 leaves the
# package manager out (other distros: install the same packages yourself).
set -euo pipefail
cd "$(dirname "$0")/../.."
say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mError: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(uname -m)" = x86_64 ] || die "this needs an x86-64 PC (found $(uname -m))"
grep -qw avx2 /proc/cpuinfo || die "this needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Zen or newer)"

# ---- the distro's packages
[ -n "${SKIP_PACKAGES:-}" ] && ID=skip ID_LIKE=skip || . /etc/os-release
ids=" ${ID:-} ${ID_LIKE:-} "
sudo_if() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi; }
case "$ids" in
*" skip "*) ;;
*" arch "*|*" archlinux "*|*" manjaro "*|*" endeavouros "*)
    pkgs="base-devel git clang lld python sdl3 ffmpeg shaderc vulkan-icd-loader vulkan-headers pkgconf"
    missing=$(pacman -T $pkgs || true)
    if [ -n "$missing" ]; then say "Installing: $missing"; sudo_if pacman -S --needed --noconfirm $missing; fi
    ;;
*" debian "*|*" ubuntu "*)
    pkgs="git build-essential clang lld python3 python3-venv pkg-config libsdl3-dev libavcodec-dev libavformat-dev
          libavutil-dev libswscale-dev libshaderc-dev libvulkan-dev"
    missing=""
    for p in $pkgs; do dpkg -s "$p" >/dev/null 2>&1 || missing="$missing $p"; done
    if [ -n "$missing" ]; then
        say "Installing:$missing"
        sudo_if apt-get update
        sudo_if apt-get install -y $missing ||
            die "apt couldn't install everything. SDL 3 (libsdl3-dev) needs Debian 13 / Ubuntu 25.04 or newer."
    fi
    ;;
*" fedora "*)
    pkgs="git make gcc-c++ clang lld python3 SDL3-devel libavcodec-free-devel libavformat-free-devel
          libavutil-free-devel libswscale-free-devel libshaderc-devel vulkan-loader-devel vulkan-headers"
    missing=""
    for p in $pkgs; do rpm -q "$p" >/dev/null 2>&1 || missing="$missing $p"; done
    if [ -n "$missing" ]; then say "Installing:$missing"; sudo_if dnf install -y $missing; fi
    ;;
*)
    die "no package list for '${PRETTY_NAME:-this distro}' yet. Install: clang, lld, Python 3 (with venv), SDL 3,
FFmpeg (libavcodec / libavformat / libavutil / libswscale), shaderc and the Vulkan loader + headers (development
packages), then run this again with SKIP_PACKAGES=1."
    ;;
esac

# ---- the game code generator's Python environment
if [ ! -x .venv/bin/python ] || ! .venv/bin/python -c 'import lz4, capstone, unicorn' 2>/dev/null; then
    say "Python environment (.venv: lz4, capstone, unicorn)"
    python3 -m venv .venv
    .venv/bin/pip install --quiet --upgrade pip
    .venv/bin/pip install --quiet lz4 capstone unicorn
fi

# ---- the runtime (shaderc: Arch and Fedora ship only the shared library)
shaderc=-lshaderc
for d in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu; do
    [ -e "$d/libshaderc.so" ] && break
    [ -e "$d/libshaderc_shared.so" ] && { shaderc=-lshaderc_shared; break; }
done
say "Building the runtime ($(nproc) jobs, a few minutes; log: build/install.log)"
mkdir -p build
if ! make -j"$(nproc)" all SHADERC=$shaderc > build/install.log 2>&1; then
    grep -E "error|Error" build/install.log | head -20
    die "the build failed (build/install.log has the whole output)"
fi
mkdir -p extracted  # (the start screen keeps your game files, saves and settings in this folder)

name=$(sed -n 's/^#define RT_APP_NAME "\(.*\)".*/\1/p' runtime/paths.h)
say "Done. Start $name with:  ./play.sh"
echo "On the start screen, click \"Select game folder...\" and pick the folder with your extracted exefs and romfs"
echo "(Z-A 2.0.0: the base game with the 2.0.0 update applied). The game code is then built for this PC once (several minutes)."
