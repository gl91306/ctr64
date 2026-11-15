/* Development aid (CMake -DCTR64_MEMCPY_PROF=ON, link --wrap=memcpy): calls and bytes of memcpy
 * per calling address, written to sdmc:/3ds/ctr64/memcpy.log every 10 s (resolve the addresses
 * with addr2line). Counters are updated without locks: approximate under threads. */
#ifdef CTR64_MEMCPY_PROF
#include <3ds.h>
#include <stdio.h>
#include <stddef.h>

void *__real_memcpy(void *d, const void *s, size_t n);

#define SLOTS 1024
static struct { u32 caller; u32 calls; u64 bytes; } g_slot[SLOTS];
static u64 g_last_dump;

static void dump(void)
{
    FILE *f = fopen("sdmc:/3ds/ctr64/memcpy.log", "w");
    if (!f)
        return;
    for (int i = 0; i < SLOTS; i++)
        if (g_slot[i].caller)
            fprintf(f, "%08lx %lu %llu\n", (unsigned long)g_slot[i].caller, (unsigned long)g_slot[i].calls,
                    (unsigned long long)g_slot[i].bytes);
    fclose(f);
}

void *__wrap_memcpy(void *d, const void *s, size_t n)
{
    const u32 caller = (u32)__builtin_return_address(0);
    u32 h = (caller >> 2) * 2654435761u >> 22;
    for (int k = 0; k < 8; k++, h = (h + 1) & (SLOTS - 1)) {
        if (g_slot[h].caller == caller || g_slot[h].caller == 0) {
            g_slot[h].caller = caller;
            g_slot[h].calls++;
            g_slot[h].bytes += n;
            break;
        }
    }
    if (n >= 64 && caller) {
        static volatile int dumping;
        const u64 now = svcGetSystemTick();
        if (now - g_last_dump > 10ull * SYSCLOCK_ARM11 && !dumping) {
            dumping = 1;
            g_last_dump = now;
            dump();
            dumping = 0;
        }
    }
    return __real_memcpy(d, s, n);
}
#endif
