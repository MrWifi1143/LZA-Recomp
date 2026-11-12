#!/bin/sh
# From this PC: copy the bot + token to the server and (re)start it.  Usage: tools/discord_bot/deploy.sh [ssh host]
set -eu
H=${1:-havoc}
here=$(cd "$(dirname "$0")" && pwd)
ssh "$H" 'mkdir -p ~/lzabot-src ~/.config/lzarecomp && chmod 700 ~/.config/lzarecomp'
scp -q "$here/lzabot.py" "$here/../discord_sync.py" "$here/lzabot.service" "$here/install.sh" "$H":lzabot-src/
ssh "$H" 'test -s ~/.config/lzarecomp/discord-token' || {
    scp -q ~/.config/lzarecomp/discord-token "$H":.config/lzarecomp/discord-token
    ssh "$H" 'chmod 600 ~/.config/lzarecomp/discord-token'; }
ssh "$H" 'cd ~/lzabot-src && sh install.sh'
