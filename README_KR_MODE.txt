This patch keeps English rendering untouched by default and enables a lightweight Korean reading mode only for EPUBs opened from /kr/.

Included changes:
- Adds KoPub Batang 14pt builtin font from crosspoint-reader-ko.
- Registers KOPUB_14_FONT_ID in firmware startup.
- Detects Korean-mode books by path prefix /kr/ or kr/.
- For /kr/ books only:
  - fontId = KOPUB_14_FONT_ID
  - lineCompression uses KoPub-specific spacing map (tight=1.00, normal=1.20, wide=1.40)
  - characterWrap forced ON
  - hyphenation forced OFF
- Character-wrap path now also works for LEFT alignment, not only JUSTIFY.

Files included:
- src/fontIds.h
- src/main.cpp
- src/activities/reader/EpubReaderActivity.cpp
- lib/Epub/Epub/ParsedText.cpp
- lib/EpdFont/builtinFonts/all.h
- lib/EpdFont/builtinFonts/kopub_14_regular.h

Manual apply:
Copy these files over the matching paths in your repo, then rebuild.
