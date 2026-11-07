// Nintendo 3DS renderer for Rogue Squadron 64 Recompiled.
//
// Plays the role RT64 plays on the desktop: ultramodern hands it each graphics
// task (send_dl) and each VI swap (update_screen) on its gfx thread. It walks the
// game's Factor 5 display lists straight from RDRAM, as the RSP would (the opcode
// meanings come from docs/reference/factor5-gbi.md, f5-model-dl-spec.md and the
// RT64 profile lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp), transforms on the CPU,
// emulates TMEM for the texture loads, and draws through citro3d.
//
// The frame is drawn into a 1024x512 canvas texture (the N64 picture in its
// bottom-left corner) and presented to the top screen at 4:3, as the MvC2 3DS
// renderer does. Conventions shared with it: PICA textures are Morton-tiled and
// stored bottom row first; depth is 1 at the near plane (GEQUAL, cleared to 0).

#include <3ds.h>
#include <sys/iosupport.h>
#include <citro3d.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultra64.h"
#include "renderer_3ds.h"

extern "C" {
extern const unsigned char rs_shbin[];
extern const unsigned int rs_shbin_size;
}

// set by main_3ds.cpp: a touch on the bottom screen asks for a frame dump; HOME menu / sleep suspends drawing
extern "C" volatile int g_rs3ds_dump_request;
extern "C" volatile int g_rs3ds_suspended;
extern "C" volatile int g_rs3ds_widescreen;
extern "C" void rs3ds_gfx_release();   // ultramodern events.cpp: the game may go on with its next frame   // 1: a 25% wider view, presented across the full 400 px

// set by the HUD-pass hook (src/game/hook_helpers.cpp): the radar model's translation, to find its matrix in the display list
float g_rs3ds_radar_t[3] = { 0.f, 0.f, 0.f };
float g_rs3ds_radar_s[2] = { -1.f, -1.f };
volatile unsigned g_rs3ds_radar_seq = 0;

// stdout is the bottom-screen console until the renderer takes that screen; afterwards it goes to the log file
static ssize_t stdout_to_log(struct _reent*, void*, const char* ptr, size_t len) {
    std::fwrite(ptr, 1, len, stderr);
    return (ssize_t)len;
}
static const devoptab_t g_stdout_log = { "stdout_log", 0, nullptr, nullptr, stdout_to_log };

namespace {

// ------------------------------------------------------------------ constants
constexpr int CANVAS_TW = 1024, CANVAS_TH = 512;
constexpr size_t VRING_BYTES = 1536 * 1024;
constexpr int VCACHE = 64;
constexpr int MAX_DEPTH = 16;
constexpr uint32_t RAM_MASK = 0x00FFFFFFu;
constexpr uint32_t RAM_LIMIT = 0x01000000u;   // recomp::mem_size on the 3DS

// geometry mode (F3DEX bits; F5 only culls on 0x2000)
constexpr uint32_t G_ZBUFFER = 0x00000001, G_CULL_BACK = 0x00002000, G_FOG = 0x00010000;

struct Vtx {             // what the shader reads
    float x, y, z, w;    // GL-style clip coordinates on the canvas
    float u, v;
    uint8_t c[4];
    float u1, v1;        // second texture (N64 TEXEL1: tile + 1)
    float fu, fv;        // fog ramp coordinate
};

struct CacheVtx {        // RSP vertex cache entry
    float cx, cy, cz, cw;   // canvas clip
    float sx, sy;           // N64 screen pixels (valid when cw > 0)
    float fog;              // 0..1
    int16_t ox, oy, oz;     // object-space position (sprites)
    uint8_t c[4];
};

struct Tile {
    uint32_t fmt = 0, siz = 0, line = 0, tmem = 0, palette = 0;
    uint32_t cmt = 0, maskt = 0, shiftt = 0, cms = 0, masks = 0, shifts = 0;
    uint32_t uls = 0, ult = 0, lrs = 0, lrt = 0;
};

struct TexEntry {
    C3D_Tex* tex;
    int tw, th;          // texture size (texels)
    uint32_t last_frame;
};

struct DrawState {       // what a batch of triangles shares
    C3D_Tex* tex;
    C3D_Tex* tex1;       // TEXEL1 (nullptr: the combiner does not read it)
    uint32_t combine_hi, combine_lo, other_h, other_l, geom;
    uint32_t prim, env, fog, blend;
    uint8_t texp1;
    uint8_t texp;        // bit0 linear, bit1 clamp s, bit2 clamp t, bit3 mirror s, bit4 mirror t
    uint8_t mode;        // 0 normal, 1 fill, 2 copy, 3 depth clear
    uint8_t fog_on;
    uint8_t flat;        // every clip w is 1 (2D): stays at screen depth in stereo 3D
    uint8_t radar;       // drawn into the radar target (bottom screen), not the canvas
    uint8_t xs;          // widescreen: x squeezed by 0.8 in the vertex shader
    int16_t sc[4];
    bool operator==(const DrawState& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};

inline uint32_t pow2(uint32_t v) { uint32_t p = 8; while (p < v && p < 1024) p <<= 1; return p; }

static uint8_t g_morton[64];
static void init_morton() {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            g_morton[y * 8 + x] = (uint8_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}
inline uint32_t tile_index(uint32_t x, uint32_t y, uint32_t pw) {
    return (((y >> 3) * (pw >> 3) + (x >> 3)) << 6) | g_morton[((y & 7) << 3) | (x & 7)];
}

class Renderer3DS final : public ultramodern::renderer::RendererContext {
public:
    explicit Renderer3DS(uint8_t* rdram) : ram(rdram) {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        ok = init();
    }
    ~Renderer3DS() override {}

    bool valid() override { return ok; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return false; }
    void enable_instant_present() override {}
    void send_dl(const OSTask* task) override;
    void update_screen() override;
    void shutdown() override;
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }

private:
    // --------------------------------------------------------- RDRAM access
    uint8_t* ram;
    bool ok = false;
    inline bool in_ram(uint32_t a, uint32_t n) const { return a < RAM_LIMIT && a + n <= RAM_LIMIT; }
    // Reads go through a page table: a page the game may rewrite while this task is drawn points at a copy taken before
    // the game was let go (see send_dl); pages read are noted so the next task copies them.
    const uint8_t* rpage[RAM_LIMIT >> 12];
    mutable uint8_t touched[RAM_LIMIT >> 12];
    inline uint32_t rd32(uint32_t a) const {
        const uint32_t m = a & RAM_MASK & ~3u;
        touched[m >> 12] = 1;
        return *(const uint32_t*)(rpage[m >> 12] + (m & 0xFFF));
    }
    inline uint16_t rd16(uint32_t a) const {
        const uint32_t m = (a & RAM_MASK) ^ 2;
        touched[m >> 12] = 1;
        return *(const uint16_t*)(rpage[m >> 12] + (m & 0xFFF));
    }
    inline uint8_t rd8(uint32_t a) const {
        const uint32_t m = (a & RAM_MASK) ^ 3;
        touched[m >> 12] = 1;
        return rpage[m >> 12][m & 0xFFF];
    }

    // pipelining: copies of the pages the last two tasks read
    static constexpr int SHADOW_PAGES = 640;
    uint8_t* shadow_pool = nullptr;
    uint16_t shadow_list[SHADOW_PAGES];
    int shadow_n = 0;
    uint8_t page_age[RAM_LIMIT >> 12];   // tasks since the page was last read (255 = long ago)
    bool pipeline = true;
    uint64_t prof_snap = 0;
    uint32_t stat_snap_pages = 0;
    void snapshot_and_release();
    void end_snapshot();
    uint32_t seg_addr(uint32_t a) const { return (segments[(a >> 24) & 0xF] + (a & 0x00FFFFFFu)) & RAM_MASK; }

    // --------------------------------------------------------- GPU objects
    DVLB_s* dvlb = nullptr;
    shaderProgram_s prog;
    C3D_Tex canvas, fog_ramp, white;
    C3D_RenderTarget* canvas_rt = nullptr;
    C3D_RenderTarget* top_rt = nullptr;
    C3D_RenderTarget* top_rt_r = nullptr;
    C3D_RenderTarget* bot_rt = nullptr;

    // stereo 3D: the right eye is drawn into the canvas at x + RIGHT_EYE_X, every 3D vertex shifted by k * (w - STEREO_CONV)
    static constexpr int RIGHT_EYE_X = 512;
    static constexpr float STEREO_CONV = 400.f;
    int stereo_loc = -1;
    bool stereo_on = false, stereo_was_on = false;
    float stereo_k = 0.f;
    float unif_k = 0.f, unif_xs = 1.f;   // the stereo uniform as last set
    void set_stereo_unif(float k, float xs) {
        if (stereo_loc < 0 || (k == unif_k && xs == unif_xs)) return;
        unif_k = k;
        unif_xs = xs;
        C3D_FVUnifSet(GPU_VERTEX_SHADER, stereo_loc, k, -STEREO_CONV, xs, 0.f);
    }

    // radar on the bottom screen: its model matrix is found by the hook's translation, its vertices give the screen box
    bool radar_track = false;
    bool radar_pass = false;      // from the radar model's matrix to the end of the HUD pass: 3D draws go to the radar target
    bool on_radar_rt = false;
    static constexpr int RADAR_TS = 256;
    C3D_Tex radar_tex;
    C3D_RenderTarget* radar_rt = nullptr;
    bool radar_drawn = false;
    void route_target(bool radar);
    void apply_scissor(const DrawState& s);
    unsigned radar_seq_seen = 0, stat_radar_hook = 0, stat_radar_match = 0, stat_radar_fallback = 0;
    float last_mv_t[3] = { 0.f, 0.f, 0.f };   // the modelview translation closest to the radar's (diagnostics)
    float closest_mv_d = 1e30f;
    uint32_t radar_found_frame = 0;
    float rbx0 = 0.f, rby0 = 0.f, rbx1 = 0.f, rby1 = 0.f;
    bool crop_valid = false;
    uint32_t crop_frame = 0;
    float crop_cx = 0.f, crop_cy = 0.f, crop_h = 72.f;
    void radar_point(float x, float y);
    void radar_matrix_loaded();
    void update_radar_crop();
    void blit(C3D_RenderTarget* rt, int W, int H, float dx0, float dy0, float dx1, float dy1, float u0, float v0, float u1, float v1);
    uint8_t* vring[2] = { nullptr, nullptr };
    size_t vused = 0;
    uint32_t frame = 1;
    bool in_frame = false, drew = false;
    bool ring_full_logged = false;

    // batching
    DrawState cur_state{}, batch_state{};
    bool have_batch = false, gpu_state_valid = false;
    uint32_t batch_first = 0, batch_count = 0;
    std::vector<Vtx> scratch;
    std::vector<Vtx> scratch_radar;

    // texture cache
    std::unordered_map<uint64_t, TexEntry> tex_cache;
    std::vector<std::pair<C3D_Tex*, uint32_t>> dead_tex;
    size_t tex_bytes = 0;

    // --------------------------------------------------------- RSP state
    uint32_t segments[16] = {};
    float mv[16], proj[16], mvp[16];
    bool mvp_dirty = true;
    CacheVtx vcache[VCACHE];
    uint32_t vcache_n = 0;
    uint32_t color_buf = 0, color_cnt = 0;
    int32_t tc_scale[2] = { 0x10000, 0x10000 };
    double fog_m = 0.0, fog_o = 0.0;
    uint32_t geom = 0;
    struct { float sx, sy, tx, ty; } vp{ 160.f, 120.f, 160.f, 120.f };
    bool vp_from_stream = false;
    uint32_t tex_tile = 0;
    bool tex_on = true;

    // --------------------------------------------------------- RDP state
    uint32_t other_h = 0, other_l = 0, combine_hi = 0, combine_lo = 0;
    uint32_t prim_color = 0xFFFFFFFF, env_color = 0xFFFFFFFF, fog_color = 0, blend_color = 0, fill_color = 0;
    uint16_t prim_depth = 0;
    struct { uint32_t addr = 0, fmt = 0, siz = 0, width = 0; } timg, cimg;
    uint32_t zimg = 0;
    int scissor[4] = { 0, 0, 320 * 4, 240 * 4 };   // 10.2 ulx, uly, lrx, lry
    Tile tiles[8];
    uint8_t tmem[4096];
    uint16_t tlut[256];
    bool tex_dirty = true;
    C3D_Tex* cur_tex = nullptr;
    int cur_tw = 1, cur_th = 1;
    float inv_tw = 1.f, inv_th = 1.f, inv_tw1 = 1.f, inv_th1 = 1.f;   // 1 / the bound textures' sizes
    C3D_Tex* cur_tex1 = nullptr;
    int cur_tw1 = 1, cur_th1 = 1;
    bool use_tex1 = false;
    uint32_t tlut_gen = 0;

    // canvas size (N64 pixels)
    int canvas_w = 320, canvas_h = 240;

    // walk
    uint32_t chunk_base[MAX_DEPTH] = {};
    uint32_t ret_stack[MAX_DEPTH] = {};
    int depth = 0;
    uint32_t hops = 0, entries = 0, faces = 0;
    uint32_t chain[MAX_DEPTH][64];
    uint32_t chain_len[MAX_DEPTH] = {};

    // stats
    uint32_t stat_tris = 0, stat_tasks = 0, stat_draws = 0;
    uint32_t stat_frame0 = 0, stat_tasks0 = 0, stat_tris0 = 0, stat_draws0 = 0;
    uint64_t stat_t0 = 0, stat_ticks = 0;
    float xf_key[4] = { -1.f, -1.f, -1.f, -1.f };
    int xf_cw = 0, xf_ch = 0;
    float xf_ax = 0.f, xf_bx = 0.f, xf_ay = 0.f, xf_by = 0.f;
    uint64_t prof_load = 0, prof_resolve = 0, prof_flush = 0, prof_wait = 0, prof_vtx = 0, prof_terrain = 0, prof_emit = 0;
    int dump_tasks = 0;                 // tasks left to log in detail (frame dump)
    uint32_t dump_batches = 0, dump_dropped_idx = 0, dump_mtx_addr = 0;

    // --------------------------------------------------------- setup
    bool init();
    void frame_begin();
    void frame_end();
    void gpu_begin();

    // --------------------------------------------------------- matrices / vertices
    void load_matrix(uint32_t addr, float* out);
    void update_mvp();
    void transform(int16_t x, int16_t y, int16_t z, CacheVtx& o);
    void ensure_viewport();

    // --------------------------------------------------------- textures
    void retire_tex(C3D_Tex* t);
    void free_dead();
    void resolve_texture();
    void resolve_tile(uint32_t idx, C3D_Tex*& out, int& ow, int& oh);
    bool combiner_reads_texel1() const;
    uint8_t tile_texp(const Tile& t, bool linear) const;
    void decode_tile(const Tile& t, int tw, int th, int tile_w, int tile_h, uint32_t* out);
    uint32_t texel_rgba(const Tile& t, int s, int tt);
    void load_block(uint32_t w0, uint32_t w1);
    void load_tile(uint32_t w0, uint32_t w1);
    void load_tlut(uint32_t w0, uint32_t w1);
    // Texture loads are recorded, not copied: a load is identified by where it reads from (plus a
    // sample of the data), so a texture the cache already holds needs no TMEM copy or hashing.
    // The recorded loads are replayed into TMEM only when a texture has to be decoded.
    struct PendingLoad { uint8_t op; uint32_t w0, w1, taddr, tsiz, twidth, tmem, line, lo, hi; };
    std::vector<PendingLoad> pending;
    uint64_t tmem_sig[512] = {};   // per 64-bit TMEM word: the load that last wrote it
    uint64_t tlut_sig[16] = {};    // per 16-entry palette bank
    void record_load(uint8_t op, uint32_t w0, uint32_t w1);
    void apply_pending();
    uint64_t sample_rdram(uint32_t a, uint32_t n) const;

    // --------------------------------------------------------- drawing
    void build_state(DrawState& s, uint8_t mode);
    void apply_state(const DrawState& s);
    void setup_tev(const DrawState& s);
    struct TevKey { uint32_t hi, lo, cyc, prim, env, fog; uint8_t mode, fog_on, tex1; bool operator==(const TevKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; } };
    TevKey tev_key{};
    bool tev_valid = false;
    void flush();
    void emit_tris(const Vtx* v, int n, uint8_t mode = 0);
    void face(const uint32_t* idx, int n, const uint32_t* st, uint32_t color_word);
    void make_vtx(const CacheVtx& c, Vtx& o, float s_texel, float t_texel, bool textured);
    void rect(float ulx, float uly, float lrx, float lry, float s0, float t0, float dsdx, float dtdy, bool textured, bool flip, uint8_t mode, uint32_t color);
    void fill_rect(uint32_t w0, uint32_t w1);
    void tex_rect(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, bool flip);
    void sprite(uint32_t pc);
    void terrain(uint32_t pc);
    void tile_quad(uint32_t pc);
    void tile_grid(uint32_t pc);

    // --------------------------------------------------------- walk
    void run_task(uint32_t data_ptr);
    bool next_chunk(uint32_t& pc);
    void enter_chunk(uint32_t& pc, uint32_t target);
};

// ===================================================================== setup
bool Renderer3DS::init() {
    init_morton();
    for (uint32_t p = 0; p < (RAM_LIMIT >> 12); p++) rpage[p] = ram + (size_t)p * 4096;
    std::memset(touched, 0, sizeof touched);
    std::memset(page_age, 255, sizeof page_age);
    shadow_pool = (uint8_t*)std::malloc((size_t)SHADOW_PAGES * 4096);
    if (FILE* f = std::fopen("gfx_sync.txt", "r")) {
        std::fclose(f);
        pipeline = false;
    }
    if (!shadow_pool) pipeline = false;
    std::fprintf(stderr, "[r3ds] game/renderer overlap %s\n", pipeline ? "on" : "off (gfx_sync.txt or no memory)");
    std::memset(tmem, 0, sizeof tmem);
    std::memset(tlut, 0, sizeof tlut);
    for (int i = 0; i < 16; i++) { mv[i] = proj[i] = mvp[i] = (i % 5) == 0 ? 1.f : 0.f; }

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 8)) return false;

