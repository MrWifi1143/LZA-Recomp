#!/bin/sh
# Play PLA Recomp. On Hyprland the window opens on PLA_MONITOR (default HDMI-A-1), floating and without taking
# focus from what you are doing (the rule lasts until Hyprland reloads). Extra arguments go to the runtime.
cd "$(dirname "$0")"
if command -v hyprctl >/dev/null 2>&1 && [ -n "$HYPRLAND_INSTANCE_SIGNATURE" ]; then
    hyprctl eval "hl.window_rule({ name = \"plarecomp\", match = { title = \"^(PLA Recomp)\$\" }, monitor = \"${PLA_MONITOR:-HDMI-A-1}\", float = true, no_initial_focus = true })" >/dev/null
fi
exec ./build/lasrecomp extracted --play "$@"
