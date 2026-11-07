// The few SDL2 audio/timer calls src/audio/audio.cpp makes, on the 3DS DSP (port3ds/sdl_shim_3ds.cpp).
#pragma once
#include <cstdint>

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef Uint32 SDL_AudioDeviceID;
typedef Uint16 SDL_AudioFormat;

#define AUDIO_S16SYS 0x8010
#define SDL_TICKS_PASSED(A, B) ((Sint32)((B) - (A)) <= 0)
#define SDL_APP_WILLENTERBACKGROUND 0x103
#define SDL_APP_DIDENTERFOREGROUND 0x106

struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    void* callback;
    void* userdata;
};

SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int allowed_changes);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
int SDL_QueueAudio(SDL_AudioDeviceID dev, const void* data, Uint32 len);
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev);
void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev);
const char* SDL_GetError(void);
Uint32 SDL_GetTicks(void);