    dvlb = DVLB_ParseFile((u32*)rs_shbin, rs_shbin_size);
    if (!dvlb) return false;
    shaderProgramInit(&prog);
    shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);

    if (!C3D_TexInitVRAM(&canvas, CANVAS_TW, CANVAS_TH, GPU_RGBA8)) return false;
    C3D_DEPTHTYPE depth24(GPU_RB_DEPTH24_STENCIL8);
    canvas_rt = C3D_RenderTargetCreateFromTex(&canvas, GPU_TEXFACE_2D, 0, depth24);
    if (!canvas_rt) return false;
    C3D_DEPTHTYPE nodepth(-1);
    top_rt = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, nodepth);
    if (!top_rt) return false;
    C3D_RenderTargetSetOutput(top_rt, GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    top_rt_r = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, nodepth);
    if (!top_rt_r) return false;
    C3D_RenderTargetSetOutput(top_rt_r, GFX_TOP, GFX_RIGHT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    // the bottom screen leaves the text console and shows the radar
    std::fflush(stdout);
    devoptab_list[STD_OUT] = &g_stdout_log;
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    bot_rt = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, nodepth);
    if (!bot_rt) return false;
    C3D_RenderTargetSetOutput(bot_rt, GFX_BOTTOM, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    stereo_loc = shaderInstanceGetUniformLocation(prog.vertexShader, "stereo");
    if (!C3D_TexInitVRAM(&radar_tex, RADAR_TS, RADAR_TS, GPU_RGBA8) && !C3D_TexInit(&radar_tex, RADAR_TS, RADAR_TS, GPU_RGBA8)) return false;
    radar_rt = C3D_RenderTargetCreateFromTex(&radar_tex, GPU_TEXFACE_2D, 0, depth24);
    if (!radar_rt) return false;

    // fog ramp: alpha = u
    if (!C3D_TexInit(&fog_ramp, 256, 8, GPU_RGBA8)) return false;
    {
        uint32_t* d = (uint32_t*)fog_ramp.data;
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 256; x++) d[tile_index(x, y, 256)] = 0xFFFFFF00u | x;
        C3D_TexFlush(&fog_ramp);
        C3D_TexSetFilter(&fog_ramp, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&fog_ramp, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    if (!C3D_TexInit(&white, 8, 8, GPU_RGBA8)) return false;
    std::memset(white.data, 0xFF, white.size);
    C3D_TexFlush(&white);

    vring[0] = (uint8_t*)linearAlloc(VRING_BYTES);
    vring[1] = (uint8_t*)linearAlloc(VRING_BYTES);
    if (!vring[0] || !vring[1]) return false;
    scratch.reserve(64);
    cur_tex = &white;
    std::printf("[r3ds] renderer ready\n");
    std::fprintf(stderr, "[r3ds] renderer ready\n");
    return true;
}

void Renderer3DS::gpu_begin() {
    C3D_BindProgram(&prog);
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 2);
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, vring[frame & 1], sizeof(Vtx), 5, 0x43210);
    C3D_DepthMap(true, -1.0f, 0.0f);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_TexBind(1, &white);
    C3D_TexBind(2, &fog_ramp);
    if (stereo_loc >= 0) C3D_FVUnifSet(GPU_VERTEX_SHADER, stereo_loc, 0.f, -STEREO_CONV, 1.f, 0.f);
    unif_k = 0.f;
    unif_xs = 1.f;
    gpu_state_valid = false;
    tev_valid = false;
}

void Renderer3DS::frame_begin() {
    if (in_frame) return;
    free_dead();
    { const uint64_t t0 = svcGetSystemTick(); C3D_FrameBegin(0); prof_wait += svcGetSystemTick() - t0; }
    in_frame = true;
    vused = 0;
    const float slider = osGet3DSliderState();
    stereo_on = slider > 0.01f && canvas_w <= RIGHT_EYE_X && stereo_loc >= 0;
    stereo_k = slider * 0.03f;
    if (stereo_on != stereo_was_on) {
        gfxSet3D(stereo_on);
        stereo_was_on = stereo_on;
    }
    C3D_RenderTargetClear(radar_rt, C3D_CLEAR_ALL, 0x00000000, 0);
    radar_drawn = false;
    on_radar_rt = false;
    C3D_FrameDrawOn(canvas_rt);
    C3D_SetViewport(0, 0, (u32)canvas_w, (u32)canvas_h);
    gpu_begin();
}

// draws the canvas region (u0,v0)-(u1,v1) onto dest pixels (dx0,dy0)-(dx1,dy1) of a W x H screen target (y down)
void Renderer3DS::blit(C3D_RenderTarget* rt, int W, int H, float dx0, float dy0, float dx1, float dy1, float u0, float v0, float u1, float v1) {
    C3D_FrameDrawOn(rt);
    C3D_RenderTargetClear(rt, C3D_CLEAR_COLOR, 0x000000FF, 0);
    if (dx1 <= dx0) return;
    const float xs[4] = { dx0, dx1, dx1, dx0 }, ys[4] = { dy0, dy0, dy1, dy1 };
    const float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
    static const int ord[6] = { 0, 1, 2, 0, 2, 3 };
    Vtx q[6];
    for (int k = 0; k < 6; k++) {
        const int i = ord[k];
        const float nx = xs[i] / (W * 0.5f) - 1.f, ny = 1.f - ys[i] / (H * 0.5f);
        // Mtx_OrthoTilt: clip x = ndc y, clip y = -ndc x
        q[k].x = ny; q[k].y = -nx; q[k].z = 0.f; q[k].w = 1.f;
        q[k].u = us[i]; q[k].v = vs[i];
        q[k].c[0] = q[k].c[1] = q[k].c[2] = q[k].c[3] = 255;
        q[k].u1 = q[k].v1 = 0.f;
        q[k].fu = 0.f; q[k].fv = 0.5f;
    }
    if (vused + sizeof q > VRING_BYTES) return;
    const size_t at = (vused + sizeof(Vtx) - 1) / sizeof(Vtx);
    std::memcpy(vring[frame & 1] + at * sizeof(Vtx), q, sizeof q);
    vused = (at + 6) * sizeof(Vtx);
    C3D_DrawArrays(GPU_TRIANGLES, (int)at, 6);
}

void Renderer3DS::frame_end() {
    flush();
    // present: the canvas (bottom-left canvas_w x canvas_h of the texture) to the top screen at 4:3
    gpu_begin();
    C3D_SetViewport(0, 0, 240, 400);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_TexSetFilter(&canvas, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&canvas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, &canvas);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));

    const float cw = g_rs3ds_widescreen ? 400.f : 240.f * 4.f / 3.f, x0 = (400.f - cw) / 2.f;
    const float u1 = (float)canvas_w / CANVAS_TW, vt = (float)canvas_h / CANVAS_TH;
    blit(top_rt, 400, 240, x0, 0.f, x0 + cw, 240.f, 0.f, vt, u1, 0.f);
    if (stereo_on) {
        const float ur = (float)RIGHT_EYE_X / CANVAS_TW;
        blit(top_rt_r, 400, 240, x0, 0.f, x0 + cw, 240.f, ur, vt, ur + u1, 0.f);
    }

    // bottom screen: the radar's own picture, square in the middle (black outside missions)
    update_radar_crop();
    if (radar_drawn && crop_valid && frame - crop_frame < 30) {
        C3D_TexSetFilter(&radar_tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&radar_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        C3D_TexBind(0, &radar_tex);
        blit(bot_rt, 320, 240, 40.f, 0.f, 280.f, 240.f, 0.f, 1.f, 1.f, 0.f);
    } else {
        blit(bot_rt, 320, 240, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
    }
    C3D_FrameEnd(0);
    in_frame = false;
    drew = false;
    have_batch = false;
    frame++;
}

void Renderer3DS::radar_point(float x, float y) {
    if (radar_found_frame != frame) {
        radar_found_frame = frame;
        rbx0 = rbx1 = x;
        rby0 = rby1 = y;
        return;
    }
    rbx0 = std::min(rbx0, x); rbx1 = std::max(rbx1, x);
    rby0 = std::min(rby0, y); rby1 = std::max(rby1, y);
}

// a modelview load: is it the radar model's? (the HUD pass camera is the identity, so its translation is the node's)
void Renderer3DS::radar_matrix_loaded() {
    radar_track = false;
    if (g_rs3ds_radar_seq == 0) return;
    const float d = std::fabs(mv[12] - g_rs3ds_radar_t[0]) + std::fabs(mv[13] - g_rs3ds_radar_t[1]) + std::fabs(mv[14] - g_rs3ds_radar_t[2]);
    if (d < closest_mv_d) {
        closest_mv_d = d;
        last_mv_t[0] = mv[12]; last_mv_t[1] = mv[13]; last_mv_t[2] = mv[14];
    }
    // the display list's matrix carries the node's translation times a fixed scale (about 256)
    const float tz = g_rs3ds_radar_t[2];
    if (tz <= 1e-4f) return;
    const float k = mv[14] / tz;
    if (k < 1.f) return;
    for (int i = 0; i < 2; i++) {
        const float want = g_rs3ds_radar_t[i] * k;
        if (std::fabs(mv[12 + i] - want) > 2.f + std::fabs(want) * 0.03f) return;
    }
    update_mvp();
    ensure_viewport();
    const float w = mvp[15];
    if (w <= 1e-5f) return;
    radar_track = true;
    radar_pass = true;
    stat_radar_match++;
    radar_point(vp.tx + mvp[12] / w * vp.sx, vp.ty + mvp[13] / w * vp.sy);
}

void Renderer3DS::update_radar_crop() {
    const unsigned seq = g_rs3ds_radar_seq;
    const bool hook_ran = seq != radar_seq_seen;
    radar_seq_seen = seq;
    if (hook_ran) stat_radar_hook++;
    if (radar_found_frame != frame && hook_ran && g_rs3ds_radar_s[0] >= 0.f) {
        // no model matrix matched: the hook's own projection of the radar position, default size
        stat_radar_fallback++;
        radar_point(g_rs3ds_radar_s[0], g_rs3ds_radar_s[1]);
    }
    if (radar_found_frame != frame) return;
    const float cx = (rbx0 + rbx1) * 0.5f, cy = (rby0 + rby1) * 0.5f;
    float h = std::max(rby1 - rby0, rbx1 - rbx0) * 1.45f;
    if (h < (float)canvas_h * 0.1f) h = (float)canvas_h * 0.3f;
    h = std::min(h, (float)canvas_h);
    if (!crop_valid) {
        crop_cx = cx; crop_cy = cy; crop_h = h;
        crop_valid = true;
    } else {
        crop_cx += (cx - crop_cx) * 0.3f;
        crop_cy += (cy - crop_cy) * 0.3f;
        crop_h += (h - crop_h) * 0.3f;
    }
    const float hw = crop_h * 0.5f, hh = crop_h * 0.5f;
    crop_cx = std::clamp(crop_cx, hw, std::max(hw, (float)canvas_w - hw));
    crop_cy = std::clamp(crop_cy, hh, std::max(hh, (float)canvas_h - hh));
    crop_frame = frame;
}

void Renderer3DS::update_screen() {
    if (g_rs3ds_suspended) return;
    if (in_frame && drew) frame_end();
}

void Renderer3DS::shutdown() {
    if (in_frame) { C3D_FrameEnd(0); in_frame = false; }
    for (auto& kv : tex_cache) { C3D_TexDelete(kv.second.tex); delete kv.second.tex; }
    tex_cache.clear();
    C3D_Fini();
}

// ===================================================================== matrices
void Renderer3DS::load_matrix(uint32_t addr, float* out) {
    for (int i = 0; i < 16; i++) {
        const int16_t ip = (int16_t)rd16(addr + 2 * i);
        const uint16_t fp = rd16(addr + 32 + 2 * i);
        out[i] = (float)ip + (float)fp / 65536.0f;
    }
}

void Renderer3DS::update_mvp() {
    if (!mvp_dirty) return;
    // row vectors: v' = v * MV * P
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0.f;
            for (int k = 0; k < 4; k++) s += mv[r * 4 + k] * proj[k * 4 + c];
            mvp[r * 4 + c] = s;
        }
    mvp_dirty = false;
}

void Renderer3DS::ensure_viewport() {
    if (vp_from_stream) return;
    int w = (scissor[2] - scissor[0]) / 4, h = (scissor[3] - scissor[1]) / 4;
    if (w <= 0 || w > 1024 || h <= 0 || h > 1024) { w = 320; h = 240; }
    vp.sx = w * 0.5f; vp.sy = h * 0.5f;
    vp.tx = w * 0.5f + scissor[0] / 4.f; vp.ty = h * 0.5f + scissor[1] / 4.f;
}

void Renderer3DS::transform(int16_t x, int16_t y, int16_t z, CacheVtx& o) {
    update_mvp();
    // canvas factors of the viewport, recomputed only when the viewport or canvas changes
    if (vp.sx != xf_key[0] || vp.sy != xf_key[1] || vp.tx != xf_key[2] || vp.ty != xf_key[3] || canvas_w != xf_cw || canvas_h != xf_ch) {
        xf_key[0] = vp.sx; xf_key[1] = vp.sy; xf_key[2] = vp.tx; xf_key[3] = vp.ty;
        xf_cw = canvas_w; xf_ch = canvas_h;
        const float ihw = 2.f / (float)canvas_w, ihh = 2.f / (float)canvas_h;
        xf_ax = vp.tx * ihw - 1.f; xf_bx = vp.sx * ihw;
        xf_ay = 1.f - vp.ty * ihh; xf_by = vp.sy * ihh;
    }
    const float fx = x, fy = y, fz = z;
    const float cx = fx * mvp[0] + fy * mvp[4] + fz * mvp[8] + mvp[12];
    const float cy = fx * mvp[1] + fy * mvp[5] + fz * mvp[9] + mvp[13];
    const float cz = fx * mvp[2] + fy * mvp[6] + fz * mvp[10] + mvp[14];
    const float cw = fx * mvp[3] + fy * mvp[7] + fz * mvp[11] + mvp[15];
    // N64 screen: s = vtrans + ndc * vscale (F5's ndc is y-down); canvas clip is GL (y up)
    o.cx = xf_ax * cw + cx * xf_bx;
    o.cy = xf_ay * cw - cy * xf_by;
    o.cz = cz;
    o.cw = cw;
    if (cw > 1e-5f) {
        const float iw = 1.f / cw;
        o.sx = vp.tx + cx * iw * vp.sx;
        o.sy = vp.ty + cy * iw * vp.sy;
        if (radar_track) radar_point(o.sx, o.sy);
        const float f = (float)((double)(cz * iw) * fog_m + fog_o);
        o.fog = f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
    } else {
        o.sx = o.sy = 0.f;
        o.fog = 1.f;
    }
    o.ox = x; o.oy = y; o.oz = z;
}

