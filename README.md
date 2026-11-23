# Knob

A tiny Windows 10/11 tray app for sound and screens: one ~400 KB exe, no installer, no background services. It only uses parts that ship with Windows.

## What it does

**Sound**
- Volume and mute for every output and input device
- Click a speaker to switch to it; in Settings › Sound, drag lines to play on several speakers at once
- Auto sync lines speakers up with your microphone; each delay can also be typed in
- Pick the microphone; mute every microphone with one button

**Apps**
- Master volume (moves all selected outputs together)
- Per-app volume and mute, with a live "playing" indicator
- Send an app to its own speaker (click its icon); on Windows 11 an app can play on several speakers

**Display**
- Brightness and contrast for external monitors (DDC/CI), brightness for the laptop panel
- Orientation, refresh rate and input for each display, plus an "identify displays" button
- Extend or duplicate a screen (with a 15-second "keep these settings?" safety), HDR on/off for screens that support it
- Night light per screen (warm tint, optional evening schedule) and "Dim further" below the monitor's 0%
- Named profiles (for example Day / Night) and automatic restore when a monitor reconnects

**Also**
- Solid or Glass style, Windows accent or one of six accent colors
- Shortcuts from a short list: open on Sound / Apps / Display, volume, mute, brightness, next speaker and more
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

## Updating

Knob checks `dist/version.txt` on the `main` branch every 6 hours (and when you click **Check now**).
If that version is newer than the running one, the footer shows a red **Update to vX** pill;
clicking it downloads `dist/Knob.exe` and replaces the installed copy.

To ship an update: bump `APP_VERSION` / `APP_VERNUM` in `knob.cpp` and the versions in `app.rc` and `app.manifest`,
build, copy `Knob.exe` to `dist/`, write the new version to `dist/version.txt`, and push to `main`.
