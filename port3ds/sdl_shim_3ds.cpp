// SDL2 audio queue + ticks on the 3DS (the calls src/audio/audio.cpp makes).
//
// Queued PCM (16-bit stereo) goes into a ring; NDSP plays fixed wave buffers
// that its callback refills from the ring (silence on underrun), as the MvC2
// port's NDSP backend. Needs sdmc:/3ds/dspfirm.cdc; without it the queue still
// drains (at the nominal rate) so the game's audio pacing keeps working.

#include <3ds.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "SDL2/SDL.h"

namespace {

constexpr int CHANNEL = 0;
constexpr int NBUF = 8;
constexpr int BUF_FRAMES = 1024;
constexpr uint32_t MIN_CHUNK = 256;
constexpr uint32_t RING_FRAMES = 1u << 15;   // ~1 s at 32 kHz

struct {
    bool ndsp = false, open = false, paused = true;
    uint32_t freq = 22050;
    int16_t* ring = nullptr;                 // stereo frames
    uint32_t rd = 0, wr = 0;                 // frame indices (mod RING_FRAMES)
    LightLock lock;
    ndspWaveBuf wb[NBUF];
    int16_t* wbdata = nullptr;
    uint64_t start_ms = 0;
    uint64_t fake_last_ms = 0;               // no DSP: drain at the nominal rate
} A;

uint32_t ring_count() { return (A.wr - A.rd) & (RING_FRAMES - 1); }

// Frames handed to the DSP and not yet played.
uint32_t dsp_pending() {
    uint32_t n = 0;
    for (int i = 0; i < NBUF; i++) {
        const uint8_t st = A.wb[i].status;
        if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) n += A.wb[i].nsamples;
    }
    const uint32_t pos = A.ndsp ? ndspChnGetSamplePos(CHANNEL) : 0;
    return n > pos ? n - pos : 0;
}

// Hands the DSP exactly what is queued (never padded with silence: a padded buffer is an audible gap).
bool fill(ndspWaveBuf* b, uint32_t min_frames) {
    int16_t* d = (int16_t*)b->data_pcm16;
    LightLock_Lock(&A.lock);
    uint32_t n = ring_count();
    if (n < min_frames || n == 0) {
        LightLock_Unlock(&A.lock);
        return false;
    }
    if (n > BUF_FRAMES) n = BUF_FRAMES;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t k = (A.rd + i) & (RING_FRAMES - 1);
        d[i * 2] = A.ring[k * 2];
        d[i * 2 + 1] = A.ring[k * 2 + 1];
    }
    A.rd = (A.rd + n) & (RING_FRAMES - 1);
    LightLock_Unlock(&A.lock);
    DSP_FlushDataCache(d, n * 4);
    b->nsamples = n;
    ndspChnWaveBufAdd(CHANNEL, b);
    return true;
}

void dsp_callback(void*) {
    if (!A.open || A.paused) return;
    for (int i = 0; i < NBUF; i++) {
        ndspWaveBuf* b = &A.wb[i];
        if (b->status != NDSP_WBUF_DONE && b->status != NDSP_WBUF_FREE) continue;
        // wait for a decent chunk while the DSP still has audio; take anything once it is about to run dry
        const uint32_t min = dsp_pending() > MIN_CHUNK ? MIN_CHUNK : 1;
        if (!fill(b, min)) break;
    }
}

} // namespace

SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec* desired, SDL_AudioSpec*, int) {
    if (!A.ring) {
        LightLock_Init(&A.lock);
        A.ring = (int16_t*)std::malloc(RING_FRAMES * 4);
        A.wbdata = (int16_t*)linearAlloc(NBUF * BUF_FRAMES * 4);
        if (!A.ring || !A.wbdata) return 0;
        A.ndsp = R_SUCCEEDED(ndspInit());
        std::printf("[audio] NDSP %s\n", A.ndsp ? "ready" : "unavailable (no sdmc:/3ds/dspfirm.cdc?): no sound");
        if (A.ndsp) ndspSetCallback(dsp_callback, nullptr);
    }
    A.freq = desired && desired->freq > 0 ? (uint32_t)desired->freq : 22050;
    LightLock_Lock(&A.lock);
    A.rd = A.wr = 0;
    LightLock_Unlock(&A.lock);
    if (A.ndsp) {
        ndspChnReset(CHANNEL);
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
        ndspChnSetRate(CHANNEL, (float)A.freq);
        ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
        float mix[12] = { 1.f, 1.f };
        ndspChnSetMix(CHANNEL, mix);
        std::memset(A.wb, 0, sizeof A.wb);
        for (int i = 0; i < NBUF; i++) {
            A.wb[i].data_pcm16 = A.wbdata + i * BUF_FRAMES * 2;
            A.wb[i].nsamples = BUF_FRAMES;
        }
    }
    A.open = true;
    A.paused = true;
    A.fake_last_ms = osGetTime();
    return 1;
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID) {
    A.open = false;
    if (A.ndsp) ndspChnWaveBufClear(CHANNEL);
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID, int pause_on) {
    A.paused = pause_on != 0;
    if (!A.paused && A.ndsp) {
        for (int i = 0; i < NBUF; i++) if (!fill(&A.wb[i], 1)) break;
    }
}

int SDL_QueueAudio(SDL_AudioDeviceID, const void* data, Uint32 len) {
    const int16_t* s = (const int16_t*)data;
    const uint32_t frames = len / 4;
    LightLock_Lock(&A.lock);
    for (uint32_t i = 0; i < frames; i++) {
        if (ring_count() >= RING_FRAMES - 1) break;   // full: drop the rest
        A.ring[A.wr * 2] = s[i * 2];
        A.ring[A.wr * 2 + 1] = s[i * 2 + 1];
        A.wr = (A.wr + 1) & (RING_FRAMES - 1);
    }
    LightLock_Unlock(&A.lock);
    return 0;
}

Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID) {
    if (!A.ndsp) {
        // no DSP: consume at the nominal rate so the game's pacing sees a draining queue
        const uint64_t now = osGetTime();
        const uint32_t played = (uint32_t)((now - A.fake_last_ms) * A.freq / 1000);
        if (played) {
            A.fake_last_ms = now;
            LightLock_Lock(&A.lock);
            const uint32_t n = ring_count() < played ? ring_count() : played;
            A.rd = (A.rd + n) & (RING_FRAMES - 1);
            LightLock_Unlock(&A.lock);
        }
    }
    LightLock_Lock(&A.lock);
    const uint32_t n = ring_count();
    LightLock_Unlock(&A.lock);
    // include what the DSP still holds, so the game's queue target is real buffered audio
    return (n + (A.ndsp ? dsp_pending() : 0)) * 4;
}

void SDL_ClearQueuedAudio(SDL_AudioDeviceID) {
    LightLock_Lock(&A.lock);
    A.rd = A.wr;
    LightLock_Unlock(&A.lock);
}

const char* SDL_GetError(void) { return "3DS audio"; }

Uint32 SDL_GetTicks(void) {
    if (!A.start_ms) A.start_ms = osGetTime();
    return (Uint32)(osGetTime() - A.start_ms);
}

// Drops everything queued (ring and DSP buffers): called on return from the HOME menu or sleep.
void rs3ds_audio_resync() {
    LightLock_Lock(&A.lock);
    A.rd = A.wr;
    LightLock_Unlock(&A.lock);
    if (A.ndsp && A.open) ndspChnWaveBufClear(CHANNEL);
    A.fake_last_ms = osGetTime();
}
