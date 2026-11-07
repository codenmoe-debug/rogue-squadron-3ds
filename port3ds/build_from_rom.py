#!/usr/bin/env python3
"""Builds rs64.3dsx from your own Rogue Squadron ROM, on Windows, macOS or Linux. Run it again to update.

    python port3ds/build_from_rom.py [path/to/rom.z64] [--sd E:/] [--serve] [--send [3DS-IP]] [--no-pull]

It updates this clone (git pull), applies the 3DS runtime patches, fetches the decomp next to this repo, imports the ROM,
generates the recompiled code (N64Recomp, RSPRecomp, the patches), then runs port3ds/build.py. The result is
build3ds/sdcard/3ds/rs64/ (rs64.3dsx, and rs64.cia when makerom and bannertool are installed). --sd copies it to an SD
card (its root) along with update_source.txt; --serve keeps it available on your network so the game installs it at its
next start; --send sends rs64.3dsx to a 3DS running the Homebrew Launcher (press Y there first). Nothing it downloads contains game code: that is made here, from your ROM.
"""
import argparse
import functools
import hashlib
import http.server
import socket
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DECOMP = REPO.parent / "rogue_squadron64"
DECOMP_URL = "https://github.com/MikeSemicolonD/rogue_squadron64.git"
ROM_SHA1 = "ed42eed1ee2db646ff7ef94ba8c5421d164a4f0d"
WIN = platform.system() == "Windows"
MAC = platform.system() == "Darwin"
EXE = ".exe" if WIN else ""
MIPS_CFLAGS = ["-mips2", "-mabi=32", "-O2", "-G0", "-mno-abicalls", "-fomit-frame-pointer", "-fno-builtin",
               "-fno-optimize-sibling-calls", "-fno-stack-protector", "-nostdinc", "-I", str(REPO / "syms/include"),
               "-Wno-unused-parameter"]


def step(msg):
    print(f"\n== {msg}", flush=True)


def fail(msg):
    sys.exit(f"\nBuild stopped: {msg}")


def run(cmd, cwd=REPO, env=None, check=True):
    cmd = [str(c) for c in cmd]
    r = subprocess.run(cmd, cwd=cwd, env=env)
    if check and r.returncode != 0:
        fail(f"{Path(cmd[0]).name} failed (exit {r.returncode})")
    return r.returncode


def quiet(cmd, cwd=REPO):
    return subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True)


# ------------------------------------------------------------------ tools
def install_hint(what):
    if WIN:
        return {
            "git": "install Git for Windows: https://git-scm.com/download/win",
            "cmake": "install CMake (https://cmake.org/download/) and Visual Studio Build Tools with 'Desktop development with C++'",
            "devkitpro": "install devkitPro with the 3DS component: https://github.com/devkitPro/installer/releases",
            "llvm": "install LLVM (the LLVM-*-win64.exe installer): https://github.com/llvm/llvm-project/releases",
        }[what]
    if MAC:
        return {
            "git": "run: xcode-select --install",
            "cmake": "run: brew install cmake  (and xcode-select --install for a C++ compiler)",
            "devkitpro": "install devkitPro (pkg installer), then: sudo dkp-pacman -S 3ds-dev  https://devkitpro.org/wiki/Getting_Started",
            "llvm": "run: brew install llvm lld",
        }[what]
    return {
        "git": "install git with your package manager",
        "cmake": "install cmake and a C++ compiler (e.g. sudo apt install cmake build-essential)",
        "devkitpro": "install devkitPro pacman, then: sudo dkp-pacman -S 3ds-dev  https://devkitpro.org/wiki/Getting_Started",
        "llvm": "install clang and lld (e.g. sudo apt install clang lld)",
    }[what]


def find_devkitpro():
    cands = []
    env = os.environ.get("DEVKITPRO", "")
    # on Windows the installer sets DEVKITPRO to an MSYS path (/opt/devkitpro), not usable from Python
    if env and not (WIN and env.startswith("/")):
        cands.append(Path(env))
    cands.append(Path("C:/devkitPro") if WIN else Path("/opt/devkitpro"))
    for c in cands:
        if c and (c / f"devkitARM/bin/arm-none-eabi-g++{EXE}").exists() and (c / "libctru/include/3ds.h").exists() \
                and (c / "libctru/include/citro3d.h").exists():
            return c
    fail("devkitPro with the 3DS libraries (3ds-dev) was not found: " + install_hint("devkitpro"))


