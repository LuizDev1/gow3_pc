# Windows renderer integration base

Snapshot of the user-provided GoW3 Windows renderer, based on commit
97f1f8d4410ac8ad4fde29d29041f06e3af51b66, including its local Windows CMake
adaptations. SOURCE_MANIFEST.json records hashes before integration edits.
Original GPL/BSD/MIT licenses and gpu/VENDOR.txt remain intact.

This is the selected Windows renderer build base for TLG. It builds the static
`gow3gpu` library only; internal upstream names are preserved. No GoW3 game
executable, launcher, runtime loader, executable patch or hook installer is imported.
The two src headers contain only inert overlay ABI declarations, not hook code.
Actor-hook status is disabled. The root TLG loader/preparation/tests are preserved.

The renderer still contains GoW3-specific graphics assumptions, shader heuristics,
settings and overlay text. They require audit before this library is executed with
TLG. Compilation is not runtime compatibility. This library is not yet linked to
the TLG diagnostic; no game frame, menu or gameplay is claimed.

Imported text whitespace is normalized; SOURCE_MANIFEST.json records the original bytes.