// ===================================================================== textures
void Renderer3DS::retire_tex(C3D_Tex* t) { dead_tex.emplace_back(t, frame); }

void Renderer3DS::free_dead() {
    size_t k = 0;
    for (size_t i = 0; i < dead_tex.size(); i++) {
        if (dead_tex[i].second + 2 <= frame) { C3D_TexDelete(dead_tex[i].first); delete dead_tex[i].first; }
        else dead_tex[k++] = dead_tex[i];
    }
    dead_tex.resize(k);
    // keep the cache under ~12 MB of linear memory: drop entries unused for a while
    if (tex_bytes > 12u * 1024u * 1024u) {
        for (auto it = tex_cache.begin(); it != tex_cache.end();) {
            if (it->second.last_frame + 30 < frame) {
                tex_bytes -= it->second.tex->size;
                retire_tex(it->second.tex);
                it = tex_cache.erase(it);
            } else ++it;
        }
    }
}

static inline uint32_t rgba5551(uint16_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
    return ((r << 3 | r >> 2) << 24) | ((g << 3 | g >> 2) << 16) | ((b << 3 | b >> 2) << 8) | ((c & 1) ? 0xFF : 0);
}
static inline uint32_t ia16(uint16_t c) { uint32_t i = c >> 8, a = c & 0xFF; return (i << 24) | (i << 16) | (i << 8) | a; }

uint32_t Renderer3DS::texel_rgba(const Tile& t, int s, int tt) {
    const uint32_t base = t.tmem * 8, line = t.line * 8;
    const uint32_t row = base + (uint32_t)tt * line;
    const uint32_t odd = (tt & 1) ? 4 : 0;
    auto byte_at = [&](uint32_t a) -> uint8_t { return tmem[(a ^ odd) & 0xFFF]; };
    switch (t.siz) {
    case 0: {   // 4-bit
        const uint8_t b = byte_at(row + (uint32_t)s / 2);
        const uint32_t n = (s & 1) ? (b & 0xF) : (b >> 4);
        if (t.fmt == 2) {   // CI4
            const uint16_t c = tlut[(t.palette * 16 + n) & 0xFF];
            return ((other_h >> 14) & 3) == 3 ? ia16(c) : rgba5551(c);
        }
        if (t.fmt == 3) {   // IA4: 3-bit intensity, 1-bit alpha
            uint32_t i = (n >> 1) & 7; i = (i << 5) | (i << 2) | (i >> 1);
            return (i << 24) | (i << 16) | (i << 8) | ((n & 1) ? 0xFF : 0);
        }
        const uint32_t i = n * 17;   // I4
        return (i << 24) | (i << 16) | (i << 8) | i;
    }
    case 1: {   // 8-bit
        const uint8_t b = byte_at(row + (uint32_t)s);
        if (t.fmt == 2) {
            const uint16_t c = tlut[b];
            return ((other_h >> 14) & 3) == 3 ? ia16(c) : rgba5551(c);
        }
        if (t.fmt == 3) {   // IA8
            const uint32_t i = (b >> 4) * 17, a = (b & 0xF) * 17;
            return (i << 24) | (i << 16) | (i << 8) | a;
        }
        return ((uint32_t)b << 24) | ((uint32_t)b << 16) | ((uint32_t)b << 8) | b;   // I8
    }
    case 2: {   // 16-bit
        const uint32_t a = row + (uint32_t)s * 2;
        const uint16_t c = (uint16_t)(byte_at(a) << 8 | byte_at(a + 1));
        if (t.fmt == 3) return ia16(c);
        if (t.fmt == 2) { const uint16_t p = tlut[c & 0xFF]; return rgba5551(p); }
        return rgba5551(c);
    }
    default: {  // 32-bit RGBA: RG in the low half of TMEM, BA in the high half
        const uint32_t a = row + (uint32_t)s * 2;
        const uint8_t r = tmem[(a ^ odd) & 0x7FF], g = tmem[((a ^ odd) + 1) & 0x7FF];
        const uint8_t b = tmem[(((a ^ odd) & 0x7FF) + 0x800) & 0xFFF], al = tmem[((((a ^ odd) + 1) & 0x7FF) + 0x800) & 0xFFF];
        return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | al;
    }
    }
}

void Renderer3DS::decode_tile(const Tile& t, int tw, int th, int tile_w, int tile_h, uint32_t* out) {
    const int mw = t.masks ? (1 << t.masks) : 0, mh = t.maskt ? (1 << t.maskt) : 0;
    for (int y = 0; y < th; y++) {
        int sy = y;
        if (mh) { const bool mir = (t.cmt & 1) && ((sy / mh) & 1); sy &= mh - 1; if (mir) sy = mh - 1 - sy; }
        if (sy >= tile_h) sy = tile_h - 1;
        for (int x = 0; x < tw; x++) {
            int sx = x;
            if (mw) { const bool mir = (t.cms & 1) && ((sx / mw) & 1); sx &= mw - 1; if (mir) sx = mw - 1 - sx; }
            if (sx >= tile_w) sx = tile_w - 1;
            out[y * tw + x] = texel_rgba(t, sx, sy);
        }
    }
}

static inline uint64_t hash_bytes(const uint8_t* p, size_t n, uint64_t h) {
    // FNV-1a over 32-bit words (texture keys; collisions only cost a wrong texture)
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w;
        std::memcpy(&w, p + i, 4);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}

bool Renderer3DS::combiner_reads_texel1() const {
    const uint32_t hi = combine_hi, lo = combine_lo;
    const uint32_t cyc = (other_h >> 20) & 3;
    if (cyc != 1) return false;   // TEXEL1 exists in 2-cycle mode only (1-cycle hardware reads TEXEL0)
    for (int cy = (cyc == 1 ? 0 : 1); cy < 2; cy++) {
        const uint32_t ca = cy ? (hi >> 5) & 0xF : (hi >> 20) & 0xF, cc = cy ? hi & 0x1F : (hi >> 15) & 0x1F;
        const uint32_t cb = cy ? (lo >> 24) & 0xF : (lo >> 28) & 0xF, cd = cy ? (lo >> 6) & 7 : (lo >> 15) & 7;
        const uint32_t aa = cy ? (lo >> 21) & 7 : (hi >> 12) & 7, ac = cy ? (lo >> 18) & 7 : (hi >> 9) & 7;
        const uint32_t ab = cy ? (lo >> 3) & 7 : (lo >> 12) & 7, ad = cy ? lo & 7 : (lo >> 9) & 7;
        if (ca == 2 || cb == 2 || cc == 2 || cc == 9 || cd == 2 || aa == 2 || ab == 2 || ac == 2 || ad == 2) return true;
    }
    return false;
}

void Renderer3DS::resolve_texture() {
    if (!tex_dirty) return;
    tex_dirty = false;
    struct Prof { uint64_t& acc; uint64_t t0; ~Prof() { acc += svcGetSystemTick() - t0; } } prof{ prof_resolve, svcGetSystemTick() };
    resolve_tile(tex_tile & 7, cur_tex, cur_tw, cur_th);
    inv_tw = 1.f / (float)cur_tw;
    inv_th = 1.f / (float)cur_th;
    use_tex1 = combiner_reads_texel1();
    if (use_tex1) {
        resolve_tile((tex_tile + 1) & 7, cur_tex1, cur_tw1, cur_th1);
        inv_tw1 = 1.f / (float)cur_tw1;
        inv_th1 = 1.f / (float)cur_th1;
    }
}

void Renderer3DS::resolve_tile(uint32_t idx, C3D_Tex*& out_tex, int& out_w, int& out_h) {
    const Tile& t = tiles[idx & 7];
    int tile_w = (int)((t.lrs >> 2) - (t.uls >> 2)) + 1, tile_h = (int)((t.lrt >> 2) - (t.ult >> 2)) + 1;
    if (tile_w <= 0 || tile_w > 1024) tile_w = 32;
    if (tile_h <= 0 || tile_h > 1024) tile_h = 32;
    int tw = (int)pow2((uint32_t)(t.masks ? std::max(1 << t.masks, 1) : tile_w));
    int th = (int)pow2((uint32_t)(t.maskt ? std::max(1 << t.maskt, 1) : tile_h));
    if (t.masks && (1 << t.masks) < 8) tw = 8;
    if (t.maskt && (1 << t.maskt) < 8) th = 8;
    tw = std::min(tw, 256);
    th = std::min(th, 256);

    // key: tile shape + the TMEM bytes it reads + its palette
    const uint32_t line = std::max<uint32_t>(t.line, 1) * 8;
    const uint32_t rows = (uint32_t)std::min(th, tile_h > th ? th : std::max(tile_h, 1));
    const uint32_t lo = (t.tmem * 8) & 0xFFF;
    uint32_t n = std::min<uint32_t>(rows * line, 4096u - lo);
    if (t.siz == 3) n = std::min<uint32_t>(rows * line, 2048u - (lo & 0x7FF));
    uint64_t h = 0xCBF29CE484222325ull;
    h = (h ^ ((uint64_t)t.fmt | (uint64_t)t.siz << 3 | (uint64_t)t.line << 5 | (uint64_t)t.tmem << 14 | (uint64_t)t.palette << 23)) * 0x100000001B3ull;
    h = (h ^ ((uint64_t)t.cms | (uint64_t)t.cmt << 2 | (uint64_t)t.masks << 4 | (uint64_t)t.maskt << 8 | (uint64_t)tile_w << 12 | (uint64_t)tile_h << 22)) * 0x100000001B3ull;
    {   // the loads that wrote the TMEM words this tile reads (first, middle, last)
        const uint32_t w_first = lo >> 3, w_last = ((lo + (n ? n - 1 : 0)) >> 3) & 511, w_mid = ((w_first + w_last) / 2) & 511;
        h = (h ^ tmem_sig[w_first]) * 0x100000001B3ull;
        h = (h ^ tmem_sig[w_mid]) * 0x100000001B3ull;
        h = (h ^ tmem_sig[w_last]) * 0x100000001B3ull;
        if (t.siz == 3) h = (h ^ tmem_sig[256 + (w_first & 255)]) * 0x100000001B3ull;
    }
    if (t.fmt == 2) {
        h = (h ^ ((other_h >> 14) & 3)) * 0x100000001B3ull;
        if (t.siz == 0) h = (h ^ tlut_sig[t.palette & 15]) * 0x100000001B3ull;
        else for (int b = 0; b < 16; b++) h = (h ^ tlut_sig[b]) * 0x100000001B3ull;
    }

    auto it = tex_cache.find(h);
    if (it != tex_cache.end()) {
        it->second.last_frame = frame;
        out_tex = it->second.tex;
        out_w = it->second.tw;
        out_h = it->second.th;
        return;
    }
    apply_pending();   // a texture to decode: bring TMEM up to date first
    C3D_Tex* tex = new C3D_Tex;
    if (!C3D_TexInit(tex, (u16)tw, (u16)th, GPU_RGBA8)) {
        delete tex;
        out_tex = &white; out_w = out_h = 8;
        return;
    }
    static std::vector<uint32_t> buf;
    buf.resize((size_t)tw * th);
    decode_tile(t, tw, th, tile_w, tile_h, buf.data());
    uint32_t* d = (uint32_t*)tex->data;
    for (int y = 0; y < th; y++) {
        const uint32_t my = (uint32_t)(th - 1 - y);   // stored bottom row first
        for (int x = 0; x < tw; x++) d[tile_index((uint32_t)x, my, (uint32_t)tw)] = buf[(size_t)y * tw + x];
    }
    C3D_TexFlush(tex);
    tex_cache.emplace(h, TexEntry{ tex, tw, th, frame });
    tex_bytes += tex->size;
    out_tex = tex;
    out_w = tw;
    out_h = th;
}

uint64_t Renderer3DS::sample_rdram(uint32_t a, uint32_t n) const {
    uint64_t h = 0x84222325CBF29CE4ull ^ a ^ ((uint64_t)n << 32);
    if (n < 4) return h;
    const uint32_t step = std::max<uint32_t>(4, (n / 16) & ~3u);
    for (uint32_t o = 0; o + 4 <= n; o += step) {
        if (a + o + 4 > RAM_LIMIT) break;
        h = (h ^ rd32(a + o)) * 0x100000001B3ull;
    }
    return (h ^ rd32(a + n - 4)) * 0x100000001B3ull;
}

void Renderer3DS::record_load(uint8_t op, uint32_t w0, uint32_t w1) {
    const uint32_t tile = (w1 >> 24) & 7;
    Tile& t = tiles[tile];
    PendingLoad L{ op, w0, w1, timg.addr, timg.siz, timg.width, t.tmem, t.line, 0, 0 };
    uint64_t sig;
    if (op == 0xF0) {   // TLUT: entries [start, start + count)
        const uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2, lrs = ((w1 >> 12) & 0xFFF) >> 2;
        const uint32_t start = t.tmem >= 256 ? t.tmem - 256 : 0, count = lrs >= uls ? lrs - uls + 1 : 0;
        L.lo = start; L.hi = std::min<uint32_t>(256, start + count);
        sig = sample_rdram(timg.addr + uls * 2, count * 2) ^ ((uint64_t)start << 40);
        for (uint32_t b = L.lo >> 4; b < 16 && b * 16 < L.hi; b++) tlut_sig[b] = sig * (b + 1);
        tlut_gen++;
    } else {
        uint32_t bytes, src;
        if (op == 0xF3) {
            const uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF, lrs = (w1 >> 12) & 0xFFF;
            const uint32_t texels = lrs >= uls ? lrs - uls + 1 : 0, sh = timg.siz;
            bytes = ((sh == 0 ? (texels + 1) / 2 : texels << (sh - 1)) + 7) & ~7u;
            const uint32_t row_bytes = timg.width * (sh == 0 ? 1 : (1u << (sh - 1)));
            src = timg.addr + ult * row_bytes + (sh == 0 ? uls / 2 : uls << (sh - 1));
        } else {
            const uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2, lrs = ((w1 >> 12) & 0xFFF) >> 2, lrt = (w1 & 0xFFF) >> 2;
            const uint32_t bpp_bits = 4u << timg.siz;
            bytes = (lrt >= ult ? lrt - ult + 1 : 1) * t.line * 8;
            src = timg.addr + ult * (timg.width * bpp_bits / 8) + uls * bpp_bits / 8;
            const uint32_t rows = lrt >= ult ? lrt - ult + 1 : 1, row_n = (lrs >= uls ? lrs - uls + 1 : 1) * bpp_bits / 8;
            sig = 0;
            for (uint32_t r = 0; r < rows && r < 256; r += std::max<uint32_t>(1, rows / 8))
                sig = (sig ^ sample_rdram(src + r * (timg.width * bpp_bits / 8), row_n)) * 0x100000001B3ull;
            // LOADTILE also sets the tile's size, as the hardware does
            t.uls = (w0 >> 12) & 0xFFF; t.ult = w0 & 0xFFF; t.lrs = (w1 >> 12) & 0xFFF; t.lrt = w1 & 0xFFF;
        }
        L.lo = t.tmem * 8; L.hi = std::min<uint32_t>(4096, L.lo + std::max<uint32_t>(bytes, 8));
        if (op == 0xF3) sig = sample_rdram(src, bytes);
        sig ^= ((uint64_t)op << 56) ^ ((uint64_t)L.lo << 44) ^ ((uint64_t)(w1 & 0xFFF) << 32) ^ ((uint64_t)timg.siz << 30) ^ t.line;
        for (uint32_t w = L.lo >> 3; w < 512 && w * 8 < L.hi; w++) tmem_sig[w] = sig + w;
        if (timg.siz == 3) for (uint32_t w = (L.lo & 0x7FF) >> 3; w < 256 && w * 8 < (L.hi & 0x7FF) + 8; w++) tmem_sig[256 + w] = sig + 256 + w;
    }
    // a later load that overwrites every TMEM byte an earlier one wrote replaces it (a 32-bit load writes
    // the same range in both TMEM halves)
    {
        auto ranges = [](const PendingLoad& X, uint32_t r[2][2]) -> int {
            if (X.op == 0xF0 || X.tsiz != 3) { r[0][0] = X.lo; r[0][1] = X.hi; return 1; }
            const uint32_t a = X.lo & 0x7FF, len = std::min<uint32_t>(X.hi - X.lo, 0x800 - a);
            r[0][0] = a; r[0][1] = a + len; r[1][0] = 0x800 + a; r[1][1] = 0x800 + a + len;
            return 2;
        };
        uint32_t lr[2][2];
        const int ln = ranges(L, lr);
        size_t k = 0;
        for (size_t i = 0; i < pending.size(); i++) {
            const PendingLoad& P = pending[i];
            bool covered = (P.op == 0xF0) == (op == 0xF0);
            if (covered) {
                uint32_t pr[2][2];
                const int pn = ranges(P, pr);
                for (int a = 0; a < pn && covered; a++) {
                    bool in = false;
                    for (int b = 0; b < ln; b++) if (lr[b][0] <= pr[a][0] && pr[a][1] <= lr[b][1]) in = true;
                    covered = in;
                }
            }
            if (!covered) pending[k++] = P;
        }
        pending.resize(k);
    }
    pending.push_back(L);
    if (pending.size() > 64) apply_pending();
    tex_dirty = true;
}

