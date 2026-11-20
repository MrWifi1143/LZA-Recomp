# Noto Sans CJK JP, subset (the overlay's Japanese / Chinese fallback)

`NotoSansCJKjp-Regular-subset.otf`: Noto Sans CJK JP Regular (Google / Adobe, SIL Open Font License 1.1, `OFL.txt`)
cut down to 5,392 characters with fontTools' `pyftsubset` (desubroutinized, no hinting or layout features): kana, CJK
punctuation and symbols, full-width forms, and the kanji of Dear ImGui's Japanese and common Simplified Chinese glyph
lists. Merged into the overlay's font (runtime/ui/menu.mm) for script text and speech bubbles.
