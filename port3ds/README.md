# Rogue Squadron 64 Recompiled - New Nintendo 3DS port (v0.2)

**New Nintendo 3DS / New 3DS XL / New 2DS XL only.** The original 3DS and 2DS are too slow. Needs Luma3DS custom firmware.

A port of [Rogue Squadron 64 Recompiled](https://github.com/MikeSemicolonD/RogueSquadron64Recomp) to the New 3DS: the
recompiled game runs natively on the ARM11, with its own renderer for the Factor 5 microcode, the recompiled MusyX synth
for sound, and the radar on the bottom screen.

**No game code or data is distributed.** The game comes in two parts:

- `rs64.3dsx`, the port (renderer, sound, controls, updater). It contains no game code and is on the
  [releases page](https://github.com/yaboimanguy-a11y/rogue-squadron-3ds/releases).
- `game.bin`, the game code, converted from **your own ROM** on your computer, once. Keep it to yourself.

## Getting started (one time, on a computer)

1. Download the latest release zip and unzip it.
2. Double-click the setup for your computer: `Setup (Windows).bat`, `Setup (Mac).command`, or run `setup-linux.sh`.
   It installs the tools it needs (it asks first), then asks for your ROM (*Star Wars: Rogue Squadron*, USA 1.0)
   and your SD card. The first run takes a while.
3. Put the SD card in your New 3DS and start *Rogue Squadron 64* from the Homebrew Launcher.

After that you don't need the computer: the game updates itself (below). If an update ever needs a new `game.bin`,
the game says so; then run the setup again.

## Updates

At each start, with Wi-Fi on, the game checks the latest release. If there is a newer one it offers to install it (A) or
skip (B): it replaces `rs64.3dsx`, or installs the release's CIA when you started the game from the HOME menu. Then
restart the game. An empty `no_update_check.txt` in `sdmc:/3ds/rs64/` turns the check off.

## Status (v0.2)

- Playable from boot through missions and cutscenes.
- Missions run roughly 12-25 fps, cutscenes faster; performance work is ongoing.
- Sound at 16000 Hz by default is recommended (see `audio_rate.txt` below).
- Widescreen (wider field of view), stereoscopic 3D, radar on the bottom screen.
- Quitting: because the game uses almost all of the New 3DS memory, the console may need a restart after you quit (from the HOME menu or with L + R + START + SELECT). That is normal.
- Known issues: the 3D logo at boot has a white sky; occasional frame drops on level loads.

## What you need

- A ROM dump of *Star Wars: Rogue Squadron* (USA, **v1.0**).
- A New 3DS with Luma3DS and the Homebrew Launcher, and `sdmc:/3ds/dspfirm.cdc` for sound (run the DSP1 homebrew once).
- Once, a Windows, Mac or Linux computer for the setup.

## Building by hand

`python port3ds/build_from_rom.py [rom] [--sd E:/] [--pick] [--send]` is what the setup runs: it updates the clone,
recompiles the game from your ROM and builds `game.bin` and `rs64.3dsx` into `build3ds/sdcard/3ds/rs64/`. It needs Git,
CMake, a C++ compiler, Python 3.11+, devkitPro (3ds-dev) and LLVM clang + lld. `python port3ds/build.py --port-only`
builds just `rs64.3dsx`, from nothing made from the ROM (that is what releases contain).

### CIA

If [makerom](https://github.com/3DSGuy/Project_CTR/releases) and [bannertool](https://github.com/Epicpkmn11/bannertool/releases)
are installed (on your PATH, or set `MAKEROM` / `BANNERTOOL`), the build also makes `rs64.cia`, which you install with FBI
to start the game from the HOME menu. Like `rs64.3dsx` it holds only the port; it loads `game.bin` from `sdmc:/3ds/rs64/`.

## Installing by hand

1. Copy `build3ds/sdcard/3ds/rs64/` (`rs64.3dsx`, `game.bin`, `README.txt`) to `sdmc:/3ds/rs64/`.
2. Put your ROM (any name, `.z64` / `.n64` / `.v64`) in the same folder. The first start checks it and keeps a verified copy.
3. Start *Rogue Squadron 64* from the Homebrew Launcher.

## Controls and options

| 3DS | N64 |
| --- | --- |
| Circle Pad | Stick |
| A / B | A (lasers) / B (secondary) |
| R, ZR / L / ZL | Z / R / L |
| X, Y, SELECT / C-stick | C buttons |
| D-pad, START | D-pad, START |
| L + R + START + SELECT | Quit |

- **Tap the bottom screen:** switch 4:3 / widescreen (saved in `aspect.txt`).
- **3D slider:** stereoscopic 3D (draws everything twice, so it costs speed).
- **`audio_rate.txt`** with a number (8000-48000): the sound rate. 16000 is recommended; lower rates speed up cutscenes but some high notes go off pitch.
- **`gfx_sync.txt`** (empty file): turns off drawing in parallel with the game, if you see graphics glitches.
- **L + R + tap:** writes a frame dump to `rs64_log.txt` for bug reports. Logs always go to `sdmc:/3ds/rs64/rs64_log.txt`.

## License

GPL-3.0, as the project it is based on. Star Wars: Rogue Squadron is the property of its owners; this project contains none of
the game's code or data.
