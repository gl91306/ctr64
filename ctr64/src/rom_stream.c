/* ctr64: cartridge ROM streamed from the SD card.
 *
 * The Old 3DS leaves ~41 MB of heap; a 32-64 MB ROM does not fit next to the rest of the
 * emulator. Instead the ROM stays in its file and 64 KB pages of it are kept in memory, as many
 * as the heap can spare (at least RS_MIN_SLOTS), the least recently used one replaced on a miss.
 * N64 games read their cartridge almost only by PI DMA, in a few places, so this costs little
 * outside of loading. The core sees the pages through ctr64_rs_page() (device/memory/memory.h):
 * cart_rom.c (CPU reads and PI DMA) and mem_base_u32 (everything that takes a pointer).
 *
 * Pages hold the ROM in the core's byte order (32-bit words native, like the core's own copy
 * after open_rom + main.c's swap); page 0 (header and boot code) is always resident.
 * Single-threaded: only the emulation thread reads the ROM. */
#include "rom_stream.h"

#include <3ds.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "md5.h"

#define RS_SHIFT 16
#define RS_PAGE (1u << RS_SHIFT)
#define RS_MAX_PAGES (0xFC00000u / RS_PAGE) /* the cartridge address range (memory.h CTR64_RS_PAGES) */
#define RS_MIN_SLOTS 48                     /* 3 MB: always kept */
#define RS_HEADROOM (12u * 1024 * 1024)     /* heap left free for the rest of the emulator */
#define RS_LOW (8u * 1024 * 1024)           /* below this, rom_stream_trim gives pages back */
#define RS_FULL_LOAD_MAX (16 * 1024 * 1024) /* Old 3DS: bigger ROMs are streamed */

/* read by the core (memory.h) */
int ctr64_rs_active;
uint8_t *ctr64_rs_map[RS_MAX_PAGES];
uint32_t ctr64_rs_stamp[RS_MAX_PAGES];
uint32_t ctr64_rs_clock;
char ctr64_rs_md5[33];

typedef struct {
    uint8_t *buf;
    int page; /* -1: free */
} Slot;

/* Developer switch <data dir>/romstress.txt: a tiny cache that keeps replacing and giving back
 * pages, to test those paths (frames must stay identical to a full load). */
static int g_min_slots = RS_MIN_SLOTS, g_max_slots = (int)RS_MAX_PAGES;
static bool g_stress;

static struct {
    FILE *f;
    int order; /* file byte order: 0 .z64, 1 .v64, 2 .n64 */
    uint32_t size, npages;
    uint8_t *page0;
    Slot slot[RS_MAX_PAGES];
    int nslots;
    /* counters */
    uint32_t faults, evictions, trims, read_errors;
    uint64_t fault_ticks, max_fault_ticks;
    char report[200];
} S;

extern char *fake_heap_end;

/* heap still available to malloc: never handed out by sbrk, plus free chunks */
static uint32_t heap_free(void)
{
    struct mallinfo mi = mallinfo();
    const char *brk = (const char *)sbrk(0);
    const uint32_t top = brk && brk < fake_heap_end ? (uint32_t)(fake_heap_end - brk) : 0;
    return top + (uint32_t)mi.fordblks;
}

/* n bytes from the file (any byte order) to the core's byte order, in place */
static void to_core_order(uint8_t *p, uint32_t n, int order)
{
    uint32_t *w = (uint32_t *)p;
    n = (n + 3) / 4;
    if (order == 0) /* .z64: big-endian words */
        for (uint32_t i = 0; i < n; i++)
            w[i] = __builtin_bswap32(w[i]);
    else if (order == 1) /* .v64: bytes swapped in 16-bit pairs */
        for (uint32_t i = 0; i < n; i++)
            w[i] = (w[i] >> 16) | (w[i] << 16);
    /* .n64: already little-endian words */
}

/* n bytes from the file's byte order to .z64 order (what the core's MD5 covers), in place */
static void to_z64_order(uint8_t *p, uint32_t n, int order)
{
    uint32_t *w = (uint32_t *)p;
    n = (n + 3) / 4;
    if (order == 1)
        for (uint32_t i = 0; i < n; i++)
            w[i] = ((w[i] & 0x00FF00FFu) << 8) | ((w[i] >> 8) & 0x00FF00FFu);
    else if (order == 2)
        for (uint32_t i = 0; i < n; i++)
            w[i] = __builtin_bswap32(w[i]);
}

