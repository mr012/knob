#!/bin/sh
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-windres app.rc -O coff -o app.res.o
x86_64-w64-mingw32-g++ -std=c++17 -Os -s -municode -mwindows -static -static-libgcc -static-libstdc++ \
  -ffunction-sections -fdata-sections -Wl,--gc-sections -fno-exceptions -fno-rtti -Wall -Wno-unknown-pragmas \
  knob.cpp app.res.o -o Knob.exe \
  -lole32 -loleaut32 -luuid -lgdiplus -lgdi32 -luser32 -lshell32 -lshlwapi -lshcore -ldwmapi -ldxva2 -lwbemuuid -lversion -ladvapi32 -lwinhttp
ls -l Knob.exe
