#!/usr/bin/env python3
"""build.py [--jobs N] [--clean]: build the New Nintendo 3DS port (rs64.3dsx) into build3ds/.

Needs: devkitPro (3ds-dev: devkitARM, libctru, citro3d, picasso, 3dsxtool, smdhtool), LLVM clang
(Homebrew llvm), and the generated sources the desktop build also uses: RecompiledFuncs/ (N64Recomp),
RecompiledPatches/ + patches/patches.bin, build/factor5_ucode/ (RSPRecomp + fixup) and
src/game/state_table.inl. See port3ds/README.md (apply port3ds/patches/*.patch to the submodules first).

Output: build3ds/sdcard/3ds/rs64/ (rs64.3dsx + README.txt), ready to copy to the SD card.
"""
import argparse
import concurrent.futures as cf
import struct
import time
import zlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# --gpr32 (experimental, broken in testing: garbled geometry and sound): 32-bit registers with the
# 64-bit functions split out, built as rs64_gpr32.3dsx. The default build keeps 64-bit registers.
SAFE = "--gpr32" not in sys.argv
OUT = ROOT / ("build3ds" if SAFE else "build3ds_gpr32")
SD_DIR = ROOT / "build3ds/sdcard/3ds/rs64"
GPR = [] if SAFE else ["-DRECOMP_GPR32"]
# --no-profile: without the core-0 sampling profiler (port3ds/prof_3ds.cpp, instrument_profile)
PROFILE = "--no-profile" not in sys.argv
PROF = ["-DRS3DS_PROFILE"] if PROFILE else []
DKP = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))
DKA = DKP / "devkitARM"
LLVM = Path(os.environ.get("LLVM_ROOT", "/opt/homebrew/opt/llvm"))
CLANG = str(LLVM / "bin/clang")
GXX = str(DKA / "bin/arm-none-eabi-g++")
GCC = str(DKA / "bin/arm-none-eabi-gcc")
RT = ROOT / "lib/N64ModernRuntime"
RT64 = ROOT / "lib/rt64"

BUILD_ID = time.strftime("%Y%m%d-%H%M%S")

ARCH_GCC = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft"]
ARCH_CLANG = ["--target=armv6k-none-eabihf", "-mcpu=mpcore", "-mfloat-abi=hard", "-mfpu=vfpv2", "-marm", "-mtp=soft"]
# int32_t as int (not newlib's long): the desktop code assumes it in std::clamp/std::max calls
INT32 = ["-U__INT32_TYPE__", "-D__INT32_TYPE__=int", "-U__UINT32_TYPE__", "-D__UINT32_TYPE__=unsigned int",
         "-U__INT_LEAST32_TYPE__", "-D__INT_LEAST32_TYPE__=int", "-U__UINT_LEAST32_TYPE__", "-D__UINT_LEAST32_TYPE__=unsigned int"]
INCS = [ROOT / "port3ds/include", ROOT / "include", ROOT / "src", OUT / "gen", ROOT / "RecompiledFuncs",
        RT / "ultramodern/include", RT / "librecomp/include", RT / "librecomp/include/librecomp", RT / "librecomp/src",
        RT / "N64Recomp/include", RT / "thirdparty", RT / "thirdparty/concurrentqueue", RT / "thirdparty/o1heap",
        RT / "thirdparty/miniz", RT / "N64Recomp/lib/rabbitizer/include", RT / "N64Recomp/lib/rabbitizer/cplusplus/include",
        RT / "N64Recomp/lib/sljit/sljit_src", RT / "N64Recomp/lib/tomlplusplus/include", RT / "N64Recomp/lib/fmt/include",
        RT / "N64Recomp/lib/ELFIO", RT64 / "src", RT64 / "include", RT64 / "src/contrib", RT64 / "src/contrib/hlslpp/include",
        DKP / "libctru/include"]
DEFS = ["-D__3DS__", "-DNDEBUG", "-DSSE2NEON_SUPPRESS_WARNINGS"] + GPR + PROF

CXXFLAGS = ARCH_GCC + INT32 + DEFS + ["-std=gnu++20", "-O2", "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections",
                                       "-Wno-unused-parameter", "-Wno-psabi"]