static bool read_page(int page, uint8_t *dst)
{
    const uint32_t off = (uint32_t)page << RS_SHIFT;
    const uint32_t n = S.size - off < RS_PAGE ? S.size - off : RS_PAGE;
    if (fseek(S.f, (long)off, SEEK_SET) != 0 || fread(dst, 1, n, S.f) != n) {
        S.read_errors++;
        memset(dst, 0, RS_PAGE);
        return false;
    }
    if (n < RS_PAGE)
        memset(dst + n, 0, RS_PAGE - n);
    to_core_order(dst, n, S.order);
    return true;
}

static Slot *new_slot(void)
{
    if (S.nslots >= g_max_slots)
        return NULL;
    uint8_t *b = (uint8_t *)memalign(64, RS_PAGE);
    if (!b)
        return NULL;
    Slot *s = &S.slot[S.nslots++];
    s->buf = b;
    s->page = -1;
    return s;
}

/* the least recently used slot (stamps are compared as differences, so the clock may wrap) */
static Slot *lru_slot(void)
{
    Slot *best = NULL;
    uint32_t age = 0;
    for (int i = 0; i < S.nslots; i++) {
        Slot *s = &S.slot[i];
        if (s->page < 0)
            return s;
        const uint32_t a = ctr64_rs_clock - ctr64_rs_stamp[s->page];
        if (!best || a > age) {
            best = s;
            age = a;
        }
    }
    return best;
}

/* the core missed page `page` (memory.h ctr64_rs_page) */
const uint8_t *ctr64_rs_fault(uint32_t page)
{
    static uint8_t zero[RS_PAGE] __attribute__((aligned(64)));
    if (page >= S.npages || !ctr64_rs_active)
        return zero; /* the core checks the ROM size first; never happens */
    const u64 t0 = svcGetSystemTick();
    Slot *s = NULL;
    if (heap_free() > RS_HEADROOM + RS_PAGE)
        s = new_slot();
    if (!s) {
        s = lru_slot();
        if (s->page >= 0) {
            ctr64_rs_map[s->page] = NULL;
            S.evictions++;
        }
    }
    read_page((int)page, s->buf);
    s->page = (int)page;
    ctr64_rs_map[page] = s->buf;
    const u64 dt = svcGetSystemTick() - t0;
    S.faults++;
    S.fault_ticks += dt;
    if (dt > S.max_fault_ticks)
        S.max_fault_ticks = dt;
    return s->buf;
}

void rom_stream_trim(void)
{
    if (!ctr64_rs_active)
        return;
    int keep = g_min_slots;
    if (g_stress)
        keep = S.nslots / 2 > g_min_slots ? S.nslots / 2 : g_min_slots;
    else if (heap_free() >= RS_LOW)
        return;
    /* free the least recently used pages until the heap has its headroom again */
    while (S.nslots > keep && (g_stress || heap_free() < RS_HEADROOM)) {
        Slot *v = lru_slot();
        Slot *last = &S.slot[S.nslots - 1];
        if (v->page >= 0)
            ctr64_rs_map[v->page] = NULL;
        /* the victim's buffer goes; the last slot moves into its place */
        free(v->buf);
        *v = *last;
        S.nslots--;
        S.trims++;
    }
}

bool rom_stream_wanted(long size, const char *data_dir)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/romstress.txt", data_dir);
    FILE *f = fopen(path, "r");
    g_stress = f != NULL;
    if (f)
        fclose(f);
    g_min_slots = g_stress ? 8 : RS_MIN_SLOTS;
    g_max_slots = g_stress ? 24 : (int)RS_MAX_PAGES;
    snprintf(path, sizeof(path), "%s/romstream.txt", data_dir); /* developer switch: always */
    f = fopen(path, "r");
    const bool forced = f != NULL;
    if (f)
        fclose(f);
    if (forced || g_stress)
        return true;
    bool new3ds = false;
    APT_CheckNew3DS(&new3ds);
    /* Old 3DS (80 MB mode): whole only if the heap keeps its headroom for the rest of the emulator
     * (GLideN64 grows by ~7 MB once a game draws; Super Mario 64's 8 MB loaded whole ran out) */
    return !new3ds && (size > RS_FULL_LOAD_MAX || (uint32_t)size + RS_HEADROOM > heap_free());
}

static void md5_path(char *out, size_t n, const char *data_dir, const uint8_t *z64_header)
{
    /* CRC1, CRC2 (header 0x10-0x17), country code, size: the file this MD5 belongs to */
    snprintf(out, n, "%s/jit/%02X%02X%02X%02X%02X%02X%02X%02X-%02X-%08lX.md5", data_dir, z64_header[0x10],
             z64_header[0x11], z64_header[0x12], z64_header[0x13], z64_header[0x14], z64_header[0x15],
             z64_header[0x16], z64_header[0x17], z64_header[0x3E], (unsigned long)S.size);
}

