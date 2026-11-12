#!/bin/sh
# Run ON the server from the folder deploy.sh fills (lzabot.py, discord_sync.py, lzabot.service, deps/).
# deps/ = discord.py and its libraries, downloaded on the PC for the server's Python (no pip/venv/sudo needed here).
set -eu
D=$HOME/lzabot
mkdir -p "$D" ~/.config/systemd/user
cp lzabot.py discord_sync.py "$D"/
rm -rf "$D/deps"; cp -r deps "$D/deps"
PYTHONPATH="$D/deps" python3 -c "import discord; print('discord.py', discord.__version__)"
cp lzabot.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable lzabot.service
systemctl --user restart lzabot.service
loginctl enable-linger "$USER" 2>/dev/null || echo "note: run 'sudo loginctl enable-linger $USER' so the bot runs without a login"
sleep 8; systemctl --user --no-pager status lzabot.service | head -6; journalctl --user -u lzabot -n 5 --no-pager