def find_llvm():
    cands = []
    if os.environ.get("LLVM_ROOT"):
        cands.append(Path(os.environ["LLVM_ROOT"]))
    if WIN:
        cands.append(Path("C:/Program Files/LLVM"))
    elif MAC:
        cands += [Path("/opt/homebrew/opt/llvm"), Path("/usr/local/opt/llvm")]
    clang = shutil.which("clang")
    if clang and not MAC:
        cands.append(Path(clang).resolve().parents[1])
    for c in cands:
        if (c / f"bin/clang{EXE}").exists():
            # Apple's clang cannot build for the 3DS; LLVM's can
            out = quiet([c / f"bin/clang{EXE}", "--print-targets"]).stdout
            if "arm" in out and "mips" in out.lower():
                return c
    fail("LLVM clang with ARM and MIPS targets was not found (set LLVM_ROOT): " + install_hint("llvm"))


def find_lld(llvm):
    for p in [llvm / f"bin/ld.lld{EXE}", Path("/opt/homebrew/opt/lld/bin/ld.lld"), Path("/usr/local/opt/lld/bin/ld.lld")]:
        if p.exists():
            return p
    w = shutil.which("ld.lld")
    if w:
        return Path(w)
    fail("ld.lld (LLVM's linker) was not found: " + install_hint("llvm"))


def built(tool_dir, name):
    for p in [tool_dir / "Release" / f"{name}{EXE}", tool_dir / f"{name}{EXE}"]:
        if p.exists():
            return p
    fail(f"{name} was not built")


# ------------------------------------------------------------------ steps
PATCHES = [("lib/N64ModernRuntime", "port3ds/patches/N64ModernRuntime.patch"), ("lib/rt64", "port3ds/patches/rt64.patch")]


def patch_state(sub, patch):
    p = str(REPO / patch)
    if quiet(["git", "-C", REPO / sub, "apply", "--reverse", "--check", p]).returncode == 0:
        return "applied"
    if quiet(["git", "-C", REPO / sub, "apply", "--check", p]).returncode == 0:
        return "clean"
    return "conflict"


def update_sources(pull):
    step("Updating the sources")
    # take the 3DS patches off first so the submodules can move to new commits
    for sub, patch in PATCHES:
        if (REPO / sub / ".git").exists() and patch_state(sub, patch) == "applied":
            run(["git", "-C", REPO / sub, "apply", "--reverse", REPO / patch])
    if pull and (REPO / ".git").exists():
        if run(["git", "pull", "--ff-only"], check=False) != 0:
            print("git pull failed (local changes?); building what is here")
    # only what the 3DS build uses (the PC graphics libraries under rt64 are large and not needed)
    run(["git", "submodule", "update", "--init", "syms", "lib/rt64"])
    run(["git", "submodule", "update", "--init", "--recursive", "lib/N64ModernRuntime"])
    run(["git", "-C", REPO / "lib/rt64", "submodule", "update", "--init", "src/contrib/hlslpp", "src/contrib/xxHash"])
    for sub, patch in PATCHES:
        st = patch_state(sub, patch)
        if st == "clean":
            run(["git", "-C", REPO / sub, "apply", REPO / patch])
        elif st == "conflict":
            fail(f"{patch} does not apply to {sub}; report it on GitHub")


def import_rom(rom):
    step("Importing the ROM")
    if not (DECOMP / ".git").exists():
        run(["git", "clone", DECOMP_URL, DECOMP])
    have = DECOMP / "rogue_squadron.z64"
    ok = have.exists() and (REPO / "rogue_squadron.z64").exists() and hashlib.sha1(have.read_bytes()).hexdigest() == ROM_SHA1
    if ok and not rom:
        print("ROM already imported")
        return
    if not rom:
        rom = input("Path to your Star Wars: Rogue Squadron ROM (USA 1.0; .z64, .v64 or .n64): ").strip().strip("'\"")
    run([sys.executable, "-I", REPO / "tools/setup/import_rom.py", rom])


