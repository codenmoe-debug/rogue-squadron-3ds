// The module's one export: what the port needs from the recompiled game.
#include "rs3ds_module.h"
#include "rs3ds_build_id.h"

void rs64_register_overlays();
const char* const* rs64_register_patches();
gpr get_entrypoint_address();
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
RspExitReason factor5_boot(uint8_t* rdram, uint32_t ucode_addr);
RspExitReason musyx_audio(uint8_t* rdram, uint32_t ucode_addr);

extern "C" __attribute__((visibility("default"))) const RS3DSModule* rs3ds_module_entry() {
    static const RS3DSModule m{ RS3DS_MODULE_ABI, RS3DS_BUILD_ID, rs64_register_overlays, rs64_register_patches,
                                get_entrypoint_address, recomp_entrypoint, factor5_boot, musyx_audio };
    return &m;
}
