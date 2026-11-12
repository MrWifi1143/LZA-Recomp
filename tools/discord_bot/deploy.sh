#!/bin/sh
# From the PC: copy the bot, its libraries and the token to the server and (re)start it.
# Usage: tools/discord_bot/deploy.sh [ssh host]   (default: havoc)
set -eu
H=${1:-havoc}
here=$(cd "$(dirname "$0")" && pwd)
pyver=$(ssh "$H" 'python3 -c "import sys; print(\"%d.%d\" % sys.version_info[:2])"')
deps=$(mktemp -d)
python3 -m pip install -q --target "$deps" --python-version "$pyver" --implementation cp --abi "cp$(echo $pyver | tr -d .)" \
    --platform manylinux_2_17_x86_64 --platform manylinux2014_x86_64 --platform manylinux_2_28_x86_64 \
    --only-binary=:all: discord.py
ssh "$H" 'rm -rf ~/lzabot-src && mkdir -p ~/lzabot-src ~/.config/lzarecomp && chmod 700 ~/.config/lzarecomp'
scp -q "$here/lzabot.py" "$here/../discord_sync.py" "$here/lzabot.service" "$here/install.sh" "$H":lzabot-src/
scp -q -r "$deps" "$H":lzabot-src/deps
rm -rf "$deps"
ssh "$H" 'test -s ~/.config/lzarecomp/discord-token' || {
    scp -q ~/.config/lzarecomp/discord-token "$H":.config/lzarecomp/discord-token
    ssh "$H" 'chmod 600 ~/.config/lzarecomp/discord-token'; }
ssh "$H" 'cd ~/lzabot-src && sh install.sh'