def generate(llvm, lld):
    step("Building the game ELF")
    run([sys.executable, REPO / "tools/re/make_elf_from_syms.py"])

    step("Building N64Recomp and RSPRecomp")
    tools = REPO / "build-tools"
    run(["cmake", "-S", REPO / "lib/N64ModernRuntime/N64Recomp", "-B", tools, "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", tools, "--config", "Release", "--target", "N64RecompCLI", "RSPRecomp", "--parallel"])
    helpers = REPO / "build-tools/helpers"
    run(["cmake", "-S", REPO / "port3ds/tools/helpers", "-B", helpers, "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", helpers, "--config", "Release", "--parallel"])
    n64recomp, rsprecomp = built(tools, "N64Recomp"), built(tools, "RSPRecomp")

    step("Recompiling the game code from your ROM")
    run([n64recomp, "rogue_squadron.toml"])
    run([rsprecomp, "rsp/factor5_boot_rsp.toml"])
    run([rsprecomp, "rsp/musyx_rsp.toml"])
    run([built(helpers, "fixup_factor5_ucode"), "build/factor5_ucode/factor5_boot_recompiled.c", "build/factor5_ucode/musyx_audio_recompiled.c"])
    run([built(helpers, "gen_state_table_tool"), "tools/state/state_model.toml", "src/game/state_table.inl"])

    step("Building the base-game patches")
    gcc = shutil.which("mips64-elf-gcc") or shutil.which("mips-linux-gnu-gcc")
    ld = shutil.which("mips64-elf-ld") or shutil.which("mips-linux-gnu-ld")
    if gcc and ld:
        cc, cflags = [gcc], []
    else:
        cc, cflags, ld = [llvm / f"bin/clang{EXE}", "--target=mips-linux-gnu"], ["-fno-pic", "-mno-odd-spreg"], lld
    obj_dir = REPO / "build-tools/patches"
    obj_dir.mkdir(parents=True, exist_ok=True)
    objs = []
    for src in sorted((REPO / "patches/src").rglob("*.c")):
        obj = obj_dir / (str(src.relative_to(REPO / "patches/src")).replace("/", "_").replace("\\", "_") + ".o")
        run(cc + MIPS_CFLAGS + cflags + ["-c", src, "-o", obj])
        objs.append(obj)
    run([ld, "-nostdlib", "-T", REPO / "patches/patches.ld", "-T", REPO / "patches/syms.ld", "--unresolved-symbols=ignore-all",
         "--emit-relocs", "--noinhibit-exec", "-e", "0"] + objs + ["-o", REPO / "patches/patches.elf"])
    run([n64recomp, "patches.toml"])


def build_3ds(dkp, llvm):
    step("Building the 3DS port")
    env = dict(os.environ, DEVKITPRO=str(dkp), LLVM_ROOT=str(llvm))
    run([sys.executable, REPO / "port3ds/build.py", "--no-profile"], env=env)


def deliver(sd, send, dkp):
    out = REPO / "build3ds/sdcard/3ds/rs64"
    if sd:
        dst = Path(sd) / "3ds" / "rs64"
        dst.mkdir(parents=True, exist_ok=True)
        for f in out.iterdir():
            shutil.copy2(f, dst / f.name)
        print(f"Copied to {dst}")
    if send is not None:
        cmd = [dkp / f"tools/bin/3dslink{EXE}", out / "rs64.3dsx"]
        if send:
            cmd[1:1] = ["-a", send]
        print("Sending to the 3DS (open the Homebrew Launcher and press Y)...")
        run(cmd, check=False)


SERVE_PORT = 8364


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.0.1", 9))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def write_update_source(folder):
    (folder / "update_source.txt").write_text(f"{lan_ip()}:{SERVE_PORT}\n")


def serve():
    out = REPO / "build3ds/sdcard/3ds/rs64"
    step("Serving the build")
    print(f"Address: {lan_ip()}:{SERVE_PORT}  (the game reads it from sdmc:/3ds/rs64/update_source.txt)")
    print("Start the game on the 3DS: it offers to install this build. Press Ctrl+C here when done.")
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(out))
    with http.server.ThreadingHTTPServer(("0.0.0.0", SERVE_PORT), handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


def main():
    ap = argparse.ArgumentParser(description="Build rs64.3dsx from your own ROM (run again to update).")
    ap.add_argument("rom", nargs="?", help="your ROM (.z64, .v64 or .n64); asked for when not imported yet")
    ap.add_argument("--sd", help="SD card root to copy 3ds/rs64/ to")
    ap.add_argument("--send", nargs="?", const="", help="send rs64.3dsx to the Homebrew Launcher over Wi-Fi (optional 3DS IP)")
    ap.add_argument("--no-pull", action="store_true", help="build without updating this clone first")
    ap.add_argument("--serve", action="store_true", help="after building, serve it on your network for the in-game updater")
    a = ap.parse_args()

    if sys.version_info < (3, 11):
        fail("Python 3.11 or newer is needed")
    for tool, hint in [("git", "git"), ("cmake", "cmake")]:
        if not shutil.which(tool):
            fail(f"{tool} was not found: " + install_hint(hint))
    dkp, llvm = find_devkitpro(), find_llvm()
    lld = find_lld(llvm)

    update_sources(not a.no_pull)
    import_rom(a.rom)
    generate(llvm, lld)
    build_3ds(dkp, llvm)
    write_update_source(REPO / "build3ds/sdcard/3ds/rs64")
    deliver(a.sd, a.send, dkp)
    step("Done")
    print(f"Copy {REPO / 'build3ds/sdcard/3ds/rs64'} to sdmc:/3ds/rs64/ (your ROM goes in that folder too), "
          "then start Rogue Squadron 64 from the Homebrew Launcher (or install rs64.cia with FBI).")
    if a.serve:
        serve()


if __name__ == "__main__":
    main()
