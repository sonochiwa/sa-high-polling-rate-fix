# Changelog

## 1.0.2

- Added `README.txt` to the release archive.

## 1.0.1

- Changed the release notes to come from this changelog.
- Removed `README.txt` from the release archive; the repository README is the
  documentation.

## 1.0.0

- Added a DirectInput 8 mouse device proxy that serves the game from Windows
  raw input on a thread of its own, so an acquired DirectInput device and the
  legacy mouse messages no longer cost the game thread per packet.
- Added posting of the cursor position, clicks, wheel and double clicks back
  to the game window, so the front-end keeps working with legacy messages
  suppressed.
- Added a latch for button presses shorter than a frame and one accumulator
  per DirectInput device.
- Added fallback to the unmodified DirectInput device for private data
  formats, absolute axis mode, action maps and raw input registration
  failures.
