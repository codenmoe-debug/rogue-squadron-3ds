// Rogue Squadron 64 Recompiled for the New Nintendo 3DS: the app (the role src/main/main.cpp plays on desktop).
//
// Folder: sdmc:/3ds/rs64/ holds rs64.3dsx and the user's own ROM (USA 1.0, .z64/.n64/.v64).
// The first boot checks it and stores a verified copy there (librecomp's select_rom); later
// boots use the stored copy. Logs go to rs64_log.txt in that folder and to the bottom screen.

#include <3ds.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"
#include "audio/audio.h"
#include "renderer_3ds.h"
#ifdef RS3DS_PROFILE
#include "rs3ds_prof.h"
#endif

// big stacks for the main thread (the game start thread is a std::thread: see sys_3ds.cpp)
u32 __stacksize__ = 1024 * 1024;
// linear heap: textures, the vertex rings, NDSP buffers; the rest (RDRAM, ROM) is the normal heap
u32 __ctru_linear_heap_size = 24u << 20;

static constexpr const char* kDir = "sdmc:/3ds/rs64";
static constexpr uint64_t RS64_ROM_HASH = 0x6B66A44153594DEAULL;
static constexpr const char8_t* kGameId = u8"rs64.n64.us.1.0";

void rs64_register_overlays();
const char* const* rs64_register_patches();
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
extern "C" void rs64_asset_overrides_init();
extern "C" void rs64_camera_shots_init();
extern "C" void rs64_mod_levels_init();
gpr get_entrypoint_address();
extern "C" { volatile uint8_t* volatile g_recomp_rdram_for_wp_raw = nullptr; }
extern volatile unsigned int g_vi_tick;

extern "C" void rs64_entrypoint_3ds(uint8_t* rdram, recomp_context* ctx) {
    g_recomp_rdram_for_wp_raw = rdram;
    rs64_asset_overrides_init();
    rs64_camera_shots_init();
    rs64_mod_levels_init();
    recomp_entrypoint(rdram, ctx);
}

// ------------------------------------------------------------------ input
// N64 buttons
enum : uint16_t {
    N_A = 0x8000, N_B = 0x4000, N_Z = 0x2000, N_START = 0x1000,
    N_DU = 0x0800, N_DD = 0x0400, N_DL = 0x0200, N_DR = 0x0100,
    N_L = 0x0020, N_R = 0x0010, N_CU = 0x0008, N_CD = 0x0004, N_CL = 0x0002, N_CR = 0x0001,
};

static std::atomic<uint32_t> g_held{ 0 };
static std::atomic<int32_t> g_stick{ 0 };   // x in the low 16 bits, y in the high
static std::atomic<int32_t> g_cstick{ 0 };

extern "C" { volatile int g_rs3ds_dump_request = 0; volatile int g_rs3ds_suspended = 0; volatile int g_rs3ds_widescreen = 0; }

// aspect.txt in the SD folder: "wide" = widescreen (wider field of view), anything else = the original 4:3
static void load_aspect() {
    char buf[16] = {};
    if (FILE* f = std::fopen("aspect.txt", "r")) {
        if (!std::fgets(buf, sizeof buf, f)) buf[0] = 0;
        std::fclose(f);
    }
    g_rs3ds_widescreen = std::strncmp(buf, "wide", 4) == 0 ? 1 : 0;
}

static void save_aspect() {
    if (FILE* f = std::fopen("aspect.txt", "w")) {
        std::fputs(g_rs3ds_widescreen ? "wide\n" : "4:3\n", f);
        std::fclose(f);
    }
}

// HOME menu and sleep: stop drawing (the renderer leaves the GPU alone) until the app is back
static aptHookCookie g_apt_cookie;
void rs3ds_audio_resync();   // sdl_shim_3ds.cpp

static void apt_hook(APT_HookType hook, void*) {
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
        g_rs3ds_suspended = 1;
    } else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        // the game kept queueing sound while away: drop that backlog so sound lines up with the picture again
        rs3ds_audio_resync();
        g_rs3ds_suspended = 0;
    }
}

static void poll_input() {}

static bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    *buttons = 0;
    *x = 0.f;
    *y = 0.f;
    if (controller_num != 0) return false;
    const uint32_t k = g_held.load();
    const int32_t st = g_stick.load(), cs = g_cstick.load();
    const int sx = (int16_t)(st & 0xFFFF), sy = (int16_t)(st >> 16);
    const int cx = (int16_t)(cs & 0xFFFF), cy = (int16_t)(cs >> 16);
    uint16_t b = 0;
    if (k & KEY_A) b |= N_A;              // fire lasers
    if (k & KEY_B) b |= N_B;              // secondary weapon
    if (k & KEY_R) b |= N_Z;              // Z
    if (k & KEY_ZR) b |= N_Z;
    if (k & KEY_L) b |= N_R;              // R
    if (k & KEY_ZL) b |= N_L;             // L
    if (k & KEY_START) b |= N_START;
    if (k & KEY_DUP) b |= N_DU;
    if (k & KEY_DDOWN) b |= N_DD;
    if (k & KEY_DLEFT) b |= N_DL;
    if (k & KEY_DRIGHT) b |= N_DR;
    if (k & KEY_X) b |= N_CU;             // C buttons on X / Y / SELECT and the C-stick
    if (k & KEY_Y) b |= N_CL;
    if (k & KEY_SELECT) b |= N_CD;
    if (cy > 40) b |= N_CU;
    if (cy < -40) b |= N_CD;
    if (cx < -40) b |= N_CL;
    if (cx > 40) b |= N_CR;
    *buttons = b;
    // Circle Pad: about +-155; N64 stick range is +-1 (scaled by the runtime to ~+-80)
    auto axis = [](int v) { float f = (float)v / 150.f; if (f > 1.f) f = 1.f; if (f < -1.f) f = -1.f; if (f > -0.08f && f < 0.08f) f = 0.f; return f; };
    *x = axis(sx);
    *y = axis(sy);
    return true;
}

static void set_rumble(int, bool) {}

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0) return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

// ------------------------------------------------------------------ gfx callbacks (the host loop)
static ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() { return nullptr; }
static ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) { return nullptr; }

// milliseconds from the 3DS system tick (osGetTime is the N64 clock here: see ultra64.h)
static uint64_t ms_now() { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000); }

static uint64_t g_last_scan = 0;
static void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    // recomp::start's host loop calls this every millisecond; scan the pad at ~250 Hz
    const uint64_t now = ms_now();
    if (now - g_last_scan < 4) return;
    g_last_scan = now;
    if (!aptMainLoop()) {
        // closing from the HOME menu: the game's threads never finish, so leave at once (a clean
        // shutdown hangs on "closing software")
        std::fprintf(stderr, "[3ds] exit requested\n");
        std::fflush(stderr);
        ndspExit();
        svcExitProcess();
    }
    hidScanInput();
    if (hidKeysDown() & KEY_TOUCH) {
        if ((hidKeysHeld() & (KEY_L | KEY_R)) == (KEY_L | KEY_R)) {
            g_rs3ds_dump_request = 1;
            std::fprintf(stderr, "Frame dump requested\n");
        } else {
            // a tap switches between the original 4:3 picture and widescreen
            g_rs3ds_widescreen = !g_rs3ds_widescreen;
            save_aspect();
        }
    }
    const uint32_t held = hidKeysHeld();
    g_held.store(held);
    circlePosition cp, cs;
    hidCircleRead(&cp);
    irrstCstickRead(&cs);
    g_stick.store((int32_t)(uint16_t)cp.dx | ((int32_t)cp.dy << 16));
    g_cstick.store((int32_t)(uint16_t)cs.dx | ((int32_t)cs.dy << 16));
    const uint32_t quit = KEY_L | KEY_R | KEY_START | KEY_SELECT;
    if ((held & quit) == quit) {
        std::fprintf(stderr, "[3ds] quit combo\n");
        std::fflush(stderr);
        ndspExit();
        svcExitProcess();
    }
}

void rs3ds_log_memory(const char* when);

static void vi_callback() { g_vi_tick = g_vi_tick + 1; }

static std::string get_game_thread_name(const OSThread* t) { return "[Game] " + std::to_string(t->id); }

// ------------------------------------------------------------------ ROM
static std::string find_rom() {
    DIR* d = opendir(kDir);
    if (!d) return {};
    std::string best;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() < 4) continue;
        std::string ext = n.substr(n.size() - 4);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".z64" && ext != ".n64" && ext != ".v64") continue;
        best = std::string(kDir) + "/" + n;
        break;
    }
    closedir(d);
    return best;
}

