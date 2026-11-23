/* Crash reports: CPU exceptions on the main thread and abort() (also reached from uncaught C++
 * exceptions such as std::bad_alloc, and from failed asserts) write sdmc:/3ds/ctr64/crash.txt
 * before the app exits. The report holds the ARM registers, the fault address, the last
 * breadcrumb (debug_crumb.h), memory figures, the emulated N64 PC and code-looking words from
 * the stack (a rough backtrace: resolve them with arm-none-eabi-addr2line -e ctr64.elf). */
#include "crash.h"

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#include "debug_crumb.h"

#define CRASH_PATH "sdmc:/3ds/ctr64/crash.txt"

static u8 g_handler_stack[16 * 1024] __attribute__((aligned(8)));
static void (*g_extra)(FILE *);

extern char __start__[], __tdata_align[]; /* 3dsx.ld: start of .text, a point in .rodata after it */

static bool is_code(u32 v)
{
    const u32 lo = (u32)__start__, hi = (u32)__tdata_align;
    return v >= lo && v < hi;
}

/* words around `addr` if that memory is readable (JIT code has no symbols to resolve) */
static void dump_words(FILE *f, const char *what, u32 addr)
{
    MemInfo mi;
    PageInfo pi;
    const u32 a = (addr & ~3u) - 32;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || !(mi.perm & MEMPERM_READ) || a + 64 > mi.base_addr + mi.size)
        return;
    fprintf(f, "%s %08lx:", what, a);
    for (int i = 0; i < 16; i++)
        fprintf(f, " %08lx", ((const u32 *)a)[i]);
    fprintf(f, "\n");
}

static void write_common(FILE *f, u32 sp)
{
    fprintf(f, "crumb: %s\n", ctr64_crumb ? ctr64_crumb : "(none)");
    fprintf(f, "heap used %u KB, linear free %lu KB, vram free %lu KB\n",
            (unsigned)(mallinfo().uordblks / 1024), linearSpaceFree() / 1024, vramSpaceFree() / 1024);
    if (g_extra)
        g_extra(f);
    fprintf(f, "stack code words:");
    const u32 *s = (const u32 *)(sp & ~3u);
    for (int i = 0, n = 0; i < 1024 && n < 32; i++)
        if (is_code(s[i])) {
            fprintf(f, " %08lx", s[i]);
            n++;
        }
    fprintf(f, "\n");
}

static void exception_handler(ERRF_ExceptionInfo *e, CpuRegisters *r)
{
    static const char *const kinds[] = {"prefetch abort", "data abort", "undefined instruction", "VFP"};
    FILE *f = fopen(CRASH_PATH, "w");
    if (f) {
        fprintf(f, "ctr64 crash: %s\n", (*(const u8 *)e) < 4 ? kinds[*(const u8 *)e] : "exception");
        fprintf(f, "pc %08lx lr %08lx sp %08lx cpsr %08lx\n", r->pc, r->lr, r->sp, r->cpsr);
        /* libctru's layout (u8 type, 3 reserved, fsr, far): this file is built with int-sized
         * enums, which would shift the fields (far and fsr came out swapped) */
        const u32 *ew = (const u32 *)e;
        fprintf(f, "far %08lx fsr %08lx\n", ew[2], ew[1]);
        for (int i = 0; i < 13; i++)
            fprintf(f, "r%-2d %08lx%s", i, r->r[i], i % 4 == 3 ? "\n" : "  ");
        fprintf(f, "\n");
        if (!is_code(r->pc))
            dump_words(f, "code at", r->pc); /* e.g. inside the JIT cache */
        write_common(f, r->sp);
        fclose(f);
    }
    ERRF_ExceptionHandler(e, r); /* the system error screen, then the app ends */
}

void __real_abort(void) __attribute__((noreturn));
void __wrap_abort(void) __attribute__((noreturn));
void __wrap_abort(void)
{
    FILE *f = fopen(CRASH_PATH, "w");
    if (f) {
        u32 sp;
        __asm__ volatile("mov %0, sp" : "=r"(sp));
        fprintf(f, "ctr64 crash: abort() called from %08lx\n", (u32)__builtin_return_address(0));
        write_common(f, sp);
        fclose(f);
    }
    __real_abort();
}

/* The core ends the program with exit(1) on errors it cannot handle. The process must not end
 * with ctr64's threads alive on the other cores: the kernel crashed in a thread switch on core 1 or
 * 2 (HW 2026-11-22; Luma's exception screen, pc 3fef3ffe). They are stopped first, and ctr64 starts
 * again (the game list) instead of leaving. */
void __real_exit(int code) __attribute__((noreturn));
void __wrap_exit(int code) __attribute__((noreturn));
void __wrap_exit(int code)
{
    static bool ending;
    if (code && !ending) {
        ending = true;
        FILE *f = fopen(CRASH_PATH, "w");
        if (f) {
            u32 sp;
            __asm__ volatile("mov %0, sp" : "=r"(sp));
            fprintf(f, "ctr64 stopped: exit(%d) called from %08lx\n", code, (u32)__builtin_return_address(0));
            write_common(f, sp);
            fclose(f);
        }
        extern void ctr_jit_thread_abandon(void), ctr64_gfx_async_abandon(void), ctr64_audio_async_abandon(void);
        ctr_jit_thread_abandon();
        ctr64_gfx_async_abandon();
        ctr64_audio_async_abandon();
        if (!envIsHomebrew())
            aptSetChainloaderToSelf();
    }
    __real_exit(code);
}

void crash_install(void (*extra)(FILE *))
{
    g_extra = extra;
    remove(CRASH_PATH ".old");
    rename(CRASH_PATH, CRASH_PATH ".old"); /* keep the previous report one run longer */
    threadOnException(exception_handler, g_handler_stack + sizeof(g_handler_stack), WRITE_DATA_TO_HANDLER_STACK);
}