void Renderer3DS::apply_pending() {
    if (pending.empty()) return;
    const auto saved_timg = timg;
    for (const PendingLoad& L : pending) {
        Tile& t = tiles[(L.w1 >> 24) & 7];
        const Tile saved_tile = t;
        timg.addr = L.taddr; timg.siz = L.tsiz; timg.width = L.twidth;
        t.tmem = L.tmem; t.line = L.line;
        if (L.op == 0xF0) load_tlut(L.w0, L.w1);
        else if (L.op == 0xF3) load_block(L.w0, L.w1);
        else load_tile(L.w0, L.w1);
        t = saved_tile;   // the tile as the display list has it now
    }
    timg = saved_timg;
    pending.clear();
}

void Renderer3DS::load_block(uint32_t w0, uint32_t w1) {
    const uint32_t tile = (w1 >> 24) & 7;
    const uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
    const uint32_t lrs = (w1 >> 12) & 0xFFF, dxt = w1 & 0xFFF;
    const Tile& t = tiles[tile];
    const uint32_t bpp_shift = timg.siz;   // 0:4b 1:8b 2:16b 3:32b (in half-bytes: size = 4 << siz bits)
    const uint32_t texels = lrs - uls + 1;
    uint32_t bytes = (bpp_shift == 0) ? (texels + 1) / 2 : texels << (bpp_shift - 1);
    bytes = (bytes + 7) & ~7u;
    const uint32_t row_bytes = (timg.width * ((bpp_shift == 0) ? 1 : (1u << (bpp_shift - 1))));
    uint32_t src = timg.addr + ult * row_bytes + ((bpp_shift == 0) ? uls / 2 : uls << (bpp_shift - 1));
    uint32_t dst = t.tmem * 8;
    const uint32_t words = bytes / 8;
    uint32_t acc = 0;
    if (timg.siz != 3 && (src & 3) == 0 && (dst & 7) == 0) {
        // fast path: whole 32-bit words (RDRAM holds each word in host order; TMEM keeps N64 byte order)
        for (uint32_t k = 0; k < words; k++) {
            const bool odd = dxt ? ((acc >> 11) & 1) : false;
            const uint32_t a = src + k * 8;
            if (a + 8 > RAM_LIMIT) break;
            const uint32_t w0 = __builtin_bswap32(rd32(a));
            const uint32_t w1 = __builtin_bswap32(rd32(a + 4));
            const uint32_t d = (dst + k * 8) & 0xFF8;
            std::memcpy(tmem + d + (odd ? 4 : 0), &w0, 4);
            std::memcpy(tmem + d + (odd ? 0 : 4), &w1, 4);
            acc += dxt;
        }
        tex_dirty = true;
        return;
    }
    for (uint32_t k = 0; k < words; k++) {
        const bool odd = dxt ? ((acc >> 11) & 1) : false;
        for (uint32_t b = 0; b < 8; b++) {
            const uint32_t a = (dst + k * 8 + b) ^ (odd ? 4 : 0);
            if (timg.siz == 3) {
                // RGBA32: RG halves go low, BA halves high
                const uint32_t px = (k * 8 + b) / 4, comp = (k * 8 + b) & 3;
                const uint32_t ta = ((dst + px * 2 + (comp & 1)) ^ (odd ? 4 : 0)) & 0x7FF;
                tmem[(comp < 2 ? ta : ta + 0x800) & 0xFFF] = rd8(src + k * 8 + b);
            } else {
                tmem[a & 0xFFF] = rd8(src + k * 8 + b);
            }
        }
        acc += dxt;
    }
    tex_dirty = true;
}

void Renderer3DS::load_tile(uint32_t w0, uint32_t w1) {
    const uint32_t tile = (w1 >> 24) & 7;
    const uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
    const uint32_t lrs = ((w1 >> 12) & 0xFFF) >> 2, lrt = (w1 & 0xFFF) >> 2;
    Tile& t = tiles[tile];
    const uint32_t bpp_bits = 4u << timg.siz;
    const uint32_t row_bytes_src = timg.width * bpp_bits / 8;
    const uint32_t line = t.line * 8;
    for (uint32_t y = ult; y <= lrt && y - ult < 1024; y++) {
        const uint32_t odd = ((y - ult) & 1) ? 4 : 0;
        const uint32_t src = timg.addr + y * row_bytes_src + uls * bpp_bits / 8;
        const uint32_t n = (lrs - uls + 1) * bpp_bits / 8;
        const uint32_t dst = t.tmem * 8 + (y - ult) * line;
        for (uint32_t b = 0; b < n && b < line; b++) {
            if (timg.siz == 3) {
                const uint32_t px = b / 4, comp = b & 3;
                const uint32_t ta = ((dst + px * 2 + (comp & 1)) ^ odd) & 0x7FF;
                tmem[(comp < 2 ? ta : ta + 0x800) & 0xFFF] = rd8(src + b);
            } else {
                tmem[((dst + b) ^ odd) & 0xFFF] = rd8(src + b);
            }
        }
    }
    t.uls = ((w0 >> 12) & 0xFFF); t.ult = (w0 & 0xFFF); t.lrs = ((w1 >> 12) & 0xFFF); t.lrt = (w1 & 0xFFF);
    tex_dirty = true;
}

void Renderer3DS::load_tlut(uint32_t w0, uint32_t w1) {
    const uint32_t tile = (w1 >> 24) & 7;
    const uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2;
    const uint32_t lrs = ((w1 >> 12) & 0xFFF) >> 2;
    const Tile& t = tiles[tile];
    const uint32_t start = t.tmem >= 256 ? t.tmem - 256 : 0;
    for (uint32_t i = 0; i <= lrs - uls && i < 256; i++) {
        const uint32_t a = timg.addr + (uls + i) * 2;
        if (start + i < 256) tlut[start + i] = rd16(a);
    }
    tlut_gen++;
    tex_dirty = true;
}

// ===================================================================== draw state
void Renderer3DS::build_state(DrawState& s, uint8_t mode) {
    std::memset(&s, 0, sizeof s);
    s.mode = mode;
    s.combine_hi = combine_hi; s.combine_lo = combine_lo;
    s.other_h = other_h; s.other_l = other_l; s.geom = geom;
    s.prim = prim_color; s.env = env_color; s.fog = fog_color; s.blend = blend_color;
    s.fog_on = (mode == 0 && (geom & G_FOG)) ? 1 : 0;
    s.tex = nullptr;
    const int sx0 = scissor[0] / 4, sy0 = scissor[1] / 4, sx1 = scissor[2] / 4, sy1 = scissor[3] / 4;
    s.sc[0] = (int16_t)std::max(0, sx0);
    s.sc[1] = (int16_t)std::max(0, canvas_h - sy1);
    s.sc[2] = (int16_t)std::min(canvas_w, sx1);
    s.sc[3] = (int16_t)std::min(canvas_h, canvas_h - sy0);
}

// N64 combiner input -> PICA TEV source/operand
struct TevIn { GPU_TEVSRC src; int op; uint32_t konst; bool needs_const; bool zero; bool one; };

static TevIn color_in(uint32_t sel, int slot, bool cycle2, const DrawState& s) {
    // slot: 0 A, 1 B, 2 C, 3 D
    TevIn r{ GPU_PRIMARY_COLOR, GPU_TEVOP_RGB_SRC_COLOR, 0, false, false, false };
    auto konst = [&](uint32_t c, int op) { r.src = GPU_CONSTANT; r.konst = c; r.needs_const = true; r.op = op; };
    if (slot == 2) {
        switch (sel) {
        case 0: r.src = cycle2 ? GPU_PREVIOUS : GPU_PRIMARY_COLOR; break;
        case 1: r.src = GPU_TEXTURE0; break;
        case 2: r.src = s.tex1 ? GPU_TEXTURE1 : GPU_TEXTURE0; break;
        case 3: konst(s.prim, GPU_TEVOP_RGB_SRC_COLOR); break;
        case 4: r.src = GPU_PRIMARY_COLOR; break;
        case 5: konst(s.env, GPU_TEVOP_RGB_SRC_COLOR); break;
        case 7: r.src = cycle2 ? GPU_PREVIOUS : GPU_PRIMARY_COLOR; r.op = GPU_TEVOP_RGB_SRC_ALPHA; break;
        case 8: r.src = GPU_TEXTURE0; r.op = GPU_TEVOP_RGB_SRC_ALPHA; break;
        case 9: r.src = s.tex1 ? GPU_TEXTURE1 : GPU_TEXTURE0; r.op = GPU_TEVOP_RGB_SRC_ALPHA; break;
        case 10: konst(s.prim, GPU_TEVOP_RGB_SRC_ALPHA); break;
        case 11: r.src = GPU_PRIMARY_COLOR; r.op = GPU_TEVOP_RGB_SRC_ALPHA; break;
        case 12: konst(s.env, GPU_TEVOP_RGB_SRC_ALPHA); break;
        case 14: konst(s.prim, GPU_TEVOP_RGB_SRC_ALPHA); break;   // prim LOD frac ~ prim alpha
        default: r.zero = true; break;
        }
        return r;
    }
    switch (sel) {
    case 0: r.src = cycle2 ? GPU_PREVIOUS : GPU_PRIMARY_COLOR; break;
    case 1: r.src = GPU_TEXTURE0; break;
    case 2: r.src = s.tex1 ? GPU_TEXTURE1 : GPU_TEXTURE0; break;
    case 3: konst(s.prim, GPU_TEVOP_RGB_SRC_COLOR); break;
    case 4: r.src = GPU_PRIMARY_COLOR; break;
    case 5: konst(s.env, GPU_TEVOP_RGB_SRC_COLOR); break;
    case 6: if (slot == 1) r.zero = true; else { r.one = true; konst(0xFFFFFFFF, GPU_TEVOP_RGB_SRC_COLOR); } break;
    case 7: if (slot == 3) r.zero = true; else r.zero = true; break;
    default: r.zero = true; break;
    }
    if (slot == 3 && sel == 6) { r.zero = false; r.one = true; konst(0xFFFFFFFF, GPU_TEVOP_RGB_SRC_COLOR); }
    return r;
}

static TevIn alpha_in(uint32_t sel, int slot, bool cycle2, const DrawState& s) {
    TevIn r{ GPU_PRIMARY_COLOR, GPU_TEVOP_A_SRC_ALPHA, 0, false, false, false };
    auto konst = [&](uint32_t c) { r.src = GPU_CONSTANT; r.konst = c; r.needs_const = true; };
    if (slot == 2) {
        switch (sel) {
        case 1: r.src = GPU_TEXTURE0; break;
    case 2: r.src = s.tex1 ? GPU_TEXTURE1 : GPU_TEXTURE0; break;
        case 3: konst(s.prim); break;
        case 4: r.src = GPU_PRIMARY_COLOR; break;
        case 5: konst(s.env); break;
        case 6: konst(s.prim); break;
        default: r.zero = true; break;   // 0 LOD frac, 7 zero
        }
        return r;
    }
    switch (sel) {
    case 0: r.src = cycle2 ? GPU_PREVIOUS : GPU_PRIMARY_COLOR; break;
    case 1: r.src = GPU_TEXTURE0; break;
    case 2: r.src = s.tex1 ? GPU_TEXTURE1 : GPU_TEXTURE0; break;
    case 3: konst(s.prim); break;
    case 4: r.src = GPU_PRIMARY_COLOR; break;
    case 5: konst(s.env); break;
    case 6: r.one = true; konst(0xFFFFFFFF); break;
    default: r.zero = true; break;
    }
    return r;
}

// one N64 (A-B)*C+D for one channel set -> up to 2 PICA stages
struct StageOp { GPU_COMBINEFUNC fn; TevIn in[3]; int nin; };

static int plan_op(const TevIn& A, const TevIn& B, const TevIn& C, const TevIn& D, StageOp out[2], GPU_TEVSRC prev_src) {
    auto same = [](const TevIn& x, const TevIn& y) { return x.src == y.src && x.op == y.op && x.konst == y.konst && x.zero == y.zero; };
    const TevIn zero_k{ GPU_CONSTANT, 0, 0, true, true, false };
    if (C.zero || (same(A, B) && !A.zero)) {
        if (D.zero) { out[0] = { GPU_REPLACE, { zero_k }, 1 }; return 1; }
        out[0] = { GPU_REPLACE, { D }, 1 };
        return 1;
    }
    if (B.zero && D.zero) {
        if (A.zero) { out[0] = { GPU_REPLACE, { zero_k }, 1 }; return 1; }
        if (A.one) { out[0] = { GPU_REPLACE, { C }, 1 }; return 1; }
        if (C.one) { out[0] = { GPU_REPLACE, { A }, 1 }; return 1; }
        out[0] = { GPU_MODULATE, { A, C }, 2 };
        return 2 - 1;
    }
    if (B.zero) {
        if (A.one) { out[0] = { GPU_ADD, { C, D }, 2 }; return 1; }
        out[0] = { GPU_MULTIPLY_ADD, { A, C, D }, 3 };
        return 1;
    }
    if (same(B, D)) {
        out[0] = { GPU_INTERPOLATE, { A, B, C }, 3 };
        return 1;
    }
    // general: (A - B) then * C + D
    out[0] = { GPU_SUBTRACT, { A.zero ? zero_k : A, B }, 2 };
    TevIn prev{ prev_src, A.op, 0, false, false, false };
    prev.src = GPU_PREVIOUS;
    prev.op = 0;
    if (D.zero) out[1] = { GPU_MODULATE, { prev, C }, 2 };
    else out[1] = { GPU_MULTIPLY_ADD, { prev, C, D }, 3 };
    return 2;
}

