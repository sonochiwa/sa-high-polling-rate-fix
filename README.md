# High Polling Rate Fix

`HighPollingRateFix.asi` is a standalone GTA San Andreas plugin that serves
the mouse from Windows raw input instead of DirectInput, removing the frame
stutter that appears with 1000 Hz and faster mice.

GTA San Andreas reads the mouse through DirectInput 8 in immediate mode: once
per frame it calls `IDirectInputDevice8::GetDeviceState` and uses the movement
that accumulated since the previous frame. It only ever looks at the sum, but
the process pays for every single packet the device sends. At 125 Hz that cost
is invisible. At 8000 Hz it arrives sixty-four times as often, and the result
is the uneven, jittery camera movement that high polling rate mice are known
for in games of this era.

There are two costs, and both were measured rather than assumed. An acquired
DirectInput mouse device costs about 42 microseconds of process CPU per
packet, against nothing at all for a device that exists but was never
acquired. Separately, Windows generates the legacy mouse messages for the
process, and that work is done under the desktop input lock, so it does not
merely compete with the game thread for CPU, it makes the game thread wait.
The second cost is the larger one in practice.

This plugin removes both. The game's system mouse device is replaced with one
that reads the same hardware through raw input on its own thread and hands the
game the accumulated movement through the ordinary DirectInput interface. The
real device is still created and still answers everything that merely
describes it, but it is never acquired, so DirectInput never processes a
packet. Legacy mouse message generation is switched off, and the messages the
front-end genuinely needs are posted back to the game window by the plugin.
The game code, the sensitivity setting and the in-game menu are untouched: the
same numbers arrive through the same call.

There is nothing to configure. The plugin has no settings file, no log and no
hotkey; it does its one job from the moment it loads. It changes no game code
and patches no addresses, hooking `dinput8.dll` only, so it is not tied to a
particular `gta_sa.exe` build.

## Features

- Removes both per-packet costs a high polling rate mouse imposes on the game
  thread: the DirectInput bookkeeping and the legacy mouse message generation.
- Reports movement losslessly: every count the device sends is delivered in
  the frame it belongs to, so sensitivity and aim are unchanged.
- Keeps the front-end working by posting back the cursor position, the clicks,
  the wheel and the double clicks that suppression would otherwise remove.
- Latches button presses shorter than a frame, which become likely once the
  device reports every 125 microseconds.
- Supports the left, right, middle and both side buttons, and the wheel.
- Gives every device its own accumulator, so two DirectInput mouse consumers
  in one process do not take movement from each other.
- Confines the cursor while the game holds the mouse exclusively, which is
  what DirectInput did before.
- Falls back to unmodified DirectInput whenever it cannot serve a request,
  including a private data format, absolute axis mode, DirectInput action
  maps, or a failure to register for raw input.
- Makes no permanent changes to `gta_sa.exe`.

## Requirements

- GTA San Andreas 1.0 US (Compact or Hoodlum executable). The plugin patches
  no game code and hooks `dinput8.dll` only, so other builds are expected to
  work but were not tested.
- An ASI loader, such as Silent's ASI Loader or Ultimate ASI Loader.

When raw input cannot be registered or a request cannot be served, the game
keeps the unmodified DirectInput device and behaves as it does without the
plugin.

## Installation

1. Extract `HighPollingRateFix.asi` into the GTA San Andreas directory or its
   `scripts` directory.
2. Start the game.

Remove the file to uninstall the fix.

## Building

Visual Studio 2022 (v143), `Release|Win32`. Open `HighPollingRateFix.sln` or
run:

```powershell
msbuild HighPollingRateFix.sln /t:Rebuild /p:Configuration=Release /p:Platform=Win32
```

The plugin is written to `build\HighPollingRateFix.asi`.

## Repository Layout

```text
HighPollingRateFix.sln
README.md
CHANGELOG.md
LICENSE
.github\workflows\release.yml   Tagged release build, checksum and attestation
src\
  HighPollingRateFix.cpp        DllMain and the dinput8 hooks
  HighPollingRateFix.rc         Version resource
  HighPollingRateFix.vcxproj
  clock.cpp / clock.h           Performance counter timing
  legacy_messages.cpp / .h      Cursor, click and wheel messages posted back to the game
  mouse_device.cpp / .h         The IDirectInputDevice8 proxy the game receives
  raw_input.cpp / .h            Raw input thread and per-device accumulators
  resource.h
  version.h
vendor\
  minhook\                      MinHook, compiled into the plugin
```

## How It Works

The plugin hooks `dinput8.dll!DirectInput8Create`, and through the object it
returns, `IDirectInput8::CreateDevice`. When the game asks for `GUID_SysMouse`,
the real device is created as usual and then wrapped:

- A dedicated thread owns a message-only window registered for raw mouse
  input. It accumulates movement, wheel and button transitions with one
  interlocked operation per packet, so nothing on that path can block the game
  thread.
- The game's `GetDeviceState` call takes and clears the accumulator and fills
  `DIMOUSESTATE2` exactly as DirectInput would, in the same units. Buffered
  reads through `GetDeviceData` are served from the same accumulator.
- Raw input is registered with `RIDEV_NOLEGACY`, which stops Windows
  generating legacy mouse messages for the process. `RIDEV_INPUTSINK` has to
  be set alongside it: on its own the flag stops the raw stream arriving
  altogether, because a message-only window never holds focus and delivery is
  otherwise focus-driven. The focus rule that implies is reapplied in the
  plugin, which drops packets whenever the foreground window does not belong
  to the game.
- The messages the front-end menu and the map read are posted back to the game
  window: the cursor position at up to 500 Hz and only when it changed, each
  button transition as it happens, and a double click when a second press
  lands inside the system double-click time and rectangle. The position comes
  from `GetCursorPos` rather than from the raw deltas, because Windows has
  already applied the pointer speed and the acceleration curve, and the menu
  should feel the way the desktop does.

## Release Integrity

Tagged releases are built by GitHub Actions from the tagged commit. Each
release carries `HighPollingRateFix-vX.Y.Z.zip`, its SHA-256 in
`HighPollingRateFix-vX.Y.Z.zip.sha256` and a signed build-provenance
attestation, which proves that the archive was produced by this repository's
workflow from that revision. It does not prove the code is bug-free.

```text
gh attestation verify HighPollingRateFix-vX.Y.Z.zip -R sonochiwa/sa-high-polling-rate-fix
```

## License

MIT. See [LICENSE](LICENSE).
