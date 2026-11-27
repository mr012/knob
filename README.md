# Knob

A tiny Windows 10/11 tray app for sound and screens: one ~700 KB exe, no installer, no background services. It only uses parts that ship with Windows.

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
- Turn off displays right away or on a timer, with an optional dim clock on the black screen and an option to power monitors fully off (DDC/CI)
- Party screen: a screen of soft light that moves with the music, in the cover's colours (four looks)
- Arrangement at real size: drag your screens where they are on your desk so the mouse crosses where the edges really meet
- Lower refresh rate on battery for the laptop screen

**Flyouts** (off until you switch them on in Settings › Flyouts)
- Media: what's playing, its player, play controls, shuffle / repeat and a draggable progress bar, with cover glows
- Up next when a song starts on its own, and lock keys (Caps, Num, Scroll, Insert)
- Never take focus, stay hidden while something runs full screen; position, screen, timings in ms

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
./build.sh       # Knob.exe — the official build, the one that goes in dist/
./build.sh dev   # Knob-dev.exe — for development only: adds a hidden --demo mode
```

`Knob-dev.exe --demo` fills Knob with the design's sample speakers, apps and screens, for comparing
the app with the design. It is never shipped: the official build leaves it out entirely.

## Updating

Knob checks `dist/version.txt` on the `main` branch every 6 hours (and when you click **Check now**).
If that version is newer than the running one, the footer shows a red **Update to vX** pill;
clicking it downloads `dist/Knob.exe` and replaces the installed copy.

To ship an update: bump `APP_VERSION` / `APP_VERNUM` in `knob.cpp` and the versions in `app.rc` and `app.manifest`,
build, copy `Knob.exe` to `dist/`, write the new version to `dist/version.txt`, and push to `main`.