void Renderer3DS::setup_tev(const DrawState& s) {
    int stage = 0;
    auto set_stage = [&](const StageOp* col, const StageOp* alp) {
        C3D_TexEnv* env = C3D_GetTexEnv(stage);
        C3D_TexEnvInit(env);
        uint32_t k = 0;
        bool have_k = false;
        auto src_of = [&](const TevIn& in) -> GPU_TEVSRC {
            if (in.needs_const) {
                if (!have_k) { k = in.konst; have_k = true; }
                return GPU_CONSTANT;
            }
            return in.src;
        };
        GPU_TEVSRC cs[3] = { GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS }, as[3] = { GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS };
        int co[3] = { 0, 0, 0 }, ao[3] = { 0, 0, 0 };
        GPU_COMBINEFUNC cf = GPU_REPLACE, af = GPU_REPLACE;
        if (col) { cf = col->fn; for (int i = 0; i < col->nin; i++) { cs[i] = src_of(col->in[i]); co[i] = col->in[i].op; } }
        if (alp) { af = alp->fn; for (int i = 0; i < alp->nin; i++) { as[i] = src_of(alp->in[i]); ao[i] = alp->in[i].op; } }
        C3D_TexEnvSrc(env, C3D_RGB, cs[0], cs[1], cs[2]);
        C3D_TexEnvSrc(env, C3D_Alpha, as[0], as[1], as[2]);
        C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB)co[0], (GPU_TEVOP_RGB)co[1], (GPU_TEVOP_RGB)co[2]);
        C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A)ao[0], (GPU_TEVOP_A)ao[1], (GPU_TEVOP_A)ao[2]);
        C3D_TexEnvFunc(env, C3D_RGB, cf);
        C3D_TexEnvFunc(env, C3D_Alpha, af);
        // PICA constant is ABGR in a u32 (r in the low byte)
        const uint32_t abgr = ((k >> 24) & 0xFF) | (((k >> 16) & 0xFF) << 8) | (((k >> 8) & 0xFF) << 16) | ((k & 0xFF) << 24);
        C3D_TexEnvColor(env, abgr);
        stage++;
    };

    const uint32_t cyc = (s.other_h >> 20) & 3;
    if (s.mode == 1 || s.mode == 3) {
        // fill: constant fill colour (stored in prim slot)
        StageOp c{ GPU_REPLACE, { TevIn{ GPU_CONSTANT, 0, s.prim, true, false, false } }, 1 };
        StageOp a{ GPU_REPLACE, { TevIn{ GPU_CONSTANT, 0, s.prim, true, false, false } }, 1 };
        set_stage(&c, &a);
    } else if (s.mode == 2 || cyc == 2) {
        StageOp c{ GPU_REPLACE, { TevIn{ GPU_TEXTURE0, 0, 0, false, false, false } }, 1 };
        set_stage(&c, &c);
    } else {
        const uint32_t hi = s.combine_hi, lo = s.combine_lo;
        const uint32_t ca[2] = { (hi >> 20) & 0xF, (hi >> 5) & 0xF };
        const uint32_t cc[2] = { (hi >> 15) & 0x1F, hi & 0x1F };
        const uint32_t aa[2] = { (hi >> 12) & 7, (lo >> 21) & 7 };
        const uint32_t ac[2] = { (hi >> 9) & 7, (lo >> 18) & 7 };
        const uint32_t cb[2] = { (lo >> 28) & 0xF, (lo >> 24) & 0xF };
        const uint32_t cd[2] = { (lo >> 15) & 7, (lo >> 6) & 7 };
        const uint32_t ab[2] = { (lo >> 12) & 7, (lo >> 3) & 7 };
        const uint32_t ad[2] = { (lo >> 9) & 7, lo & 7 };
        const int ncyc = cyc == 1 ? 2 : 1;
        for (int cy = (ncyc == 2 ? 0 : 1); cy < 2; cy++) {
            // 1-cycle mode uses the second cycle's settings (the first is often a copy of it)
            const bool c2 = (ncyc == 2 && cy == 1) || stage > 0;
            StageOp col[2], alp[2];
            const int nc = plan_op(color_in(ca[cy], 0, c2, s), color_in(cb[cy], 1, c2, s), color_in(cc[cy], 2, c2, s), color_in(cd[cy], 3, c2, s), col, GPU_PREVIOUS);
            const int na = plan_op(alpha_in(aa[cy], 0, c2, s), alpha_in(ab[cy], 1, c2, s), alpha_in(ac[cy], 2, c2, s), alpha_in(ad[cy], 3, c2, s), alp, GPU_PREVIOUS);
            const StageOp pass{ GPU_REPLACE, { TevIn{ GPU_PREVIOUS, 0, 0, false, false, false } }, 1 };
            set_stage(&col[0], &alp[0]);
            if (nc > 1 || na > 1) set_stage(nc > 1 ? &col[1] : &pass, na > 1 ? &alp[1] : &pass);
        }
    }
    if (s.fog_on) {
        // fog: mix(prev, fog colour, ramp alpha at the vertex fog amount)
        C3D_TexEnv* env = C3D_GetTexEnv(stage);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_TEXTURE2);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        const uint32_t k = s.fog;
        C3D_TexEnvColor(env, ((k >> 24) & 0xFF) | (((k >> 16) & 0xFF) << 8) | (((k >> 8) & 0xFF) << 16) | 0xFF000000u);
        stage++;
    }
    for (int i = stage; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
}

// Re-issues only the parts of the GPU state that differ from cur_state (everything when the bound state is unknown).
void Renderer3DS::apply_state(const DrawState& s) {
    const DrawState& c = cur_state;
    const bool full = !gpu_state_valid;
    const bool shared = s.tex && s.tex == s.tex1;
    // texture
    if (!full && !shared && s.tex == c.tex && s.texp == c.texp) {
    } else if (s.tex) {
        const GPU_TEXTURE_FILTER_PARAM f = (s.texp & 1) ? GPU_LINEAR : GPU_NEAREST;
        const GPU_TEXTURE_WRAP_PARAM ws = (s.texp & 2) ? GPU_CLAMP_TO_EDGE : ((s.texp & 8) ? GPU_MIRRORED_REPEAT : GPU_REPEAT);
        const GPU_TEXTURE_WRAP_PARAM wt = (s.texp & 4) ? GPU_CLAMP_TO_EDGE : ((s.texp & 16) ? GPU_MIRRORED_REPEAT : GPU_REPEAT);
        C3D_TexSetFilter(s.tex, f, f);
        C3D_TexSetWrap(s.tex, ws, wt);
        C3D_TexBind(0, s.tex);
    } else {
        C3D_TexBind(0, &white);
    }
    if (!full && !shared && s.tex1 == c.tex1 && s.texp1 == c.texp1) {
    } else if (s.tex1) {
        const GPU_TEXTURE_FILTER_PARAM f = (s.texp1 & 1) ? GPU_LINEAR : GPU_NEAREST;
        const GPU_TEXTURE_WRAP_PARAM ws = (s.texp1 & 2) ? GPU_CLAMP_TO_EDGE : ((s.texp1 & 8) ? GPU_MIRRORED_REPEAT : GPU_REPEAT);
        const GPU_TEXTURE_WRAP_PARAM wt = (s.texp1 & 4) ? GPU_CLAMP_TO_EDGE : ((s.texp1 & 16) ? GPU_MIRRORED_REPEAT : GPU_REPEAT);
        C3D_TexSetFilter(s.tex1, f, f);
        C3D_TexSetWrap(s.tex1, ws, wt);
        C3D_TexBind(1, s.tex1);
    } else {
        C3D_TexBind(1, &white);
    }
    {
        TevKey k;
        std::memset(&k, 0, sizeof k);
        k.hi = s.combine_hi; k.lo = s.combine_lo; k.cyc = (s.other_h >> 20) & 3;
        k.prim = s.prim; k.env = s.env; k.fog = s.fog; k.mode = s.mode; k.fog_on = s.fog_on; k.tex1 = s.tex1 != nullptr;
        if (!tev_valid || !(k == tev_key)) {
            setup_tev(s);
            tev_key = k;
            tev_valid = true;
        }
    }

    const uint32_t L = s.other_l;
    const bool raster_same = !full && s.mode == c.mode && s.other_l == c.other_l && s.geom == c.geom && s.blend == c.blend;
    if (raster_same) {
        if (s.sc[0] != c.sc[0] || s.sc[1] != c.sc[1] || s.sc[2] != c.sc[2] || s.sc[3] != c.sc[3]) apply_scissor(s);
        return;
    }

    // depth
    if (s.mode == 3) {
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);
    } else if (s.mode == 1 || s.mode == 2) {
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    } else {
        const bool zon = (s.geom & G_ZBUFFER) || (L & 0x30);
        const bool zcmp = zon && (L & 0x10);
        const bool zupd = zon && (L & 0x20);
        C3D_DepthTest(zcmp || zupd, zcmp ? GPU_GEQUAL : GPU_ALWAYS, zupd ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    }

    // blend: second-cycle blender CLR_IN*A_IN + CLR_MEM*1MA with FORCE_BL = alpha blend
    const uint32_t m2a = (L >> 20) & 3, m2b = (L >> 16) & 3;
    const bool force_bl = (L & 0x4000) != 0;
    if (s.mode == 1 || s.mode == 3) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    } else if (force_bl && m2a == 1 && m2b == 0) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    } else if (force_bl && m2a == 1 && m2b == 2) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
    } else {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }

    // alpha test: threshold compare, or coverage-from-alpha cutouts
    if (s.mode == 0 && ((L & 3) == 1)) C3D_AlphaTest(true, GPU_GREATER, (int)(s.blend & 0xFF) > 0 ? (int)(s.blend & 0xFF) - 1 : 0);
    else if (s.mode == 0 && (L & 0x2000) && (L & 0x1000)) C3D_AlphaTest(true, GPU_GREATER, 0x40);
    else if (s.mode == 2) C3D_AlphaTest(true, GPU_GREATER, 0);
    else C3D_AlphaTest(false, GPU_ALWAYS, 0);
    apply_scissor(s);
}

void Renderer3DS::apply_scissor(const DrawState& s) {
    if (s.sc[2] > s.sc[0] && s.sc[3] > s.sc[1])
        C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)s.sc[0], (u32)s.sc[1], (u32)s.sc[2], (u32)s.sc[3]);
    else
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

// switches between the canvas and the radar target (the bound state stays; viewport and scissor are per target)
void Renderer3DS::route_target(bool radar) {
    if (radar == on_radar_rt) return;
    on_radar_rt = radar;
    if (radar) {
        C3D_FrameDrawOn(radar_rt);
        C3D_SetViewport(0, 0, RADAR_TS, RADAR_TS);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    } else {
        C3D_FrameDrawOn(canvas_rt);
        C3D_SetViewport(0, 0, (u32)canvas_w, (u32)canvas_h);
        gpu_state_valid = false;
    }
}

void Renderer3DS::flush() {
    if (!have_batch || batch_count == 0) { have_batch = false; return; }
    struct Prof { uint64_t& acc; uint64_t t0; ~Prof() { acc += svcGetSystemTick() - t0; } } prof{ prof_flush, svcGetSystemTick() };
    route_target(batch_state.radar != 0);
    if (!gpu_state_valid || !(batch_state == cur_state)) {
        apply_state(batch_state);
        cur_state = batch_state;
        gpu_state_valid = true;
    }
    if (dump_tasks > 0 && dump_batches < 600) {
        dump_batches++;
        const DrawState& d = batch_state;
        const Vtx& v = *(const Vtx*)(vring[frame & 1] + (size_t)batch_first * sizeof(Vtx));
        std::fprintf(stderr, "[dump] draw tris=%u mode=%u mtx=%06X cc=%06X:%08X oh=%08X ol=%08X geom=%08X prim=%08X env=%08X fog=%d tex=%p/%p texp=%02X v0=(%.1f %.1f %.1f %.1f) c=%02X%02X%02X%02X uv=(%.3f %.3f)\n",
            batch_count / 3, d.mode, dump_mtx_addr & 0xFFFFFF, d.combine_hi, d.combine_lo, d.other_h, d.other_l, d.geom, d.prim, d.env, d.fog_on,
            (void*)d.tex, (void*)d.tex1, d.texp, v.x, v.y, v.z, v.w, v.c[0], v.c[1], v.c[2], v.c[3], v.u, v.v);
    }
    const float xs = batch_state.xs ? 0.8f : 1.f;
    if (!stereo_on || batch_state.radar) {
        set_stereo_unif(0.f, xs);
        C3D_DrawArrays(GPU_TRIANGLES, (int)batch_first, (int)batch_count);
    } else {
        const DrawState& d = batch_state;
        for (int eye = 0; eye < 2; eye++) {
            const int ox = eye * RIGHT_EYE_X;
            C3D_SetViewport((u32)ox, 0, (u32)canvas_w, (u32)canvas_h);
            if (d.sc[2] > d.sc[0] && d.sc[3] > d.sc[1])
                C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(d.sc[0] + ox), (u32)d.sc[1], (u32)(d.sc[2] + ox), (u32)d.sc[3]);
            else
                C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)ox, 0, (u32)(ox + canvas_w), (u32)canvas_h);
            const float k = d.flat ? 0.f : (eye ? stereo_k : -stereo_k);
            set_stereo_unif(k, xs);
            C3D_DrawArrays(GPU_TRIANGLES, (int)batch_first, (int)batch_count);
        }
    }
    stat_draws++;
    have_batch = false;
    batch_count = 0;
}

uint8_t Renderer3DS::tile_texp(const Tile& t, bool linear) const {
    return (uint8_t)((linear ? 1 : 0) | ((t.cms & 2) && !t.masks ? 2 : 0) | ((t.cmt & 2) && !t.maskt ? 4 : 0) |
                     ((t.cms & 1) && t.masks ? 8 : 0) | ((t.cmt & 1) && t.maskt ? 16 : 0));
}

void Renderer3DS::emit_tris(const Vtx* v, int n, uint8_t mode) {
    if (n <= 0) return;
    struct ProfX { uint64_t& acc; uint64_t& fl; uint64_t t0, f0; ~ProfX() { acc += (svcGetSystemTick() - t0) - (fl - f0); } } profx{ prof_emit, prof_flush, svcGetSystemTick(), prof_flush };
    frame_begin();
    DrawState s;
    build_state(s, mode);
    s.flat = (std::fabs(v[0].w - 1.f) < 1e-4f && std::fabs(v[n - 1].w - 1.f) < 1e-4f) ? 1 : 0;
    Vtx* rv = nullptr;
    bool to_radar = radar_pass && crop_valid && crop_h > 1.f;
    if (to_radar && s.flat) {
        // 2D pieces of the radar (red ring, player marker): the ones that lie around the radar
        const float ncx = crop_cx / (canvas_w * 0.5f) - 1.f, ncy = 1.f - crop_cy / (canvas_h * 0.5f);
        const float rx = crop_h * 0.8f / (canvas_w * 0.5f), ry = crop_h * 0.8f / (canvas_h * 0.5f);
        for (int i = 0; i < n && to_radar; i++)
            to_radar = std::fabs(v[i].x - ncx) <= rx && std::fabs(v[i].y - ncy) <= ry;
    }
    if (to_radar) {
        // canvas clip -> radar target clip: the square crop around the radar fills the target
        s.radar = 1;
        s.sc[0] = s.sc[1] = s.sc[2] = s.sc[3] = 0;
        const float ncx = crop_cx / (canvas_w * 0.5f) - 1.f, ncy = 1.f - crop_cy / (canvas_h * 0.5f);
        const float kx = (float)canvas_w / crop_h, ky = (float)canvas_h / crop_h;
        scratch_radar.assign(v, v + n);
        for (Vtx& o : scratch_radar) {
            o.x = (o.x - ncx * o.w) * kx;
            o.y = (o.y - ncy * o.w) * ky;
        }
        rv = scratch_radar.data();
        radar_drawn = true;
    }
    if (rv) v = rv;
    if (g_rs3ds_widescreen && !s.radar) {
        // widescreen: squeeze x by 0.8 so the wider view (the culls are widened by rs64_cull_widen) fits the canvas, which is then
        // shown across all 400 px; 2D pieces keep their shape around the centre, full-width ones (backgrounds, clears) stay as they are
        bool squeeze = true;
        if (s.flat) {
            float lo = v[0].x, hi = v[0].x;
            for (int i = 1; i < n; i++) { lo = std::min(lo, v[i].x); hi = std::max(hi, v[i].x); }
            squeeze = !(lo <= -0.98f && hi >= 0.98f);
        }
        s.xs = squeeze ? 1 : 0;
    }
    const bool textured = (mode == 2) || (mode == 0 && tex_on);
    if (textured) {
        s.tex = cur_tex;
        const bool linear = ((other_h >> 12) & 3) != 0 && mode != 2;
        s.texp = tile_texp(tiles[tex_tile & 7], linear);
        if (mode == 0 && use_tex1) {
            s.tex1 = cur_tex1;
            s.texp1 = tile_texp(tiles[(tex_tile + 1) & 7], linear);
        }
    }
    // vertices into this frame's ring
    const size_t at = (vused + sizeof(Vtx) - 1) / sizeof(Vtx);
    if ((at + (size_t)n) * sizeof(Vtx) > VRING_BYTES) {
        if (!ring_full_logged) { std::printf("[r3ds] vertex ring full: draws dropped\n"); ring_full_logged = true; }
        return;
    }
    if (have_batch && (!(s == batch_state) || at != batch_first + batch_count)) flush();
    std::memcpy(vring[frame & 1] + at * sizeof(Vtx), v, sizeof(Vtx) * (size_t)n);
    vused = (at + (size_t)n) * sizeof(Vtx);
    if (!have_batch) {
        batch_state = s;
        batch_first = (uint32_t)at;
        batch_count = 0;
        have_batch = true;
    }
    batch_count += (uint32_t)n;
    drew = true;
    stat_tris += (uint32_t)n / 3;
}

