// Sampling profiler for core 0 (see include/rs3ds_prof.h). Every 5 s it logs to rs64_log.txt:
//   [prof] core 0: <share> game threads (by N64 thread id), <share> other/idle
//   [prof] top: <function vram> <share of samples> ...   (the function on top of the running thread's stack)
// Function addresses map to names in RecompiledFuncs (the "// 0x<vram>:" comment at each function start).

#include <3ds.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rs3ds_prof.h"

namespace {

rs3ds_prof_stack g_idle_stack;               // used while no N64 thread has run yet
rs3ds_prof_stack g_stacks[64];               // one per N64 thread (slot)
uint32_t g_slot_thread[64];                  // the OSThread address of each slot
int g_slot_id[64];                           // its N64 thread id
int g_nslots = 0;
std::atomic<int> g_running_id{ -1 };         // the slot running now, -1 = none (blocked / idle)
std::atomic<int> g_synth_wait{ 0 };          // the running thread waits for the audio synth

constexpr int HBITS = 13, HSIZE = 1 << HBITS;
struct Slot { uint32_t vram, count; };
Slot g_hist[HSIZE];
uint32_t g_thread_samples[64];
uint32_t g_total = 0, g_idle = 0, g_empty = 0, g_synth = 0;
std::atomic<uint32_t> g_lookups{ 0 };

void count(uint32_t vram) {
    uint32_t h = (vram >> 2) * 2654435761u >> (32 - HBITS);
    for (int i = 0; i < 32; i++) {
        Slot& s = g_hist[(h + i) & (HSIZE - 1)];
        if (s.vram == vram) { s.count++; return; }
        if (s.vram == 0) { s.vram = vram; s.count = 1; return; }
    }
}

void report(double secs) {
    if (!g_total) return;
    char line[512];
    int n = std::snprintf(line, sizeof line, "[prof] core 0, %u samples: game %.0f%%, waiting for audio synth %.0f%%, other/idle %.0f%% |", g_total,
        100.0 * (g_total - g_idle - g_synth) / g_total, 100.0 * g_synth / g_total, 100.0 * g_idle / g_total);
    for (int k = 0; k < g_nslots && n < (int)sizeof line - 40; k++)
        if (g_thread_samples[k] * 100 >= g_total)
            n += std::snprintf(line + n, sizeof line - n, " id%d@%06X %.0f%%", g_slot_id[k], (unsigned)(g_slot_thread[k] & 0xFFFFFF), 100.0 * g_thread_samples[k] / g_total);
    std::fprintf(stderr, "%s | lookups %.0f/s\n", line, g_lookups.exchange(0) / secs);

    std::vector<Slot> top;
    for (const Slot& s : g_hist) if (s.vram) top.push_back(s);
    std::sort(top.begin(), top.end(), [](const Slot& a, const Slot& b) { return a.count > b.count; });
    n = std::snprintf(line, sizeof line, "[prof] top:");
    for (size_t i = 0; i < top.size() && i < 16 && n < (int)sizeof line - 24; i++)
        n += std::snprintf(line + n, sizeof line - n, " %08X %.1f%%", (unsigned)top[i].vram, 100.0 * top[i].count / g_total);
    std::fprintf(stderr, "%s\n", line);

    std::memset(g_hist, 0, sizeof g_hist);
    std::memset(g_thread_samples, 0, sizeof g_thread_samples);
    g_total = g_idle = g_empty = g_synth = 0;
}

void sampler(void*) {
    uint64_t t0 = svcGetSystemTick();
    for (;;) {
        svcSleepThread(1000000);   // 1 ms
        g_total++;
        const int id = g_running_id.load(std::memory_order_relaxed);
        if (id < 0 || id >= 64) {
            g_idle++;
        } else if (g_synth_wait.load(std::memory_order_relaxed)) {
            g_synth++;
        } else {
            g_thread_samples[id]++;
            const rs3ds_prof_stack* s = &g_stacks[id];
            const uint32_t depth = s->n;
            if (depth) count(s->a[(depth - 1) & 63]);
            else g_empty++;
        }
        const uint64_t now = svcGetSystemTick();
        if (now - t0 > 5ull * SYSCLOCK_ARM11) {
            report((double)(now - t0) / SYSCLOCK_ARM11);
            t0 = now;
        }
    }
}

} // namespace

extern "C" {
rs3ds_prof_stack* volatile g_rs3ds_prof_cur = &g_idle_stack;

static int slot_of(uint32_t thread, int id) {
    for (int k = 0; k < g_nslots; k++) if (g_slot_thread[k] == thread) return k;
    if (g_nslots < 64) { g_slot_thread[g_nslots] = thread; g_slot_id[g_nslots] = id; return g_nslots++; }
    return 63;
}

void rs3ds_prof_resume(uint32_t thread, int thread_id) {
    const int k = slot_of(thread, thread_id);
    g_rs3ds_prof_cur = &g_stacks[k];
    g_running_id.store(k, std::memory_order_relaxed);
}

void rs3ds_prof_block(uint32_t thread) {
    for (int k = 0; k < g_nslots; k++)
        if (g_slot_thread[k] == thread) {
            int expected = k;
            g_running_id.compare_exchange_strong(expected, -1, std::memory_order_relaxed);
            return;
        }
}

void rs3ds_prof_synth_wait(int waiting) { g_synth_wait.store(waiting, std::memory_order_relaxed); }

void rs3ds_prof_lookup(void) { g_lookups.fetch_add(1, std::memory_order_relaxed); }

void rs3ds_prof_start(void) {
    // core 0 (where the game runs), above every game and runtime thread so it samples on time
    threadCreate(sampler, nullptr, 16 * 1024, 0x18, 0, true);
    std::fprintf(stderr, "[prof] sampler started (1 ms, core 0)\n");
}
}
