// The game module (game.bin): the code recompiled from the player's own ROM, built once on their computer
// (build_from_rom.py) and loaded by the port (rs64.3dsx, which contains no game code) at startup.
#pragma once

#include <cstdint>

#include "recomp.h"
#include "librecomp/rsp.hpp"

// Raised whenever the port and the module stop fitting together (recomp_context layout, the entry points below, the
// runtime functions the module imports): the port then asks for the module to be rebuilt.
#define RS3DS_MODULE_ABI 1

struct RS3DSModule {
    uint32_t abi;
    const char* build_id;
    void (*register_overlays)();
    const char* const* (*register_patches)();
    gpr (*entrypoint_address)();
    void (*recomp_entrypoint)(uint8_t* rdram, recomp_context* ctx);
    RspExitReason (*factor5_boot)(uint8_t* rdram, uint32_t ucode_addr);
    RspExitReason (*musyx_audio)(uint8_t* rdram, uint32_t ucode_addr);
};

// in the module
extern "C" const RS3DSModule* rs3ds_module_entry();

// in the port (module_loader.cpp)
const RS3DSModule* rs3ds_load_module(const char* path, char* err, size_t err_size);