void Renderer3DS::make_vtx(const CacheVtx& c, Vtx& o, float s_texel, float t_texel, bool textured) {
    o.x = c.cx; o.y = c.cy; o.z = c.cz; o.w = c.cw;
    if ((other_l & 0x04) && prim_depth != 0xFFFF) {   // depth from the primitive (sky, sprites)
        const float zw = (float)(prim_depth & 0x7FFF) / 32767.f;
        o.z = (2.f * zw - 1.f) * c.cw;
    }
    if (textured) {
        // N64 texture shift: 1..10 divide by 2^n, 11..15 multiply by 2^(16-n)
        static const float kShift[16] = { 1.f, 1.f / 2, 1.f / 4, 1.f / 8, 1.f / 16, 1.f / 32, 1.f / 64, 1.f / 128, 1.f / 256, 1.f / 512, 1.f / 1024,
                                          32.f, 16.f, 8.f, 4.f, 2.f };
        const Tile& t = tiles[tex_tile & 7];
        o.u = (s_texel * kShift[t.shifts & 15] - (float)t.uls * 0.25f) * inv_tw;
        o.v = (t_texel * kShift[t.shiftt & 15] - (float)t.ult * 0.25f) * inv_th;
        if (use_tex1) {
            const Tile& t1 = tiles[(tex_tile + 1) & 7];
            o.u1 = (s_texel * kShift[t1.shifts & 15] - (float)t1.uls * 0.25f) * inv_tw1;
            o.v1 = (t_texel * kShift[t1.shiftt & 15] - (float)t1.ult * 0.25f) * inv_th1;
        } else {
            o.u1 = o.u; o.v1 = o.v;
        }
    } else {
        o.u = o.v = o.u1 = o.v1 = 0.f;
    }
    std::memcpy(o.c, c.c, 4);
    o.fu = c.fog; o.fv = 0.5f;
}

// a face from vertex-cache indices; st = raw per-face UVs (scaled by the 03 82 texcoord scale)
void Renderer3DS::face(const uint32_t* idx, int n, const uint32_t* st, uint32_t color_word) {
    if (++faces > 65536) return;
    CacheVtx cv[4];
    const uint32_t cofs[4] = { (color_word >> 16) & 0xFF, (color_word >> 8) & 0xFF, color_word & 0xFF, color_word >> 24 };
    const bool have_colors = color_buf >= 0x80000000u && in_ram((color_buf & RAM_MASK), 0x100);
    for (int k = 0; k < n; k++) {
        if (idx[k] >= vcache_n) { if (dump_tasks > 0) dump_dropped_idx++; return; }
        cv[k] = vcache[idx[k]];
        if (have_colors) {
            const uint32_t c = rd32((color_buf & RAM_MASK) + cofs[k]);
            cv[k].c[0] = (uint8_t)(c >> 24); cv[k].c[1] = (uint8_t)(c >> 16); cv[k].c[2] = (uint8_t)(c >> 8); cv[k].c[3] = (uint8_t)c;
        }
    }
    if (((other_h >> 20) & 3) == 2) return;   // copy mode + triangles: undefined on hardware
    // No back-face culling: the F5 notes leave its sign ambiguous, and the wrong sign hid every craft and
    // laser. Both faces are drawn and the depth test sorts them (RS3DS_CULL=1 brings the CPU cull back).
#ifdef RS3DS_CULL
    if ((geom & G_CULL_BACK) && cv[0].cw > 0 && cv[1].cw > 0 && cv[2].cw > 0) {
        const float cr = (cv[1].sx - cv[0].sx) * (cv[2].sy - cv[0].sy) - (cv[1].sy - cv[0].sy) * (cv[2].sx - cv[0].sx);
        if (cr > 0) return;
    }
#endif
    const bool textured = st != nullptr && tex_on;
    if (textured) resolve_texture();
    Vtx v[6];
    float s[4] = { 0, 0, 0, 0 }, t[4] = { 0, 0, 0, 0 };
    if (st) {
        for (int k = 0; k < n; k++) {
            const int16_t rs = (int16_t)(st[k] >> 16), rt = (int16_t)(st[k] & 0xFFFF);
            int64_t vs = ((int64_t)rs * tc_scale[0]) >> 16, vt = ((int64_t)rt * tc_scale[1]) >> 16;
            vs = std::clamp<int64_t>(vs, -32768, 32767);
            vt = std::clamp<int64_t>(vt, -32768, 32767);
            s[k] = (float)vs / 32.f;
            t[k] = (float)vt / 32.f;
        }
    }
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    const int nv = n == 4 ? 6 : 3;
    for (int i = 0; i < nv; i++) make_vtx(cv[tri[i]], v[i], s[tri[i]], t[tri[i]], textured);
    if (!textured) {
        const bool saved = tex_on;
        tex_on = false;
        emit_tris(v, nv, 0);
        tex_on = saved;
    } else {
        emit_tris(v, nv, 0);
    }
}

// a screen-space rectangle (texrect / fillrect), coordinates in N64 pixels
void Renderer3DS::rect(float ulx, float uly, float lrx, float lry, float s0, float t0, float dsdx, float dtdy, bool textured, bool flip, uint8_t mode, uint32_t color) {
    if (lrx <= ulx || lry <= uly) return;
    const float hw = canvas_w * 0.5f, hh = canvas_h * 0.5f;
    const float xs[4] = { ulx, lrx, lrx, ulx }, ys[4] = { uly, uly, lry, lry };
    const float w = lrx - ulx, h = lry - uly;
    float ss[4], ts[4];
    if (!flip) {
        ss[0] = s0; ss[1] = s0 + w * dsdx; ss[2] = s0 + w * dsdx; ss[3] = s0;
        ts[0] = t0; ts[1] = t0; ts[2] = t0 + h * dtdy; ts[3] = t0 + h * dtdy;
    } else {
        ss[0] = s0; ss[1] = s0; ss[2] = s0 + h * dsdx; ss[3] = s0 + h * dsdx;
        ts[0] = t0; ts[1] = t0 + w * dtdy; ts[2] = t0 + w * dtdy; ts[3] = t0;
    }
    float z = -1.f;   // GL near: rects draw in front
    if (mode == 3) z = 1.f;   // depth clear: far
    else if (other_l & 0x04) z = 2.f * ((float)(prim_depth & 0x7FFF) / 32767.f) - 1.f;
    static const int ord[6] = { 0, 1, 2, 0, 2, 3 };
    Vtx v[6];
    const Tile& t = tiles[tex_tile & 7];
    for (int k = 0; k < 6; k++) {
        const int i = ord[k];
        v[k].x = xs[i] / hw - 1.f; v[k].y = 1.f - ys[i] / hh; v[k].z = z; v[k].w = 1.f;
        if (textured) {
            v[k].u = (ss[i] - (float)t.uls / 4.f) / (float)cur_tw;
            v[k].v = (ts[i] - (float)t.ult / 4.f) / (float)cur_th;
        } else {
            v[k].u = v[k].v = 0.f;
        }
        v[k].c[0] = (uint8_t)(color >> 24); v[k].c[1] = (uint8_t)(color >> 16); v[k].c[2] = (uint8_t)(color >> 8); v[k].c[3] = (uint8_t)color;
        v[k].u1 = v[k].u; v[k].v1 = v[k].v;
        v[k].fu = 0.f; v[k].fv = 0.5f;
    }
    const bool saved = tex_on;
    tex_on = textured;
    const uint32_t saved_geom = geom;
    geom &= ~G_FOG;
    if (mode == 1 || mode == 3) {
        const uint32_t saved_prim = prim_color;
        prim_color = color;
        emit_tris(v, 6, mode);
        prim_color = saved_prim;
    } else {
        emit_tris(v, 6, mode);
    }
    geom = saved_geom;
    tex_on = saved;
}

void Renderer3DS::fill_rect(uint32_t w0, uint32_t w1) {
    float lrx = (float)((w0 >> 12) & 0xFFF) / 4.f, lry = (float)(w0 & 0xFFF) / 4.f;
    float ulx = (float)((w1 >> 12) & 0xFFF) / 4.f, uly = (float)(w1 & 0xFFF) / 4.f;
    const uint32_t cyc = (other_h >> 20) & 3;
    if (cyc == 3 || cyc == 2) { lrx += 1.f; lry += 1.f; }
    if (cyc == 3) {
        const bool depth_clear = (cimg.addr & RAM_MASK) == (zimg & RAM_MASK);
        uint32_t c;
        if (cimg.siz == 3) c = fill_color;
        else c = rgba5551((uint16_t)(fill_color >> 16)) | 0xFF;
        rect(ulx, uly, lrx, lry, 0, 0, 0, 0, false, false, depth_clear ? 3 : 1, c);
    } else {
        rect(ulx, uly, lrx, lry, 0, 0, 0, 0, false, false, 0, 0xFFFFFFFF);
    }
}

void Renderer3DS::tex_rect(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, bool flip) {
    const uint32_t tile = (w1 >> 24) & 7;
    float lrx = (float)((w0 >> 12) & 0xFFF) / 4.f, lry = (float)(w0 & 0xFFF) / 4.f;
    const float ulx = (float)((w1 >> 12) & 0xFFF) / 4.f, uly = (float)(w1 & 0xFFF) / 4.f;
    const float s = (float)(int16_t)(w2 >> 16) / 32.f, t = (float)(int16_t)(w2 & 0xFFFF) / 32.f;
    float dsdx = (float)(int16_t)(w3 >> 16) / 1024.f, dtdy = (float)(int16_t)(w3 & 0xFFFF) / 1024.f;
    const uint32_t cyc = (other_h >> 20) & 3;
    if (cyc == 2 || cyc == 3) { lrx += 1.f; lry += 1.f; dsdx /= 4.f; }
    const uint32_t saved = tex_tile;
    if (tex_tile != tile) { tex_tile = tile; tex_dirty = true; }
    resolve_texture();
    rect(ulx, uly, lrx, lry, s, t, dsdx, dtdy, true, flip, cyc == 2 ? 2 : 0, 0xFFFFFFFF);
    if (saved != tile) { tex_tile = saved; tex_dirty = true; }
}

// 0xBD: camera-facing billboard around a cached vertex (overlay 0x2C)
void Renderer3DS::sprite(uint32_t pc) {
    const uint32_t w0 = rd32(pc), w1 = rd32(pc + 4);
    const uint32_t slot = ((w0 >> 5) & 0x7F8u) / 0x28u;
    if (slot >= vcache_n) return;
    const CacheVtx& c = vcache[slot];
    const uint32_t w3 = rd32(pc + 12), w4 = rd32(pc + 16);
    const bool swap = (w0 & 1u) != 0;
    int16_t hx = (int16_t)(w3 >> 16), hy = (int16_t)(w3 & 0xFFFF);
    if (swap) std::swap(hx, hy);
    if (hx <= 0 || hx > 4000) hx = 40;
    if (hy <= 0 || hy > 4000) hy = 40;
    const float texS = (float)(int16_t)(w4 >> 16), texT = (float)(int16_t)(w4 & 0xFFFF);
    update_mvp();
    // world directions that project to screen x / y: cross products of the MVP columns
    const float cX[3] = { mvp[0], mvp[4], mvp[8] }, cY[3] = { mvp[1], mvp[5], mvp[9] }, cW[3] = { mvp[3], mvp[7], mvp[11] };
    auto cross = [](const float a[3], const float b[3], float o[3]) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; };
    auto norm = [](float v[3]) { const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; } };
    float R[3], U[3];
    cross(cY, cW, R); cross(cW, cX, U); norm(R); norm(U);
    const float sgx[4] = { -1, 1, -1, 1 }, sgy[4] = { -1, -1, 1, 1 };
    CacheVtx q[4];
    float ss[4], ts[4];
    for (int k = 0; k < 4; k++) {
        const float dx = sgx[k] * hx, dy = sgy[k] * hy;
        const int16_t px = (int16_t)std::clamp(c.ox + dx * R[0] + dy * U[0], -32768.f, 32767.f);
        const int16_t py = (int16_t)std::clamp(c.oy + dx * R[1] + dy * U[1], -32768.f, 32767.f);
        const int16_t pz = (int16_t)std::clamp(c.oz + dx * R[2] + dy * U[2], -32768.f, 32767.f);
        transform(px, py, pz, q[k]);
        q[k].c[0] = q[k].c[1] = q[k].c[2] = q[k].c[3] = 0xFF;
        float sf = (sgx[k] + 1.f) * 0.5f, tf = (sgy[k] + 1.f) * 0.5f;
        if (swap) std::swap(sf, tf);
        if (w0 & 2u) sf = 1.f - sf;
        if (w0 & 4u) tf = 1.f - tf;
        ss[k] = (sf * texS - 16.f) / 32.f;
        ts[k] = (tf * texT - 16.f) / 32.f;
    }
    const uint32_t saved_prim = prim_color, saved_l = other_l, saved_geom = geom;
    prim_color = w1;
    other_l = 0x00504A50u;   // depth test on, no depth write, alpha blend
    geom &= ~G_CULL_BACK;
    resolve_texture();
    static const int ord[6] = { 0, 1, 3, 0, 3, 2 };
    Vtx v[6];
    for (int i = 0; i < 6; i++) make_vtx(q[ord[i]], v[i], ss[ord[i]], ts[ord[i]], tex_on);
    emit_tris(v, 6, 0);
    prim_color = saved_prim;
    other_l = saved_l;
    geom = saved_geom;
}