static void wait_exit(const char* msg) {
    std::printf("\n%s\n\nPress START to exit.\n", msg);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

int main() {
    osSetSpeedupEnable(true);   // New 3DS: 804 MHz + L2 cache
    // the host loop (input, quit) must preempt the game threads (0x2D): see set_native_thread_priority
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x28);
    // let the app use most of the system core (core 1): the renderer runs there (ultramodern events.cpp)
    const Result cpu_limit = APT_SetAppCpuTimeLimit(80);
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);
    irrstInit();
    aptHook(&g_apt_cookie, apt_hook, nullptr);
    std::printf("Rogue Squadron 64 Recompiled\nNew Nintendo 3DS port " RS3DS_VERSION "\n\n");

    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    if (!n3ds) std::printf("WARNING: this is not a New 3DS;\nthe game will be far too slow.\n\n");

    mkdir(kDir, 0777);
    chdir(kDir);
    load_aspect();
    rs3ds_check_for_update(kDir);
    if (FILE* f = std::freopen("sdmc:/3ds/rs64/rs64_log.txt", "w", stderr)) setvbuf(f, nullptr, _IOLBF, 0);

    // the 3DS renderer walks the display list straight from RDRAM before the task completes
    setenv("ROGUESQ_PARSE_SNAPSHOT", "0", 1);
    // the MusyX synth runs in the background (on core 2); its input pages are copied first (events.cpp), so speech keeps its data
    setenv("ROGUESQ_AUDIO_ASYNC", "1", 1);
    // periodic timing lines in rs64_log.txt (synth time per audio task)
    setenv("ROGUESQ_LOG_HEARTBEAT", "1", 1);
    // ~100 ms of real buffered audio rides through the synth's longer gaps on the 3DS
    setenv("ROGUESQ_AUDIO_LATENCY_MS", "100", 0);
    std::fprintf(stderr, "[3ds] boot: New 3DS=%d, core 1 time limit 80%%: %s\n", (int)n3ds, R_SUCCEEDED(cpu_limit) ? "ok" : "refused");
    rs3ds_log_memory("boot");

    rs64_register_overlays();
    rs64_register_patches();
    recomp::register_config_path(std::filesystem::path(kDir));
    recomp::GameEntry game{};
    game.rom_hash = RS64_ROM_HASH;
    game.internal_name = "ROGUE SQUADRON";
    game.game_id = kGameId;
    game.mod_game_id = "rs64";
    game.save_type = recomp::SaveType::Eep4k;
    game.is_enabled = true;
    game.entrypoint_address = get_entrypoint_address();
    game.entrypoint = rs64_entrypoint_3ds;
    recomp::register_game(game);

    std::u8string id = kGameId;
    std::printf("Checking the stored ROM...\n");
    recomp::check_all_stored_roms();
    if (!recomp::is_rom_valid(id)) {
        const std::string rom = find_rom();
        if (rom.empty()) {
            wait_exit("No ROM found.\nPut your own dump of Star Wars:\nRogue Squadron (USA 1.0), as .z64,\n.n64 or .v64, in sdmc:/3ds/rs64/");
            gfxExit();
            return 0;
        }
        std::printf("Importing %s\n(one time, about 20 s)...\n", rom.c_str());
        const recomp::RomValidationError r = recomp::select_rom(std::filesystem::path(rom), id);
        if (r != recomp::RomValidationError::Good) {
            wait_exit(r == recomp::RomValidationError::IncorrectVersion
                ? "That ROM is a different version.\nOnly USA 1.0 works (not Rev 1,\nEurope or Japan)."
                : "That file is not the Rogue Squadron\nUSA 1.0 ROM.");
            gfxExit();
            return 0;
        }
        recomp::check_all_stored_roms();
    }
    std::printf("ROM OK. Starting.\n");
    std::fprintf(stderr, "[3ds] ROM ok, recomp::start\n");
#ifdef RS3DS_PROFILE
    rs3ds_prof_start();
#endif
    rs3ds_log_memory("ROM checked");
    std::printf("Quit: L + R + START + SELECT\n\n");

    recomp::Configuration cfg{};
    cfg.project_version = { 0, 1, 0 };
    cfg.window_handle = nullptr;
    cfg.rsp_callbacks.get_rsp_microcode = rs64::audio::get_rsp_microcode;
    cfg.renderer_callbacks.create_render_context = rs3ds::create_render_context;
    cfg.audio_callbacks.queue_samples = rs64::audio::queue_samples;
    cfg.audio_callbacks.get_frames_remaining = rs64::audio::get_frames_remaining;
    cfg.audio_callbacks.set_frequency = rs64::audio::set_frequency;
    cfg.input_callbacks.poll_input = poll_input;
    cfg.input_callbacks.get_input = get_n64_input;
    cfg.input_callbacks.set_rumble = set_rumble;
    cfg.input_callbacks.get_connected_device_info = get_connected_device_info;
    cfg.gfx_callbacks.create_gfx = create_gfx;
    cfg.gfx_callbacks.create_window = create_window;
    cfg.gfx_callbacks.update_gfx = update_gfx;
    cfg.events_callbacks.vi_callback = vi_callback;
    cfg.events_callbacks.gfx_init_callback = []() {
        std::u8string gid = kGameId;
        std::fprintf(stderr, "[3ds] gfx ready, starting the game\n");
        rs3ds_log_memory("gfx ready");
        if (recomp::is_rom_valid(gid)) recomp::start_game(gid);
    };
    cfg.error_handling_callbacks.message_box = [](const char* msg) {
        std::fprintf(stderr, "[Error] %s\n", msg);
        std::printf("[Error] %s\n", msg);
    };
    cfg.threads_callbacks.get_game_thread_name = get_game_thread_name;
    recomp::start(cfg);
    std::fprintf(stderr, "[3ds] recomp::start returned\n");

    irrstExit();
    gfxExit();
    return 0;
}