# recompiled game code: clang (fast on the huge generated functions), as the MvC2 port
GAME_PROF = PROF + (["-include", str(ROOT / "port3ds/include/rs3ds_prof.h")] if PROFILE else [])
GAME_CFLAGS = ARCH_CLANG + GPR + ["-D__3DS__", "-DNDEBUG", "-O2", "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections", "-fshort-enums",
                            "-Wno-unused-variable", "-Wno-implicit-function-declaration", "-Dmain=rs_main",
                            "-isystem", str(DKA / "arm-none-eabi/include"), "-include", str(ROOT / "include/mem_wu.h")]
# game.bin: position independent, only rs3ds_module_entry visible (port3ds/module/module.ver)
MOD_FLAGS = ["-fPIC", "-fvisibility=hidden"]
IMPORTS = ROOT / "port3ds/module/imports.txt"
HOST_CFLAGS = ARCH_GCC + DEFS + ["-std=gnu11", "-O2", "-ffunction-sections", "-fdata-sections"]


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(" ".join(str(c) for c in cmd) + "\n" + r.stdout + r.stderr)
        raise SystemExit(f"build3ds: failed: {cmd[-1]}")
    return r


def stale(out, deps):
    if not out.exists():
        return True
    t = out.stat().st_mtime
    return any(Path(d).exists() and Path(d).stat().st_mtime > t for d in deps)


MODULE_HOST = ["src/game/register_overlays.cpp", "src/game/register_patches.cpp"]


RECOMP_DEF = re.compile(r"^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{", re.M)


def port_overrides(port_objs):
    """Recompiled functions the port replaces with its own (zmemcpy): the desktop link lets the port's copy win; in
    game.bin the recompiled copy is renamed away so its callers import the port's."""
    nm = str(DKA / "bin/arm-none-eabi-nm")
    rsp = OUT / "objs_nm.rsp"
    rsp.write_text("\n".join(f'"{o}"' for o in port_objs))
    out = run([nm, "-g", "--defined-only", f"@{rsp}"]).stdout
    # the game's own main is compiled as rs_main (-Dmain=rs_main), so the port's main is not an override
    return {l.split()[-1] for l in out.splitlines() if len(l.split()) == 3} - {"main"}


def module_funcs(overrides):
    """RecompiledFuncs/*.c, with the definitions of the port's overrides renamed (copies in build3ds/gen/mod)."""
    out = []
    gen = OUT / "gen/mod"
    gen.mkdir(parents=True, exist_ok=True)
    for src in sorted((ROOT / "RecompiledFuncs").glob("*.c")):
        text = src.read_text()
        hit = [n for n in RECOMP_DEF.findall(text) if n in overrides]
        if not hit:
            out.append(src)
            continue
        for n in hit:
            text = text.replace(f"RECOMP_FUNC void {n}(uint8_t* rdram, recomp_context* ctx) {{",
                                f"__attribute__((unused)) static void {n}__replaced_by_port(uint8_t* rdram, recomp_context* ctx) {{")
        dst = gen / src.name
        if not dst.exists() or dst.read_text() != text:
            dst.write_text(text)
        print(f"build3ds: game.bin uses the port's {', '.join(hit)}")
        out.append(dst)
    return out


def module_sources(overrides=frozenset()):
    """(source, kind) for game.bin: the code recompiled from the ROM plus the tables that name it. Built on the player's
    computer only; never part of rs64.3dsx."""
    s = [(p, "game") for p in module_funcs(overrides)]
    s += [(p, "cxx") for p in sorted((ROOT / "RecompiledFuncs").glob("*.cpp"))]
    s += [(ROOT / "src/game/toml_hooks.c", "game"), (ROOT / "RecompiledPatches/patches.c", "game"), (OUT / "gen/patches_bin.c", "game")]
    s += [(ROOT / "build/factor5_ucode/factor5_boot_recompiled.c", "cxx"), (ROOT / "build/factor5_ucode/musyx_audio_recompiled.c", "cxx")]
    s += [(ROOT / p, "cxx") for p in MODULE_HOST]
    s += [(ROOT / "port3ds/module/module_entry.cpp", "cxx")]
    return s