// 0x05 05 02: flat terrain tile
void Renderer3DS::tile_quad(uint32_t pc) {
    const uint32_t w1 = rd32(pc + 4), w2 = rd32(pc + 8);
    const uint32_t col[4] = { rd32(pc + 12), rd32(pc + 16), rd32(pc + 20), rd32(pc + 24) };
    const uint32_t w7 = rd32(pc + 28), w8 = rd32(pc + 32), w9 = rd32(pc + 36);
    const int16_t h[4] = { (int16_t)(w1 >> 16), (int16_t)w1, (int16_t)(w2 >> 16), (int16_t)w2 };
    const int32_t x = (int16_t)(w8 >> 16), y = (int32_t)(int16_t)w8 << 4, z = (int16_t)(w9 >> 16), sz = (int16_t)w9;
    int32_t span = (int16_t)(w7 & 0xFFFF);
    {   // full-texture span (the RT64 profile's f5_terrain_uv_span)
        const Tile& t = tiles[tex_tile & 7];
        const int dim = (int)((t.lrs - t.uls) >> 2) + 1;
        if (!t.masks && !t.maskt && dim >= 2 && dim == (int)((t.lrt - t.ult) >> 2) + 1 && span == ((dim - 1) << 5)) span = dim << 5;
    }
    ensure_viewport();
    const int32_t px[4] = { x, x + sz, x, x + sz }, pz[4] = { z, z, z + sz, z + sz };
    const float us[4] = { 0, (float)span, 0, (float)span }, vt[4] = { (float)span, (float)span, 0, 0 };
    CacheVtx q[4];
    for (int k = 0; k < 4; k++) {
        const int32_t yy = std::clamp<int32_t>(y + h[k], -32768, 32767);
        transform((int16_t)std::clamp<int32_t>(px[k], -32768, 32767), (int16_t)yy, (int16_t)std::clamp<int32_t>(pz[k], -32768, 32767), q[k]);
        q[k].c[0] = (uint8_t)(col[k] >> 24); q[k].c[1] = (uint8_t)(col[k] >> 16); q[k].c[2] = (uint8_t)(col[k] >> 8); q[k].c[3] = (uint8_t)col[k];
    }
    if (tex_on) resolve_texture();
    static const int ord[6] = { 0, 3, 2, 0, 1, 3 };
    Vtx v[6];
    for (int i = 0; i < 6; i++) make_vtx(q[ord[i]], v[i], us[ord[i]] / 32.f, vt[ord[i]] / 32.f, tex_on);
    emit_tris(v, 6, 0);
}

// 0x05 05 00: heightfield tile. Heights/colors follow the ucode's seam and interior geomorph (as the RT64 profile's
// f5_tile_grid) and every tile becomes the same 5x5 lattice, so neighbouring tiles share edge vertices exactly.
void Renderer3DS::tile_grid(uint32_t pc) {
    const uint32_t r0w0 = rd32(pc), r0w1 = rd32(pc + 4), r1w0 = rd32(pc + 8), r1w1 = rd32(pc + 12);
    const uint32_t r2w1 = rd32(pc + 20), r3w0 = rd32(pc + 24);
    const uint32_t w7 = rd32(pc + 28), w8 = rd32(pc + 32), w9 = rd32(pc + 36);
    const uint32_t hptr = r1w0 & RAM_MASK, cptr = r1w1 & RAM_MASK;
    const int gridN = (int)((r0w0 >> 16) & 0xFF) > 1 ? (int)((r0w0 >> 16) & 0xFF) : 5;
    const int shift = (int)(r0w0 & 0xFF) & 7;
    const int M = ((gridN - 1) >> shift) + 1;
    if (M < 2 || M > 5) return;
    const int32_t x = (int16_t)(w8 >> 16), y = (int32_t)(int16_t)w8 << 4, z = (int16_t)(w9 >> 16), sz = (int16_t)w9;
    const int16_t s = (int16_t)(w7 & 0xFFFF);
    int32_t total = (int32_t)s * (M - 1);
    {
        const Tile& t = tiles[tex_tile & 7];
        const int dim = (int)((t.lrs - t.uls) >> 2) + 1;
        if (!t.masks && !t.maskt && dim >= 2 && dim == (int)((t.lrt - t.ult) >> 2) + 1 && total == ((dim - 1) << 5)) total = dim << 5;
    }
    const float step5 = (float)((M - 1) * sz) / 4.0f;
    const float uvcell = (float)total / 4.0f / 32.f;

    auto clamp16 = [](int64_t v) -> int32_t { return (int32_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); };
    auto mean2 = [](int32_t a, int32_t b) -> int32_t { return (a + b) >> 1; };
    auto morph = [&](int64_t acc, int32_t h, uint32_t w) -> int32_t {
        const int64_t hi = clamp16(acc >> 16), lo = acc & 0xFFFF;
        int64_t a2 = ((lo * (int64_t)w) >> 16) + hi * (int64_t)w;
        a2 += (int64_t)h * (int64_t)((0x10000u - w) & 0xFFFFu);
        return clamp16(a2 >> 16);
    };

    // channel 0 = height (s8 << 4), 1..4 = RGBA
    int32_t P[5][5][5] = {};
    const int stride = 1 << shift;
    for (int j = 0; j < M; ++j)
        for (int i = 0; i < M; ++i)
            P[j][i][0] = (int32_t)(int8_t)rd8(hptr + (uint32_t)((j * stride) * gridN + i * stride)) << 4;
    {
        const int L = (int)((r0w0 >> 12) & 0xF);
        const int d = L - shift;
        const int cstep = d >= 0 ? (0x100 >> d) : (0x100 << -d);
        const int cmask = d >= 0 ? (1 << d) - 1 : 0;
        const int Mc = ((gridN - 1) >> L) + 1;
        auto src = [&](int idx, int ch) -> int32_t { return (int32_t)rd8(cptr + (uint32_t)idx * 4u + (uint32_t)ch); };
        for (int j = 0; j < M; ++j)
            for (int i = 0; i < M; ++i) {
                const int idx = ((j * cstep) >> 8) * Mc + ((i * cstep) >> 8);
                const bool mi = (i & cmask) != 0, mj = (j & cmask) != 0;
                for (int ch = 0; ch < 4; ++ch)
                    P[j][i][1 + ch] = (mi || mj) ? mean2(src(idx, ch), src(idx + (mi ? 1 : 0) + (mj ? Mc : 0), ch)) : src(idx, ch);
            }
    }

    // edge seams: the coarser tile goes down "equal", the finer down "differs" with the coarser tile's weight
    const int nbXmin = (int)((r0w1 >> 24) & 0xFF), nbXmax = (int)((r0w1 >> 16) & 0xFF);
    const int nbZmin = (int)((r0w1 >> 8) & 0xFF), nbZmax = (int)(r0w1 & 0xFF);
    const uint32_t wXmin = r2w1 & 0xFFFFu, wXmax = (r3w0 >> 16) & 0xFFFFu;
    const uint32_t wZmin = r3w0 & 0xFFFFu, wZmax = (w7 >> 16) & 0xFFFFu;
    const uint32_t wInterior = (r2w1 >> 16) & 0xFFFFu;
    bool differs[4] = {};
    auto seam = [&](int e, int nb, uint32_t w) {
        auto E = [&](int k, int ch) -> int32_t& {
            return e == 0 ? P[0][k][ch] : e == 1 ? P[M - 1][k][ch] : e == 2 ? P[k][0][ch] : P[k][M - 1][ch];
        };
        for (int ch = 0; ch < 5; ++ch) {
            if (nb != shift) {
                for (int k = 1; k < M - 1; k += 2) E(k, ch) = mean2(E(k - 1, ch), E(k + 1, ch));
                if (w != 0 && M >= 5) {
                    for (int k = 1; k < M - 1; ++k) {
                        if ((k & 3) == 0) continue;
                        const int64_t f = (int64_t)(k & 3) << 14;
                        const int k0 = k & ~3;
                        E(k, ch) = morph((int64_t)E(k0, ch) * (0x10000 - f) + (int64_t)E(k0 + 4, ch) * f, E(k, ch), w);
                    }
                }
            } else if (w != 0) {
                for (int k = 1; k < M - 1; k += 2)
                    E(k, ch) = morph((int64_t)E(k - 1, ch) * 0x8000 + (int64_t)E(k + 1, ch) * 0x8000, E(k, ch), w);
            }
        }
        differs[e] = nb != shift;
    };
    if (M >= 3) {
        seam(0, nbZmin, wZmin);
        seam(1, nbZmax, wZmax);
        seam(2, nbXmin, wXmin);
        seam(3, nbXmax, wXmax);
    }
    if (wInterior != 0 && M >= 3) {
        const int64_t half = wInterior >> 1, inv = (0x10000u - wInterior) & 0xFFFFu;
        for (int j = 1; j <= M - 2; ++j)
            for (int i = 1; i <= M - 2; ++i) {
                if (!(i & 1) && !(j & 1)) continue;
                const int dj = j & 1, di = i & 1;
                for (int ch = 0; ch < 5; ++ch) {
                    const int64_t a = P[j - dj][i - di][ch], b = P[j + dj][i + di][ch];
                    P[j][i][ch] = clamp16(((a + b) * half + (int64_t)P[j][i][ch] * inv) >> 16);
                }
            }
    }

    // 5x5 lattice: smaller pools fill the in-between points with midpoints; edges next to a coarser tile sit on its segments
    float Lt[5][5][5];
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 5; ++c)
            for (int ch = 0; ch < 5; ++ch) {
                if (M >= 5) { Lt[r][c][ch] = (float)P[r][c][ch]; continue; }
                if (M == 2) {
                    const float fr = r / 4.f, fc = c / 4.f;
                    Lt[r][c][ch] = (P[0][0][ch] * (1 - fc) + P[0][1][ch] * fc) * (1 - fr) + (P[1][0][ch] * (1 - fc) + P[1][1][ch] * fc) * fr;
                    continue;
                }
                const int r0 = r >> 1, c0 = c >> 1;
                const int r1 = (r & 1) ? r0 + 1 : r0, c1 = (c & 1) ? c0 + 1 : c0;
                Lt[r][c][ch] = 0.5f * ((float)P[r0][c0][ch] + (float)P[r1][c1][ch]);
            }
    for (int e = 0; e < 4; ++e) {
        if (!differs[e]) continue;
        for (int k = 1; k < 4; k += 2)
            for (int ch = 0; ch < 5; ++ch) {
                float& p = e == 0 ? Lt[0][k][ch] : e == 1 ? Lt[4][k][ch] : e == 2 ? Lt[k][0][ch] : Lt[k][4][ch];
                const float a = e == 0 ? Lt[0][k - 1][ch] : e == 1 ? Lt[4][k - 1][ch] : e == 2 ? Lt[k - 1][0][ch] : Lt[k - 1][4][ch];
                const float b = e == 0 ? Lt[0][k + 1][ch] : e == 1 ? Lt[4][k + 1][ch] : e == 2 ? Lt[k + 1][0][ch] : Lt[k + 1][4][ch];
                p = 0.5f * (a + b);
            }
    }

    ensure_viewport();
    CacheVtx g[5][5];
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 5; ++c) {
            const int32_t px = x + (int32_t)(c * step5), pz = z + (int32_t)(r * step5), py = y + (int32_t)Lt[r][c][0];
            transform((int16_t)std::clamp<int32_t>(px, -32768, 32767), (int16_t)std::clamp<int32_t>(py, -32768, 32767),
                      (int16_t)std::clamp<int32_t>(pz, -32768, 32767), g[r][c]);
            for (int ch = 0; ch < 4; ch++) {
                const float val = Lt[r][c][1 + ch] + 0.5f;
                g[r][c].c[ch] = (uint8_t)(val > 255.f ? 255.f : (val < 0.f ? 0.f : val));
            }
        }
    if (tex_on) resolve_texture();
    std::vector<Vtx>& v = scratch;
    v.clear();
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            const int a[6][2] = { { j, i }, { j + 1, i + 1 }, { j + 1, i }, { j, i }, { j, i + 1 }, { j + 1, i + 1 } };
            for (int k = 0; k < 6; k++) {
                Vtx o;
                const int r = a[k][0], c = a[k][1];
                make_vtx(g[r][c], o, uvcell * c, uvcell * (4 - r), tex_on);
                v.push_back(o);
            }
        }
    emit_tris(v.data(), (int)v.size(), 0);
}

void Renderer3DS::terrain(uint32_t pc) {
    const uint32_t w0 = rd32(pc);
    if (((w0 >> 8) & 0x2u) != 0) tile_quad(pc);
    else tile_grid(pc);
}

// ===================================================================== walk
void Renderer3DS::enter_chunk(uint32_t& pc, uint32_t target) {
    ++hops;
    if (depth < MAX_DEPTH) { chunk_base[depth] = target; chain_len[depth] = 0; }
    pc = target + 8;
}

bool Renderer3DS::next_chunk(uint32_t& pc) {
    const uint32_t base = depth < MAX_DEPTH ? chunk_base[depth] : 0;
    const uint32_t next = base ? rd32(base) : 0;
    const uint32_t target = next & 0x00FFFFFFu;
    const bool fwd = (next >> 24) == 0x80u && target != 0 && target + 0x108u <= RAM_LIMIT;
    const bool linked = fwd && rd32(target + 4) == (0x80000000u | base);
    bool revisit = false;
    if (linked && depth < MAX_DEPTH) {
        uint32_t& n = chain_len[depth];
        for (uint32_t i = 0; i < n; i++) if (chain[depth][i] == target) { revisit = true; break; }
        if (n < 64) chain[depth][n++] = target;
        else revisit = true;
    }
    if (!linked || revisit || target == base || ++hops > 4096u) return false;
    const uint32_t keep = depth < MAX_DEPTH ? chain_len[depth] : 0;
    enter_chunk(pc, target);
    if (depth < MAX_DEPTH) chain_len[depth] = keep;
    return true;
}

