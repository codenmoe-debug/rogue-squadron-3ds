# Rogue Squadron 64 Recompiled - New Nintendo 3DS port (v0.1)

**New Nintendo 3DS / New 3DS XL / New 2DS XL only.** The original 3DS and 2DS are too slow.

A port of [Rogue Squadron 64 Recompiled](https://github.com/MikeSemicolonD/RogueSquadron64Recomp) to the New 3DS homebrew
environment: the recompiled game runs natively on the ARM11, with its own citro3d renderer for the Factor 5 microcode, the
recompiled MusyX synth for sound, and the radar on the bottom screen.

**No game files are included, and no prebuilt 3dsx is released.** The game code is recompiled from your own ROM, so you build
the 3dsx yourself, and it stays personal: do not share it.

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

## Building

1. Clone with submodules and apply the 3DS runtime changes:
   ```bash
   git clone --recursive https://github.com/yaboimanguy-a11y/rogue-squadron-3ds
   cd rogue-squadron-3ds
   git -C lib/N64ModernRuntime apply ../../port3ds/patches/N64ModernRuntime.patch
   git -C lib/rt64 apply ../../port3ds/patches/rt64.patch
   ```
2. Generate the recompiled sources exactly as the desktop project does (its README covers each step):
   - `RecompiledFuncs/` from the decomp ELF with N64Recomp (`regen_funcs`),
   - `RecompiledPatches/` and `patches/patches.bin` (the patches ELF; needs a MIPS GCC),
   - `build/factor5_ucode/` (RSPRecomp of `rsp/*.toml`, then `tools/fixup_factor5_ucode.cpp`),
   - `src/game/state_table.inl` (`gen_state_table`).

   The simplest route is to configure and build the desktop project once with CMake; it produces all of these.
3. Build the 3DS port:
   ```bash
   python3 port3ds/build.py
   ```
   Output: `build3ds/sdcard/3ds/rs64/` with `rs64.3dsx` and `README.txt`. `--no-profile` drops the sampling profiler.

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
