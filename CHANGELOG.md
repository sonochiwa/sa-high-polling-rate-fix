# Changelog

## 1.0.3

- Fixed the NVIDIA App overlay cursor stopping every second while the game
  camera moved instead.

## 1.0.2

- Added `README.txt` to the release archive.

## 1.0.1

- Changed the release notes to come from the changelog.
- Removed `README.txt` from the release archive.

## 1.0.0

- Serves the mouse from raw input on its own thread, removing the stutter
  of 1000 Hz and faster mice.
- Front-end cursor, clicks, wheel and double clicks keep working.
- Falls back to unmodified DirectInput when a request cannot be served.
