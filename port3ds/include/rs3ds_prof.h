// The 3DS profiler (port3ds/prof_3ds.cpp): which game function is running on core 0.
// The profiling build wraps every recompiled function with RS3DS_PROF_ENTER / RS3DS_PROF_LEAVE
// (port3ds/build.py, instrument_profile), keeping a shadow call stack per N64 thread; a sampler
// thread reads the running thread's top entry every millisecond.
#pragma once
#include <stdint.h>

typedef struct rs3ds_prof_stack {
    volatile uint32_t n;
    uint32_t a[64];
} rs3ds_prof_stack;

#ifdef __cplusplus
extern "C" {
#endif
extern rs3ds_prof_stack* volatile g_rs3ds_prof_cur;
void rs3ds_prof_resume(uint32_t thread, int thread_id);   // an N64 thread (OSThread address, id) starts running
void rs3ds_prof_block(uint32_t thread);                   // that N64 thread blocks (no-op if another one already runs)
void rs3ds_prof_synth_wait(int waiting);                  // the running thread waits for the audio synth (1) / is done (0)
void rs3ds_prof_start(void);             // start the sampler
void rs3ds_prof_lookup(void);            // one get_function call (indirect call)
#ifdef __cplusplus
}
#endif

#define RS3DS_PROF_ENTER(v) do { rs3ds_prof_stack* s_ = g_rs3ds_prof_cur; s_->a[s_->n & 63] = (v); s_->n = s_->n + 1; } while (0)
#define RS3DS_PROF_LEAVE() do { rs3ds_prof_stack* s_ = g_rs3ds_prof_cur; if (s_->n) s_->n = s_->n - 1; } while (0)