def sources():
    """(source, kind) for rs64.3dsx, the port: no code from the ROM; kind: cxx, c, asm."""
    s = []
    skip = {"mods.cpp", "mod_events.cpp", "mod_hooks.cpp", "mod_manifest.cpp", "mod_config_api.cpp", "mod_data_api.cpp", "task_win32.cpp"}
    s += [(p, "cxx") for p in sorted((RT / "ultramodern/src").glob("*.cpp")) if p.name not in skip]
    s += [(p, "cxx") for p in sorted((RT / "librecomp/src").glob("*.cpp")) if p.name not in skip]
    s += [(RT / "thirdparty/o1heap/o1heap/o1heap.c", "c"), (RT / "thirdparty/miniz/miniz.c", "c"), (RT / "thirdparty/miniz/miniz_tinfl.c", "c"), (RT / "thirdparty/miniz/miniz_tdef.c", "c")]
    host = ["src/game/builtin_hooks.cpp",
            "src/game/nav_sequencer.cpp", "src/game/static_standins.cpp", "src/game/game_state.cpp",
            "src/game/hook_helpers.cpp", "src/game/upstream_compat.cpp", "src/game/mod_content/mod_level_hooks.cpp",
            "src/game/mod_content/asset_overrides.cpp", "src/game/lang/lang.cpp", "src/lockstep/lockstep_core.cpp",
            "src/lockstep/lockstep.cpp", "src/host_api/host_api.cpp", "src/audio/audio.cpp"]
    s += [(ROOT / p, "cxx") for p in host]
    s += [(p, "cxx") for p in sorted((ROOT / "port3ds").glob("*.cpp"))]
    s += [(OUT / "gen/rs_shbin.c", "c"), (OUT / "gen/host_exports.S", "asm")]
    return s


def obj_path(src, mod=False):
    rel = os.path.relpath(src, ROOT).replace("/", "__").replace("..", "up")
    return OUT / ("objm" if mod else "obj") / (rel + ".o")


def deps_of(obj):
    """headers the last compile of obj read (its -MMD .d file)"""
    d = obj.with_suffix(".d")
    if not d.exists():
        return []
    text = d.read_text().replace("\\\n", " ")
    return [t.replace("\\ ", " ") for t in re.findall(r"(?:\\ |\S)+", text.split(":", 1)[1])] if ":" in text else []


def compile_one(src, kind, mod=False):
    obj = obj_path(src, mod)
    if not stale(obj, [src, __file__] + deps_of(obj)):
        return obj
    obj.parent.mkdir(parents=True, exist_ok=True)
    incs = [f"-I{i}" for i in INCS]
    if kind in ("game", "game64"):
        flags = GAME_CFLAGS if kind == "game" else [f for f in GAME_CFLAGS if f != "-DRECOMP_GPR32"]
        flags = flags + (MOD_FLAGS if mod else GAME_PROF)
        cmd = [CLANG] + flags + [f"-I{i}" for i in INCS if "libctru" not in str(i)] + ["-c", src, "-o", obj]
    elif kind == "cxx":
        lang = ["-x", "c++"] if str(src).endswith(".c") else []
        # the recompiled RSP audio microcode (MusyX synth) is the hottest code after the game: -O3
        hot = ["-O3"] if "factor5_ucode" in str(src) else []
        # the 3DS renderer is the frame-rate limit in missions (core 1)
        if str(src).endswith("renderer_3ds.cpp"): hot = ["-O3", "-ffast-math"]
        cmd = [GXX] + CXXFLAGS + hot + (MOD_FLAGS if mod else []) + incs + lang + ["-c", src, "-o", obj]
    elif kind == "asm":
        cmd = [GCC] + ARCH_GCC + ["-c", src, "-o", obj]
    else:
        cmd = [GCC] + HOST_CFLAGS + incs + ["-c", src, "-o", obj]
    run(cmd[:1] + ["-MMD", "-MF", obj.with_suffix(".d")] + cmd[1:])
    return obj


