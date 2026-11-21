# Knob

A tiny Windows 10/11 tray app for sound and screens: one ~400 KB exe, no installer, no background services. It only uses parts that ship with Windows.

## What it does

**Sound**
- Volume and mute for every output and input device
- Play on several outputs at once (the default output's audio is copied to the others), with a sync delay per output to line up Bluetooth
- Pick the microphone; mute every microphone with one button

**Apps**
- Master volume (moves all selected outputs together)
- Per-app volume and mute, with a live "playing" indicator

**Display**
- Brightness and contrast for external monitors (DDC/CI), brightness for the laptop panel
- Orientation for each display, plus an "identify displays" button
- Named profiles (for example Day / Night) and automatic restore when a monitor reconnects

**Also**
- Solid or Glass style, Windows accent or one of six accent colors
- Optional global shortcut to open the panel
- Keep awake, start with Windows
- Updates itself from this repo's releases

## Install

Download `Knob.exe` from the [latest release](../../releases/latest) and run it. It installs itself to `%LOCALAPPDATA%\Knob` and runs from the tray. Running a newer `Knob.exe` later replaces the installed one.

Settings live in `%APPDATA%\Knob\settings.ini`.

> Knob is not code-signed yet, so Windows SmartScreen may warn ("More info → Run anyway"), and Smart App Control blocks it.

## Build

Cross-compiles from Linux with MinGW-w64:

```sh
sudo apt install g++-mingw-w64-x86-64
./build.sh
```

## Releasing

1. Bump `APP_VERSION` / `APP_VERNUM` in `knob.cpp` and the versions in `app.rc` and `app.manifest`.
2. Push to `main`. The Release workflow builds `Knob.exe` and, if that version has no release yet, publishes `vX.Y.Z` with it attached.

Running copies of Knob check the latest release every 6 hours and offer the update in the panel's footer.
