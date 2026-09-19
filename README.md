# High Polling Rate Fix

`HighPollingRateFix.asi` is a GTA San Andreas plugin that removes the frame
stutter that appears with 1000 Hz and faster mice.

The game reads the mouse through DirectInput and pays for every packet the
device sends, so a mouse polling at 1000 Hz or more makes the camera jitter.
The plugin serves the mouse from Windows raw input on its own thread and
hands the game the same movement through the same interface. Sensitivity,
the in-game settings and the front-end are unchanged.

## Features

- Removes the per-packet cost of high polling rate mice.
- Delivers every count the device sends in the frame it belongs to.
- Latches button presses shorter than a frame.
- Supports the left, right, middle and side buttons, and the wheel.
- Falls back to unmodified DirectInput whenever it cannot serve a request.
- Patches no game code; hooks `dinput8.dll` only.

## Requirements

- GTA San Andreas 1.0 US (Compact or Hoodlum executable). Other builds are
  expected to work but were not tested.
- An ASI loader, such as Silent's ASI Loader or Ultimate ASI Loader.

## Installation

1. Extract `HighPollingRateFix.asi` into the GTA San Andreas directory or its
   `scripts` directory.
2. Start the game.

There is nothing to configure. Remove the file to uninstall.

## Release Integrity

Releases are built by GitHub Actions from the tagged commit and carry a
SHA-256 file and a build-provenance attestation:

```text
gh attestation verify HighPollingRateFix-vX.Y.Z.zip -R sonochiwa/sa-high-polling-rate-fix
```

## License

MIT. See [LICENSE](LICENSE).
