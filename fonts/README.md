# System fonts (optional)

Some of the game's text can use the Switch's built-in system font, which is not part of the game files. Without it,
the recomp falls back to a font installed on your PC (Windows: Yu Gothic or MS Gothic, then Arial; Linux: DejaVu Sans, else Noto Sans or Liberation Sans). Most text is drawn
with the game's own fonts from the RomFS and does not change.

For the exact look, put a copy taken from **your own** console here (system titles *FontStandard*,
0100000000000811, and *FontNintendoExtension*, 0100000000000810):

- `nintendo_udsg-r_std_003.bfttf`: the main font (a `.ttf` with the same name, or named `nintendo_udsgr_std_003`,
  works too)
- `nintendo_ext_003.bfttf`: the button symbols (optional)

`LARECOMP_FONT=<file>` overrides all of them (.ttf / .otf / .ttc / .bfttf).

These fonts belong to Nintendo: they are kept out of git (`.gitignore`) and must never be shared.
