#!/bin/sh
# Play PL:A Recomp: the start screen (controller test and remapping, keyboard, settings), then the game; --play skips it.
# On Hyprland the window opens on PLA_MONITOR (default HDMI-A-1), floating and without taking focus from what you are
# doing (the rule lasts until Hyprland reloads). Extra arguments go to the runtime.
cd "$(dirname "$0")"
if command -v hyprctl >/dev/null 2>&1 && [ -n "$HYPRLAND_INSTANCE_SIGNATURE" ]; then
    hyprctl eval "hl.window_rule({ name = \"plarecomp\", match = { title = \"^(PL:A Recomp)\$\" }, monitor = \"${PLA_MONITOR:-HDMI-A-1}\", float = true, no_initial_focus = true })" >/dev/null
fi
# The game's log (frame times, errors, crash reports) also goes to play.log; the run before is kept in play-prev.log
[ -f play.log ] && mv -f play.log play-prev.log
./build/lasrecomp extracted "$@" 2>&1 | tee play.log