static bool md5_cached(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char h[40] = {0};
    const bool ok = fgets(h, sizeof(h), f) && strlen(h) >= 32 && strspn(h, "0123456789ABCDEF") >= 32;
    fclose(f);
    if (ok) {
        memcpy(ctr64_rs_md5, h, 32);
        ctr64_rs_md5[32] = 0;
    }
    return ok;
}

/* the whole file once, through a page-sized buffer */
static bool md5_compute(void (*progress)(int pct))
{
    uint8_t *buf = (uint8_t *)memalign(64, RS_PAGE);
    if (!buf)
        return false;
    md5_state_t st;
    md5_byte_t d[16];
    md5_init(&st);
    bool ok = fseek(S.f, 0, SEEK_SET) == 0;
    int shown = -1;
    for (uint32_t off = 0; ok && off < S.size; off += RS_PAGE) {
        const uint32_t n = S.size - off < RS_PAGE ? S.size - off : RS_PAGE;
        ok = fread(buf, 1, n, S.f) == n;
        if (!ok)
            break;
        to_z64_order(buf, n, S.order);
        md5_append(&st, buf, (int)n);
        const int pct = (int)((uint64_t)(off + n) * 100 / S.size);
        if (progress && pct / 5 != shown) {
            shown = pct / 5;
            progress(pct);
        }
    }
    free(buf);
    if (!ok)
        return false;
    md5_finish(&st, d);
    for (int i = 0; i < 16; i++)
        snprintf(ctr64_rs_md5 + i * 2, 3, "%02X", d[i]);
    return true;
}

const uint8_t *rom_stream_open(FILE *f, long size, const char *data_dir, void (*progress)(int pct))
{
    rom_stream_close();
    memset(&S, 0, sizeof(S));
    S.f = f;
    S.size = (uint32_t)size;
    S.npages = (S.size + RS_PAGE - 1) >> RS_SHIFT;
    setvbuf(f, NULL, _IONBF, 0); /* whole pages go straight to the SD card */

    uint8_t *p0 = (uint8_t *)memalign(64, RS_PAGE);
    if (!p0)
        goto fail;
    S.page0 = p0;
    {
        uint8_t first = 0;
        if (fseek(f, 0, SEEK_SET) != 0 || fread(&first, 1, 1, f) != 1)
            goto fail;
        S.order = first == 0x37 ? 1 : first == 0x40 ? 2 : 0; /* as the core's swap_copy_rom */
    }
    if (!read_page(0, p0))
        goto fail;
    {
        uint8_t hdr[0x40];
        for (int i = 0; i < 0x40; i++)
            hdr[i] = p0[i ^ 3]; /* core order -> .z64 */
        char path[320];
        md5_path(path, sizeof(path), data_dir, hdr);
        if (!md5_cached(path)) {
            if (!md5_compute(progress))
                goto fail;
            char dir[300];
            snprintf(dir, sizeof(dir), "%s/jit", data_dir);
            mkdir(dir, 0777);
            FILE *m = fopen(path, "w");
            if (m) {
                fprintf(m, "%s\n", ctr64_rs_md5);
                fclose(m);
            }
        }
    }
    for (int i = 0; i < g_min_slots; i++)
        if (!new_slot())
            goto fail;
    memset(ctr64_rs_map, 0, sizeof(ctr64_rs_map));
    memset(ctr64_rs_stamp, 0, sizeof(ctr64_rs_stamp));
    ctr64_rs_clock = 0;
    ctr64_rs_map[0] = p0;
    ctr64_rs_active = 1;
    return p0;

fail:
    rom_stream_close();
    return NULL;
}

const char *rom_stream_report(void)
{
    if (!ctr64_rs_active)
        return "";
    const double ms = 1000.0 / SYSCLOCK_ARM11;
    snprintf(S.report, sizeof(S.report),
             "ROM stream: %d pages (%d KB), %lu misses (%.0f ms, max %.1f ms), %lu evicted, %lu trimmed, %lu errors, heap free %lu KB",
             S.nslots, S.nslots * (int)(RS_PAGE / 1024), (unsigned long)S.faults, (double)S.fault_ticks * ms,
             (double)S.max_fault_ticks * ms, (unsigned long)S.evictions, (unsigned long)S.trims,
             (unsigned long)S.read_errors, (unsigned long)(heap_free() / 1024));
    return S.report;
}

void rom_stream_close(void)
{
    ctr64_rs_active = 0;
    for (int i = 0; i < S.nslots; i++)
        free(S.slot[i].buf);
    S.nslots = 0;
    free(S.page0);
    S.page0 = NULL;
    if (S.f)
        fclose(S.f);
    S.f = NULL;
    memset(ctr64_rs_map, 0, sizeof(ctr64_rs_map));
}
