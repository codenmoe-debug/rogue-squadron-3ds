#!/bin/bash
# Rogue Squadron 64 for New 3DS: one-time setup on Linux (Debian/Ubuntu; Python 3.11+, so Ubuntu 24.04 or newer).
# Installs the build tools (only what is missing, asking first), then builds the game from your own ROM and copies it
# to your SD card.
set -e
ask() { read -r -p "$1 [Y/n] " r; [[ -z "$r" || "$r" =~ ^[Yy] ]]; }
echo "Rogue Squadron 64 for New 3DS - one-time setup"
echo

if command -v apt-get > /dev/null; then
    pkgs=(git cmake build-essential python3 python3-tk clang lld curl)
    missing=()
    for p in "${pkgs[@]}"; do dpkg -s "$p" > /dev/null 2>&1 || missing+=("$p"); done
    if [ ${#missing[@]} -gt 0 ]; then
        ask "Install ${missing[*]}? (asks for your password)" || exit 1
        sudo apt-get update && sudo apt-get install -y "${missing[@]}"
    fi
else
    echo "Not an apt system: install git, cmake, a C++ compiler, Python 3.11+ with tkinter, clang and lld yourself."
fi
if [ ! -x /opt/devkitpro/devkitARM/bin/arm-none-eabi-g++ ]; then
    ask "devkitPro (the 3DS toolchain) is needed. Install it? (asks for your password)" || exit 1
    if ! command -v dkp-pacman > /dev/null; then
        curl -fL -o /tmp/install-devkitpro-pacman https://apt.devkitpro.org/install-devkitpro-pacman
        chmod +x /tmp/install-devkitpro-pacman
        sudo /tmp/install-devkitpro-pacman
    fi
    sudo dkp-pacman -Sy --needed --noconfirm 3ds-dev
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$DIR/../.."
if [ ! -f "$REPO/port3ds/build_from_rom.py" ]; then
    REPO="$HOME/rogue-squadron-3ds"
    [ -d "$REPO/.git" ] || git clone https://github.com/yaboimanguy-a11y/rogue-squadron-3ds "$REPO"
fi
python3 "$REPO/port3ds/build_from_rom.py" --pick
