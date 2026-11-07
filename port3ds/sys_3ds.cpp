// 3DS system glue: thread creation (stack size, priority).

#include <3ds.h>

#include <cstddef>
#include <cstdio>

struct __pthread_t;

// libctru creates std::thread / pthread threads with a 32 KB stack at the lowest priority (0x3F).
// The recompiled game runs deep call chains on its threads, and threads that never set their own
// priority (the game start thread, the savers) would only run when everything else sleeps. Every
// thread gets 256 KB above the game threads instead (0x29); ultramodern then sets the timing and
// game threads' priorities (set_native_thread_priority).
// The core the next thread created by this thread goes to (set right before a std::thread is made).
// New 3DS: core 2 is free for the app; it does not exist on the original 3DS, where the thread falls back to core 0.
static thread_local int t_next_core = 0;
extern "C" void rs3ds_next_thread_core(int core) { t_next_core = core; }

extern "C" int __wrap___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg, void* stack_addr, size_t stack_size) {
    if (stack_addr) return 22;   // EINVAL: caller-provided stacks are not supported (as libctru)
    if (stack_size < 256 * 1024) stack_size = 256 * 1024;
    const int core = t_next_core;
    t_next_core = 0;
    Thread t = threadCreate((ThreadFunc)func, arg, stack_size, 0x29, core, false);
    if (!t && core != 0) t = threadCreate((ThreadFunc)func, arg, stack_size, 0x29, 0, false);
    if (!t) return 12;           // ENOMEM
    *thread = (struct __pthread_t*)t;
    // the thread object and its stack are one heap block below the TLS at the stack top
    std::fprintf(stderr, "[3ds] thread %p (core %d): entry %p, stack ~%p-%p\n", (void*)t, core, (void*)func, (void*)((char*)t), (void*)((char*)t + stack_size + 64));
    return 0;
}

extern "C" {
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
}

// the memory the app got: logged at boot and when the game starts
void rs3ds_log_memory(const char* when) {
    std::fprintf(stderr, "[3ds] memory (%s): heap %08lX+%lu KB, linear %08lX+%lu KB, linear free %lu KB, app free %lu KB\n", when,
        (unsigned long)__ctru_heap, (unsigned long)(__ctru_heap_size >> 10), (unsigned long)__ctru_linear_heap,
        (unsigned long)(__ctru_linear_heap_size >> 10), (unsigned long)(linearSpaceFree() >> 10),
        (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10));
}
