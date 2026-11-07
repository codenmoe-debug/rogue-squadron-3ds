# Rogue Squadron 64 Recompiled - New Nintendo 3DS port (v0.1)

**New Nintendo 3DS / New 3DS XL / New 2DS XL only.** The original 3DS and 2DS are too slow.

A port of [Rogue Squadron 64 Recompiled](https://github.com/MikeSemicolonD/RogueSquadron64Recomp) to the New 3DS homebrew
environment: the recompiled game runs natively on the ARM11, with its own citro3d renderer for the Factor 5 microcode, the
recompiled MusyX synth for sound, and the radar on the bottom screen.

**No game files and no prebuilt 3dsx are included.** The game code is recompiled from your own ROM, so you build the 3dsx
yourself (below), and it stays personal: do not share it.

## Status (v0.1)

- Playable from boot through missions and cutscenes.
- Missions run roughly 12-25 fps, cutscenes faster; performance work is ongoing.
- Sound at 16000 Hz by default is recommended (see `audio_rate.txt` below).
- Widescreen (wider field of view), stereoscopic 3D, radar on the bottom screen.
- Quitting: because the game uses almost all of the New 3DS memory, the console may need a restart after you quit (from the HOME menu or with L + R + START + SELECT). That is normal.
- Known issues: the 3D logo at boot has a white sky; occasional frame drops on level loads.

## What you need

- A ROM dump of *Star Wars: Rogue Squadron* (USA, **v1.0**), xxHash3-64 `0x6B66A44153594DEA`.
- macOS or Linux with:
  - [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` group (devkitARM, libctru, citro3d, picasso, 3dsxtool, smdhtool)
  - LLVM clang with ARM support (Homebrew `llvm` on macOS; set `LLVM_ROOT` if it is elsewhere)
  - Python 3, CMake, and a host C++ compiler (for N64Recomp / RSPRecomp)
- A New 3DS with the Homebrew Launcher (Luma3DS custom firmware) and `sdmc:/3ds/dspfirm.cdc` for sound (run the DSP1 homebrew once).

## Building it the easy way (Windows, macOS, Linux)

Install once:

| | Windows | macOS | Linux |
| --- | --- | --- | --- |
| Git, CMake, a C++ compiler | Git for Windows, CMake, Visual Studio Build Tools (C++) | `xcode-select --install`, `brew install cmake` | `sudo apt install git cmake build-essential` |
| Python 3.11+ | python.org | `brew install python` | your package manager |
| devkitPro (3DS) | [installer](https://github.com/devkitPro/installer/releases), tick 3DS | [pkg](https://devkitpro.org/wiki/Getting_Started), then `sudo dkp-pacman -S 3ds-dev` | dkp-pacman, then `sudo dkp-pacman -S 3ds-dev` |
| LLVM (clang + lld) | [LLVM installer](https://github.com/llvm/llvm-project/releases) | `brew install llvm lld` | `sudo apt install clang lld` |

Then, from this repository:

```bash
python port3ds/build_from_rom.py path/to/your/rom.z64
```

It updates the clone, prepares everything, recompiles the game from your ROM and builds the port into
`build3ds/sdcard/3ds/rs64/`. Run the same command again (the ROM path is only needed the first time) to update.

- `--sd E:/` copies the result onto your SD card.
- `--serve` keeps the build available on your network for the in-game updater (below).
- `--send` sends `rs64.3dsx` straight to the Homebrew Launcher over Wi-Fi (press Y in the launcher first).

### CIA

If [makerom](https://github.com/3DSGuy/Project_CTR/releases) and [bannertool](https://github.com/Epicpkmn11/bannertool/releases)
are installed (on your PATH, or set `MAKEROM` / `BANNERTOOL`), the build also makes `rs64.cia`, which you install with FBI
to start the game from the HOME menu. Like the 3dsx, it is made from your ROM: keep it to yourself.

## Updates

At each start (with Wi-Fi on) the game:

1. checks this repository's latest release and tells you when a newer version is out;
2. if `update_source.txt` (written by the builder) points at your PC and `build_from_rom.py --serve` is running there with
   a different build, offers to **install it**: it replaces `rs64.3dsx`, or installs `rs64.cia` through the system when
   you started the game from the HOME menu. Restart the game to use it.

So updating is: run `python port3ds/build_from_rom.py --serve` on your PC, then start the game on the 3DS and press A.
An empty `no_update_check.txt` in `sdmc:/3ds/rs64/` turns both checks off.

## Installing

1. Copy `build3ds/sdcard/3ds/rs64/` to `sdmc:/3ds/rs64/`.
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
