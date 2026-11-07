#!/bin/bash
# Rogue Squadron 64 for New 3DS: one-time setup on a Mac. Installs the build tools (only what is missing, asking first),
# then builds the game from your own ROM and copies it to your SD card.
set -e
ask() { read -r -p "$1 [Y/n] " r; [[ -z "$r" || "$r" =~ ^[Yy] ]]; }
echo "Rogue Squadron 64 for New 3DS - one-time setup"
echo

if ! xcode-select -p > /dev/null 2>&1; then
    echo "Apple's Command Line Tools are needed; a window opens to install them."
    xcode-select --install || true
    read -r -p "Press Return when they have finished installing."
fi
if ! command -v brew > /dev/null 2>&1 && [ ! -x /opt/homebrew/bin/brew ] && [ ! -x /usr/local/bin/brew ]; then
    ask "Homebrew (the Mac package manager) is needed. Install it?" || exit 1
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi
eval "$(/opt/homebrew/bin/brew shellenv 2> /dev/null || /usr/local/bin/brew shellenv)"
missing=()
for p in git cmake python@3.13 python-tk@3.13 llvm lld; do brew list "$p" > /dev/null 2>&1 || missing+=("$p"); done
if [ ${#missing[@]} -gt 0 ]; then
    ask "Install ${missing[*]} with Homebrew?" || exit 1
    brew install "${missing[@]}"
fi
if [ ! -x /opt/devkitpro/devkitARM/bin/arm-none-eabi-g++ ]; then
    ask "devkitPro (the 3DS toolchain) is needed. Install it? (asks for your password)" || exit 1
    if [ ! -x /opt/devkitpro/pacman/bin/pacman ]; then
        curl -fL -o /tmp/devkitpro-pacman.pkg https://github.com/devkitPro/pacman/releases/latest/download/devkitpro-pacman-installer.pkg
        sudo installer -pkg /tmp/devkitpro-pacman.pkg -target /
    fi
    sudo /opt/devkitpro/pacman/bin/pacman -Sy --needed --noconfirm 3ds-dev
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$DIR/../.."
if [ ! -f "$REPO/port3ds/build_from_rom.py" ]; then
    REPO="$HOME/rogue-squadron-3ds"
    [ -d "$REPO/.git" ] || git clone https://github.com/yaboimanguy-a11y/rogue-squadron-3ds "$REPO"
fi
"$(brew --prefix python@3.13)/bin/python3.13" "$REPO/port3ds/build_from_rom.py" --pick
read -r -p "Press Return to close."
