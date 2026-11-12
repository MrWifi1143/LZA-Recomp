#!/bin/sh
# Run ON the server, from a folder holding lzabot.py, discord_sync.py, lzabot.service (deploy.sh copies them).
# Needs python3 (with venv); the token at ~/.config/lzarecomp/discord-token (deploy.sh copies it, mode 600).
set -eu
D=$HOME/lzabot
mkdir -p "$D" ~/.config/systemd/user
cp lzabot.py discord_sync.py "$D"/
[ -x "$D/venv/bin/python" ] || python3 -m venv "$D/venv"
"$D/venv/bin/pip" install -q --upgrade discord.py
cp lzabot.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now lzabot.service
systemctl --user restart lzabot.service
# keep it running while logged out (may ask for a password on some systems)
loginctl enable-linger "$USER" 2>/dev/null || echo "note: run 'sudo loginctl enable-linger $USER' so the bot runs without a login"
sleep 5; systemctl --user --no-pager status lzabot.service | head -12