void Renderer3DS::run_task(uint32_t data_ptr) {
    uint32_t pc = data_ptr & RAM_MASK;
    depth = 0;
    hops = entries = faces = 0;
    std::memset(chunk_base, 0, sizeof chunk_base);
    std::memset(chain_len, 0, sizeof chain_len);
    chunk_base[0] = pc;
    vp_from_stream = false;
    bool running = true;

    auto end_list = [&]() {   // B8: pop, or end the task
        if (depth > 0) { depth--; pc = ret_stack[depth]; }
        else running = false;
    };

    while (running) {
        if (++entries > 200000u) break;
        if (pc + 8 > RAM_LIMIT) break;
        if (depth < MAX_DEPTH && chunk_base[depth] && pc >= chunk_base[depth] + 0x108u) {
            if (!next_chunk(pc)) { end_list(); continue; }
        }
        const uint32_t w0 = rd32(pc), w1 = rd32(pc + 4);
        const uint32_t op = w0 >> 24;
        uint32_t adv = 8;
        switch (op) {
        case 0x00: case 0x80: case 0xB0: case 0xB2: case 0xAF: case 0xEF: case 0xC0: case 0x60:
        case 0xE6: case 0xE7: case 0xE8: case 0xE9: case 0xB1: case 0xB3: case 0xEA: case 0xEB: case 0xEC:
            break;
        case 0x01: {   // matrix
            const uint32_t addr = seg_addr(w1);
            if (!in_ram(addr, 64)) break;
            if ((w0 >> 16) & 1) {
                load_matrix(addr, proj);
                radar_pass = false;
            }
            else load_matrix(addr, mv);
            mvp_dirty = true;
            if (!((w0 >> 16) & 1)) radar_matrix_loaded();
            if (!((w0 >> 16) & 1)) dump_mtx_addr = addr;
            if (dump_tasks > 0) {
                const float* m = ((w0 >> 16) & 1) ? proj : mv;
                std::fprintf(stderr, "[dump] mtx %s @%06X w0=%08X row0=(%.3f %.3f %.3f %.3f) row3=(%.1f %.1f %.1f %.3f)\n", ((w0 >> 16) & 1) ? "proj" : "mv",
                    addr, w0, m[0], m[1], m[2], m[3], m[12], m[13], m[14], m[15]);
            }
            break;
        }
        case 0x02:
            color_buf = w1;
            color_cnt = (w0 & 0xFFFu) + 1u;
            break;
        case 0x03: {
            const uint32_t sub = (w0 >> 16) & 0xFF;
            const uint32_t a = rd32(pc + 8), b = rd32(pc + 12), c = rd32(pc + 16), d = rd32(pc + 20);
            if (sub == 0x80) {
                vp.sx = (float)(int16_t)(a >> 16) / 4.f; vp.sy = (float)(int16_t)(a & 0xFFFF) / 4.f;
                vp.tx = (float)(int16_t)(c >> 16) / 4.f; vp.ty = (float)(int16_t)(c & 0xFFFF) / 4.f;
                (void)b; (void)d;
                vp_from_stream = true;
            } else if (sub == 0x88) {
                fog_m = (double)(int32_t)a / 65536.0;
                fog_o = (double)(int32_t)b / 65536.0;
            } else if (sub == 0x82) {
                tc_scale[0] = (int32_t)((a & 0xFFFF0000u) | (c >> 16));
                tc_scale[1] = (int32_t)((a << 16) | (c & 0xFFFFu));
            }
            adv = 24;
            break;
        }
        case 0x04: case 0x14: {
            if (op == 0x14) { adv = 16; break; }   // 16-byte state command
            const uint32_t n = (w0 >> 10) & 0x3F;
            const uint32_t src = seg_addr(w1);
            if (n == 0 || !in_ram(src, n * 8)) break;
            const uint64_t tv0 = svcGetSystemTick();
            ensure_viewport();
            const bool have_colors = color_buf >= 0x80000000u && in_ram(color_buf & RAM_MASK, n * 4);
            for (uint32_t i = 0; i < n && i < VCACHE; i++) {
                const int16_t x = (int16_t)rd16(src + i * 8), y = (int16_t)rd16(src + i * 8 + 2), z = (int16_t)rd16(src + i * 8 + 4);
                transform(x, y, z, vcache[i]);
                const uint32_t c = have_colors ? rd32((color_buf & RAM_MASK) + i * 4) : 0xFFFFFFFFu;
                vcache[i].c[0] = (uint8_t)(c >> 24); vcache[i].c[1] = (uint8_t)(c >> 16); vcache[i].c[2] = (uint8_t)(c >> 8); vcache[i].c[3] = (uint8_t)c;
            }
            vcache_n = std::min<uint32_t>(n, VCACHE);
            prof_vtx += svcGetSystemTick() - tv0;
            if (dump_tasks > 0) {
                const CacheVtx& c = vcache[0];
                std::fprintf(stderr, "[dump] vtx n=%u src=%06X v0 obj=(%d %d %d) clip=(%.1f %.1f %.1f %.1f) col=%02X%02X%02X%02X fog=%.2f vp=(%.0f %.0f %.0f %.0f)\n",
                    n, src, c.ox, c.oy, c.oz, c.cx, c.cy, c.cz, c.cw, c.c[0], c.c[1], c.c[2], c.c[3], c.fog, vp.sx, vp.sy, vp.tx, vp.ty);
            }
            break;
        }
        case 0x05:
            if (((w0 >> 16) & 0xFFu) == 0x05u) {
                const uint64_t tt0 = svcGetSystemTick(), tf0 = prof_flush, te0 = prof_emit;
                terrain(pc);
                adv = 40;
                prof_terrain += svcGetSystemTick() - tt0 - (prof_flush - tf0) - (prof_emit - te0);
            }
            break;
        case 0x06: {
            if ((w0 & 0x00FEFFFFu) != 0) break;   // F5 reuses the byte with data: not a call
            const uint32_t target = seg_addr(w1);
            if (target == 0 || target + 0x108u > RAM_LIMIT) break;
            if (((w0 >> 16) & 1) == 0) {
                if (depth + 1 >= MAX_DEPTH) break;
                ret_stack[depth] = pc + 8;
                depth++;
            }
            enter_chunk(pc, target);
            continue;
        }
        case 0x07: {
            const uint32_t target = seg_addr(w1);
            if (target == 0 || target + 0x108u > RAM_LIMIT) { end_list(); continue; }
            enter_chunk(pc, target);
            continue;
        }
        case 0xB5: case 0x12:
            if (!next_chunk(pc)) end_list();
            continue;
        case 0xB8: case 0x0F:
            end_list();
            continue;
        case 0xBF: case 0x08: {
            const uint32_t idx[3] = { ((w1 >> 16) & 0xFF) / 5u, ((w1 >> 8) & 0xFF) / 5u, (w1 & 0xFF) / 5u };
            if (w0 & 2) {
                const uint32_t st[3] = { rd32(pc + 16), rd32(pc + 20), rd32(pc + 24) };
                face(idx, 3, st, rd32(pc + 8));
                adv = 32;
            } else {
                face(idx, 3, nullptr, rd32(pc + 8));
                adv = 16;
            }
            break;
        }
        case 0xB4: case 0x13: {
            const uint32_t idx[4] = { ((w1 >> 16) & 0xFF) / 5u, ((w1 >> 8) & 0xFF) / 5u, (w1 & 0xFF) / 5u, (w1 >> 24) / 5u };
            if (w0 & 2) {
                const uint32_t st[4] = { rd32(pc + 16), rd32(pc + 20), rd32(pc + 24), rd32(pc + 28) };
                face(idx, 4, st, rd32(pc + 8));
                adv = 32;
            } else {
                face(idx, 4, nullptr, rd32(pc + 8));
                adv = 16;
            }
            break;
        }
        case 0xBD:
            sprite(pc);
            adv = 24;
            break;
        case 0xBE: case 0x09: case 0x0A:
            adv = 16;
            break;
        case 0xBB: case 0x0C: {   // texture: tile + on (the scale is unused by the F5 ucode)
            const uint32_t tile = (w0 >> 8) & 7;
            const bool on = (w0 & 0xFF) != 0;
            if (tile != tex_tile) { tex_tile = tile; tex_dirty = true; }
            tex_on = on;
            break;
        }
        case 0xBC: case 0x0B: {   // moveword
            const uint32_t idx = w0 & 0xFF;
            if (idx == 8) fog_m = (double)(int32_t)w1 / 65536.0;
            else if (idx == 0x0A) fog_o = (double)(int32_t)w1 / 65536.0;
            else if (idx == 6) segments[((w0 >> 8) & 0xFFFF) / 4 & 0xF] = w1 & RAM_MASK;
            break;
        }
        case 0xB6: case 0x11: geom &= ~w1; break;
        case 0xB7: case 0x10: geom |= w1; break;
        case 0xB9: case 0x0E: {
            const uint32_t sh = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
            const uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << sh;
            other_l = (other_l & ~mask) | (w1 & mask);
            break;
        }
        case 0xBA: case 0x0D: {
            const uint32_t sh = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
            const uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << sh;
            const uint32_t old = other_h;
            other_h = (other_h & ~mask) | (w1 & mask);
            if ((old ^ other_h) & 0xC000) tex_dirty = true;   // TLUT type
            break;
        }
        // ---------------------------------------------------------- RDP
        case 0xE4: case 0xE5:
            tex_rect(w0, w1, rd32(pc + 8), rd32(pc + 12), op == 0xE5);
            adv = 16;
            break;
        case 0xED:
            scissor[0] = (int)((w0 >> 12) & 0xFFF); scissor[1] = (int)(w0 & 0xFFF);
            scissor[2] = (int)((w1 >> 12) & 0xFFF); scissor[3] = (int)(w1 & 0xFFF);
            break;
        case 0xEE: prim_depth = (uint16_t)(w1 >> 16); break;
        case 0xF0: { const uint64_t t0 = svcGetSystemTick(); record_load(0xF0, w0, w1); prof_load += svcGetSystemTick() - t0; break; }
        case 0xF2: {
            Tile& t = tiles[(w1 >> 24) & 7];
            t.uls = (w0 >> 12) & 0xFFF; t.ult = w0 & 0xFFF; t.lrs = (w1 >> 12) & 0xFFF; t.lrt = w1 & 0xFFF;
            tex_dirty = true;
            break;
        }
        case 0xF3: { const uint64_t t0 = svcGetSystemTick(); record_load(0xF3, w0, w1); prof_load += svcGetSystemTick() - t0; break; }
        case 0xF4: { const uint64_t t0 = svcGetSystemTick(); record_load(0xF4, w0, w1); prof_load += svcGetSystemTick() - t0; break; }
        case 0xF5: {
            Tile& t = tiles[(w1 >> 24) & 7];
            t.fmt = (w0 >> 21) & 7; t.siz = (w0 >> 19) & 3; t.line = (w0 >> 9) & 0x1FF; t.tmem = w0 & 0x1FF;
            t.palette = (w1 >> 20) & 0xF; t.cmt = (w1 >> 18) & 3; t.maskt = (w1 >> 14) & 0xF; t.shiftt = (w1 >> 10) & 0xF;
            t.cms = (w1 >> 8) & 3; t.masks = (w1 >> 4) & 0xF; t.shifts = w1 & 0xF;
            tex_dirty = true;
            break;
        }
        case 0xF6: fill_rect(w0, w1); break;
        case 0xF7: fill_color = w1; break;
        case 0xF8: fog_color = w1; break;
        case 0xF9: blend_color = w1; break;
        case 0xFA: prim_color = w1; break;
        case 0xFB: env_color = w1; break;
        case 0xFC:
            if ((w0 & 0x00FFFFFFu) != combine_hi || w1 != combine_lo) tex_dirty = true;
            combine_hi = w0 & 0x00FFFFFFu; combine_lo = w1;
            break;
        case 0xFD:
            timg.addr = seg_addr(w1); timg.fmt = (w0 >> 21) & 7; timg.siz = (w0 >> 19) & 3; timg.width = (w0 & 0xFFF) + 1;
            break;
        case 0xFE: zimg = seg_addr(w1); break;
        case 0xFF: {
            const uint32_t fmt = (w0 >> 21) & 7, siz = (w0 >> 19) & 3, width = (w0 & 0xFFF) + 1;
            const uint32_t addr = seg_addr(w1);
            // F5 emits SETCIMG with bogus payloads now and then: keep only plausible colour images
            if (w1 != 0 && fmt == 0 && (siz == 2 || siz == 3) && width >= 16 && width <= 1024 && addr + width * 2 <= RAM_LIMIT) {
                cimg.addr = addr; cimg.fmt = fmt; cimg.siz = siz; cimg.width = width;
                if (addr != (zimg & RAM_MASK)) {
                    const int nw = (int)std::min<uint32_t>(width, 640);
                    if (nw != canvas_w) {
                        flush();
                        canvas_w = nw;
                        canvas_h = nw > 400 ? 480 : 240;
                        if (in_frame) C3D_SetViewport(0, 0, (u32)canvas_w, (u32)canvas_h);
                        gpu_state_valid = false;
                    }
                }
            }
            break;
        }
        default:
            break;
        }
        pc += adv;
    }
    flush();
}

// Copies the pages the last two tasks read (the display list pool, matrices, vertex and texture data) and lets the game
// build its next frame while this one is drawn from the copies. Pages read for the first time are read live.
void Renderer3DS::snapshot_and_release() {
    std::memset(touched, 0, sizeof touched);
    if (!pipeline || dump_tasks > 0) return;
    const uint64_t t0 = svcGetSystemTick();
    shadow_n = 0;
    // the pages the last task read first, then those read in the 7 before it (the game rotates its per-frame buffers)
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t p = 0; p < (RAM_LIMIT >> 12) && shadow_n < SHADOW_PAGES; p++) {
            const uint8_t age = page_age[p];
            if (pass == 0 ? age != 0 : (age == 0 || age >= 8)) continue;
            uint8_t* dst = shadow_pool + (size_t)shadow_n * 4096;
            std::memcpy(dst, ram + (size_t)p * 4096, 4096);
            rpage[p] = dst;
            shadow_list[shadow_n++] = (uint16_t)p;
        }
    }
    stat_snap_pages += (uint32_t)shadow_n;
    prof_snap += svcGetSystemTick() - t0;
    rs3ds_gfx_release();
}

void Renderer3DS::end_snapshot() {
    for (int i = 0; i < shadow_n; i++) rpage[shadow_list[i]] = ram + (size_t)shadow_list[i] * 4096;
    shadow_n = 0;
    for (uint32_t p = 0; p < (RAM_LIMIT >> 12); p++) {
        if (touched[p]) page_age[p] = 0;
        else if (page_age[p] < 255) page_age[p]++;
    }
}

void Renderer3DS::send_dl(const OSTask* task) {
    if (!ok || !task) return;
    if (g_rs3ds_suspended) return;   // HOME menu / sleep: the GPU belongs to the system
    if (g_rs3ds_dump_request && dump_tasks == 0) {
        g_rs3ds_dump_request = 0;
        dump_tasks = 4;
        dump_batches = dump_dropped_idx = 0;
        std::fprintf(stderr, "[dump] ---- frame dump start (frame %u) ----\n", frame);
    }
    if (dump_tasks > 0) std::fprintf(stderr, "[dump] task dl=%08X\n", (unsigned)task->t.data_ptr);
    stat_tasks++;
    radar_pass = false;
    const uint64_t t0 = svcGetSystemTick();
    snapshot_and_release();
    run_task((uint32_t)task->t.data_ptr);
    end_snapshot();
    stat_ticks += svcGetSystemTick() - t0;
    if (dump_tasks > 0 && --dump_tasks == 0) {
        std::fprintf(stderr, "[dump] ---- end: %u draws logged, %u faces dropped for out-of-range vertex indices ----\n", dump_batches, dump_dropped_idx);
        std::fflush(stderr);
    }
    // every ~2 s: presented fps, renderer time per task, triangles and draws per frame
    if (!stat_t0) stat_t0 = t0;
    const uint64_t span = svcGetSystemTick() - stat_t0;
    if (span > 2ull * SYSCLOCK_ARM11) {
        const double secs = (double)span / SYSCLOCK_ARM11;
        const uint32_t frames = frame - stat_frame0, tasks = stat_tasks - stat_tasks0;
        auto ms = [&](uint64_t t) { return tasks ? (double)t / tasks * 1000.0 / SYSCLOCK_ARM11 : 0.0; };
        std::fprintf(stderr, "[r3ds]   per task: tmem loads %.1f ms, texture lookup/decode %.1f ms, GPU state+draws %.1f ms, GPU wait %.1f ms, vertices %.1f ms, terrain %.1f ms, triangle setup %.1f ms, snapshot %.1f ms (%u pages)\n",
            ms(prof_load), ms(prof_resolve), ms(prof_flush), ms(prof_wait), ms(prof_vtx), ms(prof_terrain), ms(prof_emit), ms(prof_snap), tasks ? stat_snap_pages / tasks : 0);
        prof_load = prof_resolve = prof_flush = prof_wait = prof_vtx = prof_terrain = prof_emit = prof_snap = 0;
        stat_snap_pages = 0;
        std::fprintf(stderr, "[r3ds] %.1f fps, %u tasks, render %.1f ms/task, %u tris/frame, %u draws/frame, %u textures (%u KB)\n",
            frames / secs, tasks, tasks ? (double)stat_ticks / tasks * 1000.0 / SYSCLOCK_ARM11 : 0.0,
            frames ? (stat_tris - stat_tris0) / frames : 0, frames ? (stat_draws - stat_draws0) / frames : 0,
            (unsigned)tex_cache.size(), (unsigned)(tex_bytes >> 10));
        std::fprintf(stderr, "[r3ds] radar: hook %u, matrix matches %u, fallback %u, t=(%.1f %.1f %.1f) s=(%.0f %.0f) crop=(%.0f %.0f h%.0f) closest mv=(%.1f %.1f %.1f)\n",
            stat_radar_hook, stat_radar_match, stat_radar_fallback, g_rs3ds_radar_t[0], g_rs3ds_radar_t[1], g_rs3ds_radar_t[2],
            g_rs3ds_radar_s[0], g_rs3ds_radar_s[1], crop_cx, crop_cy, crop_h, last_mv_t[0], last_mv_t[1], last_mv_t[2]);
        stat_radar_hook = stat_radar_match = stat_radar_fallback = 0;
        closest_mv_d = 1e30f;
        stat_t0 = svcGetSystemTick();
        stat_frame0 = frame; stat_tasks0 = stat_tasks; stat_tris0 = stat_tris; stat_draws0 = stat_draws;
        stat_ticks = 0;
    }
}

} // namespace

std::unique_ptr<ultramodern::renderer::RendererContext> rs3ds::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
    return std::make_unique<Renderer3DS>(rdram);
}