def gen():
    g = OUT / "gen"
    g.mkdir(parents=True, exist_ok=True)
    pica = ROOT / "port3ds/rs.v.pica"
    shbin = g / "rs.shbin"
    if stale(g / "rs_shbin.c", [pica]):
        run([DKP / "tools/bin/picasso", "-o", shbin, pica])
        data = shbin.read_bytes()
        body = ",\n".join("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) for i in range(0, len(data), 16))
        (g / "rs_shbin.c").write_text(f"const unsigned char rs_shbin[] __attribute__((aligned(4))) = {{\n{body}\n}};\n"
                                      f"const unsigned int rs_shbin_size = {len(data)};\n")
    # a new id every build: the in-game updater compares it with the build your PC serves (build_from_rom.py --serve)
    (g / "rs3ds_build_id.h").write_text(f'#define RS3DS_BUILD_ID "{BUILD_ID}"\n')
    pb = ROOT / "patches/patches.bin"
    if pb.exists() and stale(g / "patches_bin.c", [pb]):
        run([sys.executable, ROOT / "port3ds/tools/file_to_c.py", pb, "rs64_patches_bin", g / "patches_bin.c", g / "patches_bin.h"])


GPR64_OPS = re.compile(r"^\s*// 0x[0-9A-F]+: (dsll|dsrl|dsra|dsll32|dsrl32|dsra32|dadd|daddu|daddi|daddiu|dsub|dsubu|dmult|dmultu|ddiv|ddivu|"
                       r"ld|sd|ldl|ldr|sdl|sdr|dsllv|dsrlv|dsrav|lwu|dmfc1|dmtc1|lld|scd)\s")
CALL_LINE = re.compile(r"^(\s*)(\{ )?(\w+)\(rdram, ctx\);( \})?\s*$")


def split_gpr64():
    """The game runs with 32-bit registers (RECOMP_GPR32); the few functions that use 64-bit MIPS
    instructions go to gen/recomp/funcs64.c, built with 64-bit registers. Each keeps its name and the
    32-bit context: it copies the registers in (sign-extended) and out, and around every call it makes."""
    out = OUT / "gen/recomp"
    out.mkdir(parents=True, exist_ok=True)
    srcs = sorted((ROOT / "RecompiledFuncs").glob("funcs_*.c"))
    stamp = out / "funcs64.c"
    if not stale(stamp, srcs + [Path(__file__)]):
        return
    moved, callees = [], set()
    for src in srcs:
        lines = src.read_text().splitlines(keepends=True)
        keep, i = [], 0
        while i < len(lines):
            m = re.match(r"^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{", lines[i])
            if not m:
                keep.append(lines[i]); i += 1; continue
            j = i
            while not lines[j].startswith(";}"):
                j += 1
            body = lines[i:j + 1]
            if any(GPR64_OPS.match(l) for l in body):
                moved.append((m.group(1), body))
            else:
                keep.extend(body)
            i = j + 1
        (out / src.name).write_text("".join(keep))
    parts = ["// generated by port3ds/build.py (split_gpr64): the functions that use 64-bit MIPS registers\n",
             "#include <string.h>\n#include \"recomp.h\"\n\n",
             "typedef struct { uint32_t r[32]; fpr f[32]; uint64_t hi, lo; uint32_t* f_odd; uint32_t status_reg; uint8_t mips3_float_mode; } recomp_context32;\n\n",
             "static inline void ctx_from32(const recomp_context32* a, recomp_context* b) {\n",
             "    gpr* d = &b->r0;\n    for (int i = 0; i < 32; i++) d[i] = (gpr)(int64_t)(int32_t)a->r[i];\n",
             "    memcpy(&b->f0, a->f, sizeof a->f);\n    b->hi = a->hi; b->lo = a->lo; b->status_reg = a->status_reg; b->mips3_float_mode = a->mips3_float_mode;\n",
             "    const char* fo = (const char*)a->f_odd;\n",
             "    b->f_odd = (fo >= (const char*)a->f && fo < (const char*)(a->f + 32)) ? (uint32_t*)((char*)&b->f0 + (fo - (const char*)a->f)) : a->f_odd;\n}\n\n",
             "static inline void ctx_to32(const recomp_context* b, recomp_context32* a) {\n",
             "    const gpr* s = &b->r0;\n    for (int i = 0; i < 32; i++) a->r[i] = (uint32_t)s[i];\n",
             "    memcpy(a->f, &b->f0, sizeof a->f);\n    a->hi = b->hi; a->lo = b->lo; a->status_reg = b->status_reg; a->mips3_float_mode = b->mips3_float_mode;\n}\n\n",
             "// After a call: a register whose low half the callee did not change keeps its 64-bit value (registers\n",
             "// the callee never touched are unchanged on hardware too); a changed one takes the new value, sign-extended.\n",
             "static inline void ctx_merge32(const recomp_context32* a, recomp_context* b, const uint32_t pre[32]) {\n",
             "    gpr* d = &b->r0;\n    for (int i = 0; i < 32; i++) if (a->r[i] != pre[i]) d[i] = (gpr)(int64_t)(int32_t)a->r[i];\n",
             "    memcpy(&b->f0, a->f, sizeof a->f);\n    b->hi = a->hi; b->lo = a->lo; b->status_reg = a->status_reg; b->mips3_float_mode = a->mips3_float_mode;\n}\n\n"]
    bodies = []
    for name, body in moved:
        out_body = [f"static void {name}__g64(uint8_t* rdram, recomp_context* ctx, recomp_context32* ctx32) {{\n"]
        for l in body[1:]:
            if "(rdram, ctx)" in l:
                cm = CALL_LINE.match(l)
                if not cm:
                    raise SystemExit(f"build3ds: split_gpr64: unhandled call in {name}: {l.strip()}")
                callees.add(cm.group(3))
                l = (f"{cm.group(1)}{{ uint32_t pre_[32]; ctx_to32(ctx, ctx32); memcpy(pre_, ctx32->r, sizeof pre_); "
                     f"{cm.group(3)}(rdram, ctx32); ctx_merge32(ctx32, ctx, pre_); }}\n")
            out_body.append(l)
        out_body.append(f"void {name}(uint8_t* rdram, recomp_context32* ctx32) {{\n"
                        f"    recomp_context c;\n    ctx_from32(ctx32, &c);\n    {name}__g64(rdram, &c, ctx32);\n    ctx_to32(&c, ctx32);\n}}\n\n")
        bodies.append("".join(out_body))
    decls = "".join(f"void {c}(uint8_t* rdram, recomp_context32* ctx);\n" for c in sorted(callees))
    stamp.write_text("".join(parts) + decls + "\n" + "".join(bodies))
    print(f"build3ds: {len(moved)} functions keep 64-bit registers: " + ", ".join(n for n, _ in moved), flush=True)


def instrument_profile():
    """Copies of RecompiledFuncs with every function wrapped in RS3DS_PROF_ENTER(vram) / RS3DS_PROF_LEAVE()
    (include/rs3ds_prof.h), for the core-0 sampler."""
    out = OUT / "gen/prof"
    out.mkdir(parents=True, exist_ok=True)
    head = re.compile(r"^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{")
    addr = re.compile(r"// 0x([0-9A-F]{8}):")
    for src in sorted((ROOT / "RecompiledFuncs").glob("funcs_*.c")):
        dst = out / src.name
        if not stale(dst, [src, Path(__file__)]):
            continue
        lines = src.read_text().splitlines(keepends=True)
        res, i = [], 0
        while i < len(lines):
            if not head.match(lines[i]):
                res.append(lines[i]); i += 1; continue
            j = i
            while not lines[j].startswith(";}"):
                j += 1
            vram = next((addr.search(l).group(1) for l in lines[i:j] if addr.search(l)), "00000000")
            res.append(lines[i])
            res.append(f"    RS3DS_PROF_ENTER(0x{vram}u);\n")
            for l in lines[i + 1:j]:
                res.append(l.replace("return;", "{ RS3DS_PROF_LEAVE(); return; }"))
            res.append("    RS3DS_PROF_LEAVE();\n")
            res.append(lines[j])
            i = j + 1
        dst.write_text("".join(res))


def link(objs):
    elf = OUT / "rs64.elf"
    if not stale(elf, objs):
        return elf
    ld = [GXX, "-specs=3dsx.specs"] + ARCH_GCC + ["-Wl,--gc-sections", "-Wl,--wrap=__syscall_thread_create", "-Wl,--allow-multiple-definition",
                                                 f"-Wl,-Map,{OUT / 'rs64.map'}"]
    rsp = OUT / "objs.rsp"
    rsp.write_text("\n".join(f'"{o}"' for o in objs))
    run(ld + [f"@{rsp}", "-o", elf, f"-L{DKP / 'libctru/lib'}", "-lcitro3d", "-lctru", "-lm"])
    return elf


def package(elf):
    smdh = OUT / "rs64.smdh"
    run([DKP / "tools/bin/smdhtool", "--create", "Rogue Squadron 64" if SAFE else "Rogue Squadron 64 (gpr32 test)", "Star Wars: Rogue Squadron (New 3DS)", "RS64 Recomp 3DS port",
         DKP / "libctru/default_icon.png", smdh])
    x = OUT / "rs64.3dsx"
    run([DKP / "tools/bin/3dsxtool", elf, x, f"--smdh={smdh}"])
    sd = SD_DIR
    sd.mkdir(parents=True, exist_ok=True)
    shutil.copy2(x, sd / ("rs64.3dsx" if SAFE else "rs64_gpr32.3dsx"))
    shutil.copy2(ROOT / "port3ds/README.txt", sd / "README.txt")
    (sd / "build_id.txt").write_text(BUILD_ID + "\n")
    if SAFE:
        make_cia(elf, smdh, sd)
    return sd


def plain_png(path, w, h):
    """A plain dark-blue gradient banner image (no artwork from the game)."""
    rows = b"".join(b"\x00" + bytes([(10, 14 + y // 4, 40 + y // 2)[c] for x in range(w) for c in range(3)]) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def silent_wav(path):
    n = 16000
    path.write_bytes(b"RIFF" + struct.pack("<I", 36 + n * 2) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16) +
                     b"data" + struct.pack("<I", n * 2) + bytes(n * 2))


def find_tool(name):
    for p in [os.environ.get(name.upper()), shutil.which(name), str(ROOT / "build-tools/cia" / name),
              str(ROOT / "build-tools/cia" / (name + ".exe"))]:
        if p and Path(p).exists():
            return p
    return None


def make_cia(elf, smdh, sd):
    """rs64.cia (installs to the HOME menu) when makerom and bannertool are available; skipped otherwise."""
    makerom, bannertool = find_tool("makerom"), find_tool("bannertool")
    if not makerom or not bannertool:
        print("build3ds: no makerom/bannertool, so no CIA (see port3ds/README.md: Building a CIA)")
        return
    png, wav, bnr = OUT / "banner.png", OUT / "banner.wav", OUT / "banner.bnr"
    plain_png(png, 256, 128)
    silent_wav(wav)
    run([bannertool, "makebanner", "-i", png, "-a", wav, "-o", bnr])
    cia = OUT / "rs64.cia"
    run([makerom, "-f", "cia", "-o", cia, "-elf", elf, "-rsf", ROOT / "port3ds/cia/rs64.rsf", "-icon", smdh, "-banner", bnr,
         "-exefslogo", "-target", "t"])
    shutil.copy2(cia, sd / "rs64.cia")


def compile_all(srcs, jobs, mod):
    objs, errors = [], []
    with cf.ThreadPoolExecutor(jobs) as ex:
        futs = {ex.submit(compile_one, s, k, mod): s for s, k in srcs}
        done = 0
        for f in cf.as_completed(futs):
            done += 1
            try:
                objs.append(f.result())
            except SystemExit as e:
                errors.append(str(e))
            if done % 20 == 0 or done == len(srcs):
                print(f"[{done}/{len(srcs)}] compiled ({'game.bin' if mod else 'rs64.3dsx'})", flush=True)
    if errors:
        raise SystemExit("\n".join(errors))
    return objs


def link_module(objs):
    """game.bin: an ARM shared object exporting rs3ds_module_entry; everything it needs from the port is imported by name."""
    so = OUT / "game.so"
    if stale(so, objs + [ROOT / "port3ds/module/module.ver"]):
        rsp = OUT / "objm.rsp"
        rsp.write_text("\n".join(f'"{o}"' for o in objs))
        run([GXX] + ARCH_GCC + ["-shared", "-nostdlib", "-fPIC", "-Wl,--hash-style=sysv", "-Wl,--gc-sections",
                                f"-Wl,--version-script={ROOT / 'port3ds/module/module.ver'}", f"-Wl,-Map,{OUT / 'game.map'}",
                                f"@{rsp}", "-o", so])
    nm = str(DKA / "bin/arm-none-eabi-nm")
    out = run([nm, "-D", "--undefined-only", so]).stdout
    names = sorted({l.split()[-1] for l in out.splitlines() if l.strip()})
    IMPORTS.write_text("".join(n + "\n" for n in names))
    binf = OUT / "game.bin"
    run([str(DKA / "bin/arm-none-eabi-strip"), "--strip-unneeded", so, "-o", binf])
    print(f"build3ds: game.bin {binf.stat().st_size // 1024} KB, imports {len(names)} names from the port")
    return binf


def gen_host_exports():
    """host_exports.S: name -> address for every function and variable game.bin imports (port3ds/module/imports.txt)."""
    names = [n for n in IMPORTS.read_text().split() if n]
    lines = ["    .section .rodata.rs3ds_host_exports,\"a\"", "    .balign 4", "    .global rs3ds_host_exports", "rs3ds_host_exports:"]
    for i, n in enumerate(names):
        lines.append(f"    .word .Lname{i}, {n}")
    lines.append("    .word 0, 0")
    lines.append("    .section .rodata.rs3ds_host_names,\"aMS\",%progbits,1")
    for i, n in enumerate(names):
        lines.append(f".Lname{i}: .asciz \"{n}\"")
    out = OUT / "gen/host_exports.S"
    text = "\n".join(lines) + "\n"
    if not out.exists() or out.read_text() != text:
        out.write_text(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--no-profile", action="store_true", help="without the core-0 sampling profiler")
    ap.add_argument("--port-only", action="store_true", help="only rs64.3dsx (no game.bin): needs nothing made from the ROM")
    a = ap.parse_args()
    if not SAFE:
        raise SystemExit("build3ds: --gpr32 is not supported with the game module")
    if a.clean and OUT.exists():
        shutil.rmtree(OUT)
    if not a.port_only:
        missing = [p for p in ["RecompiledFuncs/funcs.h", "RecompiledPatches/patches.c", "patches/patches.bin",
                               "build/factor5_ucode/musyx_audio_recompiled.c"] if not (ROOT / p).exists()]
        if missing:
            raise SystemExit("build3ds: generate these first (or use build_from_rom.py): " + ", ".join(missing))
    if not (ROOT / "src/game/state_table.inl").exists():
        raise SystemExit("build3ds: src/game/state_table.inl is missing (build_from_rom.py makes it)")
    gen()
    srcs = sources()
    exports_src = OUT / "gen/host_exports.S"
    objs = compile_all([x for x in srcs if x[0] != exports_src], a.jobs, False)
    game_bin = None
    if not a.port_only:
        game_bin = link_module(compile_all(module_sources(port_overrides(objs)), a.jobs, True))
    gen_host_exports()
    objs += compile_all([x for x in srcs if x[0] == exports_src], a.jobs, False)
    # project overrides (src/, port3ds/) before the runtime (lib/): with --allow-multiple-definition the first wins, as the desktop link
    elf = link(sorted(objs, key=lambda o: (o.name.startswith("lib__"), o.name)))
    sd = package(elf)
    if game_bin:
        shutil.copy2(game_bin, sd / "game.bin")
    print(f"build3ds: {sd / 'rs64.3dsx'} ({(sd / 'rs64.3dsx').stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
