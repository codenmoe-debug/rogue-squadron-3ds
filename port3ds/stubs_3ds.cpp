// Things the desktop build gets from code the 3DS port leaves out: the SDL/ImGui menus
// (menu_config.cpp, pause_menu.cpp), RT64's render context and video settings, the RT64 F5
// diagnostics, and librecomp's mod loader (code mods need a JIT). Each stand-in gives the
// "not overridden" answer, so the game's own code runs: its own main and pause menus, the
// original draw distance, no mods.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/mods.hpp"
#include "recompiler/live_recompiler.h"

extern "C" {
// ---- toml hook helpers (menus, HUD, draw distance)
int rs64_hook(uint32_t, uint8_t*, recomp_context*) { return 0; }
float rs64_throttle_hook(uint8_t*, float v, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return v; }
float rs64_shadow_keep_dist(void) { return 0.0f; }
float rs64_shadow_cull_margin(void) { return 1.0f; }   // divides the projected view-test size: 1 = no widening (0 culled every object)
extern volatile int g_rs3ds_widescreen;   // main_3ds.cpp
float rs64_cull_widen(void) { return g_rs3ds_widescreen ? 1.25f : 1.0f; }   // widescreen: (400/240) / (4/3)
float rs64_res_scale(void) { return 1.0f; }
float rs64_aspect_scale(void) { return 1.0f; }
int rs64_atst_head(float*, float*) { return 0; }
unsigned rs64_pause_array_for(uint8_t*, unsigned, unsigned native) { return native; }
unsigned rs64_pause_label_for(uint8_t*, unsigned, unsigned) { return 0; }
unsigned rs64_gof_override(uint8_t*, unsigned) { return 0; }
int rs64_controls_menu_active(void) { return 0; }
int rs64_menu_install_main(uint8_t*) { return -1; }
int rs64_menu_sfx_swap(recomp_context*) { return 0; }
void rs64_wheel_typed(uint8_t*, recomp_context*) {}
void rs64_slider_redirect(uint8_t*, unsigned) {}
void rs64_pause_slider_store(uint8_t*, recomp_context*) {}
void rs64_pause_slider_load(uint8_t*, recomp_context*) {}
void rs64_pause_slider_ends(uint8_t*, recomp_context*) {}
void rs64_pause_slider_bar(uint8_t*, recomp_context*) {}
void rs64_pause_install(uint8_t*, recomp_context*) {}
void rs64_pause_hover_sound(uint8_t*, recomp_context*) {}
void rs64_pause_confirm(uint8_t*, unsigned) {}
void rs64_pause_back_end(uint8_t*, unsigned) {}
void rs64_pause_back_begin(uint8_t*, unsigned) {}
void rs64_passcode_row(uint8_t*, recomp_context*) {}
void rs64_menu_text_language(int) {}
void rs64_menu_request_quit(void) {}
void rs64_menu_input_state(uint8_t*, recomp_context*) {}
void rs64_menu_hires_apply(uint8_t*) {}
void rs64_menu_confirm_sound(uint8_t*, recomp_context*) {}
void rs64_menu_confirm_main(uint8_t*) {}
void rs64_controls_load_models(uint8_t*, recomp_context*) {}
void rs64_controls_force_luke(uint8_t*) {}

// ---- RT64 F5 profile hooks and diagnostics (the 3DS renderer needs no per-object ids)
void rs64_f5_map_sprite(unsigned, unsigned) {}
void rs64_f5_map_node(unsigned, unsigned) {}
void rs64_f5_cable_submit(uint32_t, uint32_t, uint32_t) {}
void rs64_f5_set_texture_pool_move(uint32_t, uint32_t, uint32_t) {}
volatile long long g_rs64_pdl_us = 0;
volatile unsigned long long g_rs64_last_workload_id = 0;
volatile unsigned long long g_rs64_cpu_cleared_wid = 0;
volatile unsigned g_rs64_cpu_cleared_fb = 0;
volatile unsigned g_f5_task_hops = 0, g_f5_task_faces = 0;
}

// ---- video settings (src/video/video_config.cpp): the original game's look
namespace rs64::video {
float draw_distance() { return 1.0f; }
bool legacy_terrain() { return false; }
float hud_placement() { return 0.0f; }
bool keep_cutscene_draw_distance() { return false; }
std::string language() { return "en"; }
void set_language(const std::string&) {}
float effective_draw_distance(float setting, bool, uint32_t) { return setting; }
}

// ---- mods: none on the 3DS
namespace N64Recomp {
void live_recompiler_init() {}
ShimFunction::~ShimFunction() {}
}

namespace recomp::mods {
void register_hook_exports() {}
void register_data_exports() {}
void register_config_exports() {}
std::string error_to_string(ModOpenError) { return "mods are not supported on the 3DS"; }
std::string error_to_string(ModLoadError) { return "mods are not supported on the 3DS"; }

ModContext::ModContext() {}
ModContext::~ModContext() {}
void ModContext::register_game(const std::string&) {}
std::vector<ModOpenErrorDetails> ModContext::scan_mod_folder(const std::filesystem::path&) { return {}; }
void ModContext::load_mods_config() {}
bool ModContext::is_mod_enabled(const std::string&) { return false; }
std::vector<ModLoadErrorDetails> ModContext::load_mods(const GameEntry&, uint8_t*, int32_t, uint32_t& ram_used) { ram_used = 0; return {}; }
std::filesystem::path ModContext::get_mod_filename(const std::string&) const { return {}; }
std::vector<char> ModContext::read_mod_file(const std::string&, const std::string&, bool& exists) const { exists = false; return {}; }
std::optional<ModDetails> ModContext::get_details_for_mod(const std::string&) const { return std::nullopt; }
std::vector<ModDetails> ModContext::get_all_mod_details(const std::string&) { return {}; }
ConfigValueVariant ModContext::get_mod_config_value(size_t, const std::string&) const { return {}; }
ConfigValueVariant ModContext::get_mod_config_value(const std::string&, const std::string&) const { return {}; }
void ModContext::set_mods_config_path(const std::filesystem::path&) {}
void ModContext::set_mod_config_directory(const std::filesystem::path&) {}
}

// pieces of the mod loader the ModContext members still reference (never constructed on the 3DS)
namespace N64Recomp {
LiveGeneratorOutput::~LiveGeneratorOutput() {}
}
class recomp::mods::DynamicLibrary {
public:
    ~DynamicLibrary() {}
};

namespace recomp::mods {
ModHandle::~ModHandle() {}
void LiveRecompilerCodeHandle::set_imported_function(size_t, GenericFunction) {}
CodeModLoadError LiveRecompilerCodeHandle::populate_reference_symbols(const N64Recomp::Context&, std::string&) { return CodeModLoadError::Good; }
GenericFunction LiveRecompilerCodeHandle::get_function_handle(size_t) { return GenericFunction{}; }
}

// RT64 per-frame counters read by the frame profile in upstream_compat.cpp
#include <atomic>
extern "C" {
std::atomic<uint32_t> g_rs64_frame_tris{ 0 };
std::atomic<uint32_t> g_rs64_frame_draws{ 0 };
std::atomic<uint32_t> g_rs64_frame_texloads{ 0 };
}
