#include "core_host.h"

#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern char ctr64_rs_md5[33]; /* rom_stream.c */

#include "audio_ndsp.h"
#include "prof.h"
#include "rom_browser.h"
#include "rom_stream.h"
#include "input_script.h"
#include "libretro.h"
#include "settings.h"

/* glsm shim (src/core_compat): the core's context_reset, captured at load time */
extern retro_hw_context_reset_t ctr64_core_context_reset;

#define MAX_OPTIONS 160

typedef struct {
    char key[64];
    char value[48];
} Option;

/* 3DS-specific choices that override the core's defaults. */
static const struct { const char *key, *value; } k_overrides[] = {
    {"mupen64plus-cpucore", "dynamic_recompiler"},
    {"mupen64plus-rsp-plugin", "hle"},
    {"mupen64plus-rdp-plugin", "gliden64"},
    {"mupen64plus-ThreadedRenderer", "False"},
    /* GLideN64 on the PICA backend: native resolution, no features the PICA cannot do.
     * Frame buffer emulation stays on: it is GLideN64's normal path. */
    {"mupen64plus-EnableFBEmulation", "True"},
    {"mupen64plus-EnableNativeResFactor", "1"},
    {"mupen64plus-MultiSampling", "0"},
    {"mupen64plus-FXAA", "0"},
    {"mupen64plus-EnableCopyColorToRDRAM", "Off"},
    {"mupen64plus-EnableCopyDepthToRDRAM", "Off"},
    {"mupen64plus-EnableCopyAuxToRDRAM", "False"},
    {"mupen64plus-EnableN64DepthCompare", "False"},
    {"mupen64plus-EnableShadersStorage", "False"},
    {"mupen64plus-EnableLODEmulation", "False"},
    {"mupen64plus-EnableHWLighting", "False"},
    {"mupen64plus-EnableLegacyBlending", "True"},
    {"mupen64plus-EnableFragmentDepthWrite", "False"},
    {"mupen64plus-EnableInaccurateTextureCoordinates", "True"},
    {"mupen64plus-EnableOverscan", "Disabled"},
    {"mupen64plus-txHiresEnable", "False"},
    {"mupen64plus-txFilterMode", "None"},
    {"mupen64plus-txEnhancementMode", "None"},
    {"mupen64plus-aspect", "4:3"},
    {"mupen64plus-43screensize", "320x240"},
    /* texture cache entries: textures live in the 12 MB GPU-visible linear heap */
    {"mupen64plus-MaxTxCacheSize", "600"},
};

static struct {
    char data_dir[256];
    char system_dir[256];
    char save_dir[256];
    Option opts[MAX_OPTIONS];
    int nopts;
    Option user[32]; /* overrides from <data_dir>/options.txt (key=value lines) */
    int nuser;
    FILE *log;
    bool loaded;
    struct retro_system_av_info av;
    char srm_path[300];
    unsigned char *srm_shadow; /* save RAM as last written to the SD card */
    char rom_base[128];        /* ROM file name without extension, names saves and states */
    char rom_id[24];           /* header CRC1, CRC2 and country: names remembered-block lists */
    bool input_blocked;        /* menu open: the game sees no buttons */
    bool input_release;        /* ... nor afterwards until all buttons are released */
} H;

/* ---- logging ------------------------------------------------------------------------------ */

static void RETRO_CALLCONV core_log(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;

    /* buffered: a flush per line is an SD write on the emulation or video thread. Errors are
     * flushed at once (crash reports), the rest at menu, suspend and unload (core_host_flush_log) */
    if (H.log && (level > RETRO_LOG_DEBUG || g_settings.debug_info)) {
        va_start(ap, fmt);
        vfprintf(H.log, fmt, ap);
        va_end(ap);
        if (level >= RETRO_LOG_ERROR)
            fflush(H.log);
    }
    if (level >= RETRO_LOG_WARN && g_settings.debug_info) { /* on the bottom screen: only with Debug info */
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
}

/* ---- options (legacy SET_VARIABLES interface) ---------------------------------------------- */

static void set_variables(const struct retro_variable *vars)
{
    H.nopts = 0;
    for (; vars->key && H.nopts < MAX_OPTIONS; vars++) {
        Option *o = &H.opts[H.nopts++];
        const char *sep = strstr(vars->value, "; ");
        const char *first = sep ? sep + 2 : "";
        size_t len = strcspn(first, "|");

        snprintf(o->key, sizeof(o->key), "%s", vars->key);
        snprintf(o->value, sizeof(o->value), "%.*s", (int)len, first);
        for (size_t i = 0; i < sizeof(k_overrides) / sizeof(k_overrides[0]); i++)
            if (!strcmp(o->key, k_overrides[i].key))
                snprintf(o->value, sizeof(o->value), "%s", k_overrides[i].value);
        for (int i = 0; i < H.nuser; i++)
            if (!strcmp(o->key, H.user[i].key))
                snprintf(o->value, sizeof(o->value), "%s", H.user[i].value);
    }
}

static void load_user_options(const char *path)
{
    char line[160];
    FILE *f = fopen(path, "r");

    if (!f)
        return;
    while (fgets(line, sizeof(line), f) && H.nuser < 32) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq = 0;
        eq[1 + strcspn(eq + 1, "\r\n")] = 0;
        /* longer keys and values are cut to the field size */
        snprintf(H.user[H.nuser].key, sizeof(H.user[0].key), "%.*s", (int)sizeof(H.user[0].key) - 1, line);
        snprintf(H.user[H.nuser].value, sizeof(H.user[0].value), "%.*s", (int)sizeof(H.user[0].value) - 1, eq + 1);
        if (H.log)
            fprintf(H.log, "option %s = %s\n", H.user[H.nuser].key, H.user[H.nuser].value);
        H.nuser++;
    }
    fclose(f);
}

static bool get_variable(struct retro_variable *var)
{
    for (int i = 0; i < H.nopts; i++) {
        if (!strcmp(H.opts[i].key, var->key)) {
            var->value = H.opts[i].value;
            return true;
        }
    }
    var->value = NULL;
    return false;
}

/* ---- libretro callbacks ---------------------------------------------------------------------- */

static uint64_t RETRO_CALLCONV cpu_features(void) { return 0; }
static retro_time_t RETRO_CALLCONV time_usec(void) { return (retro_time_t)(osGetTime() * 1000); }
static retro_perf_tick_t RETRO_CALLCONV perf_counter(void) { return svcGetSystemTick(); }
static void RETRO_CALLCONV perf_noop(struct retro_perf_counter *c) { (void)c; }

static bool RETRO_CALLCONV environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
        struct retro_perf_callback *p = data;
        memset(p, 0, sizeof(*p));
        p->get_time_usec = time_usec;
        p->get_cpu_features = cpu_features;
        p->get_perf_counter = perf_counter;
        p->perf_register = perf_noop;
        p->perf_start = perf_noop;
        p->perf_stop = perf_noop;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = H.system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = H.save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        set_variables((const struct retro_variable *)data);
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
        return get_variable((struct retro_variable *)data);
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
        if (cmd == RETRO_ENVIRONMENT_GET_JIT_CAPABLE)
            *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        return true;
    default:
        return false;
    }
}

static void RETRO_CALLCONV video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    /* Presentation is done by GLideN64's swap (PICA backend), not after retro_run. */
    (void)data; (void)w; (void)h; (void)pitch;
}

static void RETRO_CALLCONV audio_sample(int16_t l, int16_t r)
{
    int16_t f[2] = {l, r};
    audio_ndsp_write(f, 1);
}

static size_t RETRO_CALLCONV audio_batch(const int16_t *data, size_t frames)
{
    return audio_ndsp_write(data, frames);
}

static void RETRO_CALLCONV input_poll(void) {}

/* Development aid: <data dir>/pokes.txt writes N64 RAM at given VIs (counted like inputs.txt),
 * e.g. to warp to other scenes while recording remembered blocks. One write per line, the same
 * syntax as the Windows build's CTR64_REF_POKE: <vi>:<addr>=<value>[/<bytes 1,2,4>] (hex address
 * and value, big-endian value); several per line separated by commas. */
#define MAX_POKES 4096
static struct { u32 vi, addr, value; u8 bytes, peek; } g_pokes[MAX_POKES];
static int g_poke_n, g_poke_next;

static void pokes_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    g_poke_n = g_poke_next = 0;
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *p;
        const unsigned long vi = strtoul(line, &p, 10);
        if (*p != ':')
            continue;
        while ((*p == ':' || *p == ',') && g_poke_n < MAX_POKES) {
            unsigned long addr = strtoul(p + 1, &p, 16), value = 0, bytes = 1;
            const int peek = *p == '?'; /* <vi>:<addr>?<bytes>: log the value instead */
            if (*p == '=')
                value = strtoul(p + 1, &p, 16);
            else if (peek)
                bytes = strtoul(p + 1, &p, 10);
            if (*p == '/')
                bytes = strtoul(p + 1, &p, 10);
            g_pokes[g_poke_n].vi = (u32)vi;
            g_pokes[g_poke_n].addr = (u32)addr;
            g_pokes[g_poke_n].value = (u32)value;
            g_pokes[g_poke_n].bytes = (u8)(bytes > 4 ? 4 : bytes);
            g_pokes[g_poke_n].peek = (u8)peek;
            g_poke_n++;
        }
    }
    fclose(f);
}

/* after each VI: the writes due (lines in VI order) */
static void pokes_apply(unsigned vi)
{
    unsigned char *ram = g_poke_n ? retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM) : NULL;
    while (ram && g_poke_next < g_poke_n && g_pokes[g_poke_next].vi <= vi) {
        const u32 addr = g_pokes[g_poke_next].addr, value = g_pokes[g_poke_next].value;
        const unsigned bytes = g_pokes[g_poke_next].bytes;
        if (g_pokes[g_poke_next].peek) {
            u32 v = 0;
            for (unsigned i = 0; i < bytes; i++)
                v = v << 8 | ram[((addr + i) & 0x7FFFFF) ^ 3];
            if (H.log)
                fprintf(H.log, "peek VI %u: %08lx = %lx (%u bytes)\n", vi, (unsigned long)addr, (unsigned long)v, bytes);
            g_poke_next++;
            continue;
        }
        for (unsigned i = 0; i < bytes; i++) /* RDRAM is held as host-order words */
            ram[((addr + i) & 0x7FFFFF) ^ 3] = (unsigned char)(value >> (8 * (bytes - 1 - i)));
        if (H.log)
            fprintf(H.log, "poke VI %u: %08lx = %lx (%u bytes)\n", vi, (unsigned long)addr, (unsigned long)value, bytes);
        g_poke_next++;
    }
}

extern unsigned int ctr64_video_frames; /* core_compat/gln64_ctr.c */
extern unsigned ctr64_vi_count;        /* gfx_async.c: VIs queued, counted on this thread */
void ctr64_gfx_drain(void);
void ctr64_gfx_async_stop(void);
void ctr64_audio_async_stop(void);

static int16_t RETRO_CALLCONV input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    u32 k = hidKeysHeld();

    if (port != 0)
        return 0;
    if (g_input_script.loaded) /* <data dir>/inputs.txt replaces the pad (development aid) */
        return input_script_state(ctr64_vi_count + 1, device, index, id);
    if (H.input_blocked)
        return 0;
    if (H.input_release) { /* menu just closed: wait until the button that closed it is up */
        if (k & ~(KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT))
            return 0;
        H.input_release = false;
    }
    const u32 *m = g_settings.map;
    if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD) {
        /* RetroPad mapping used by Mupen64Plus-Next: B = N64 A, Y = N64 B, L2 = Z; the 3DS
         * button for each comes from the settings (menu.c). A D-pad direction assigned to an N64
         * button (the Old 3DS defaults put the C buttons there) is no longer the N64 D-pad. */
        u32 used = 0;
        for (int i = 0; i < IN_COUNT; i++)
            used |= m[i];
        const struct { unsigned id; u32 key; } map[] = {
            {RETRO_DEVICE_ID_JOYPAD_B, m[IN_A]},     {RETRO_DEVICE_ID_JOYPAD_Y, m[IN_B]},
            {RETRO_DEVICE_ID_JOYPAD_L2, m[IN_Z]},
            {RETRO_DEVICE_ID_JOYPAD_L, m[IN_L]},     {RETRO_DEVICE_ID_JOYPAD_R, m[IN_R]},
            {RETRO_DEVICE_ID_JOYPAD_START, m[IN_START]},
            {RETRO_DEVICE_ID_JOYPAD_UP, KEY_DUP & ~used},     {RETRO_DEVICE_ID_JOYPAD_DOWN, KEY_DDOWN & ~used},
            {RETRO_DEVICE_ID_JOYPAD_LEFT, KEY_DLEFT & ~used}, {RETRO_DEVICE_ID_JOYPAD_RIGHT, KEY_DRIGHT & ~used},
        };
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            int16_t mask = 0;
            for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
                if (k & map[i].key)
                    mask |= 1 << map[i].id;
            return mask;
        }
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (map[i].id == id)
                return (k & map[i].key) ? 1 : 0;
        return 0;
    }
    if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG) {
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
            circlePosition c;
            hidCircleRead(&c);
            int v = id == RETRO_DEVICE_ID_ANALOG_X ? c.dx : -c.dy;
            v = v * 32767 * g_settings.stick_range / (150 * 100);
            return v > 32767 ? 32767 : v < -32767 ? -32767 : (int16_t)v;
        }
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
            /* C buttons: C-stick on New 3DS, plus the buttons mapped to them */
            circlePosition c;
            hidCstickRead(&c);
            int v = id == RETRO_DEVICE_ID_ANALOG_X ? c.dx : -c.dy;
            v = v * 32767 / 140;
            if (id == RETRO_DEVICE_ID_ANALOG_X && (k & m[IN_CLEFT]))
                v = -32767;
            if (id == RETRO_DEVICE_ID_ANALOG_X && (k & m[IN_CRIGHT]))
                v = 32767;
            if (id == RETRO_DEVICE_ID_ANALOG_Y && (k & m[IN_CUP]))
                v = -32767;
            if (id == RETRO_DEVICE_ID_ANALOG_Y && (k & m[IN_CDOWN]))
                v = 32767;
            return v > 32767 ? 32767 : v < -32767 ? -32767 : (int16_t)v;
        }
    }
    return 0;
}

/* ---- lifecycle --------------------------------------------------------------------------------- */

bool core_host_init(const char *data_dir)
{
    char path[300];

    memset(&H, 0, sizeof(H));
    mkdir(data_dir, 0777);
    snprintf(H.data_dir, sizeof(H.data_dir), "%s", data_dir);
    snprintf(H.system_dir, sizeof(H.system_dir), "%s/system", data_dir);
    snprintf(H.save_dir, sizeof(H.save_dir), "%s/saves", data_dir);
    mkdir(H.system_dir, 0777);
    mkdir(H.save_dir, 0777);
    snprintf(path, sizeof(path), "%s/states", data_dir);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/jit", data_dir);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/core.log", data_dir);
    H.log = fopen(path, "w");
    if (H.log) {
        setvbuf(H.log, NULL, _IOFBF, 16 * 1024);
        fprintf(H.log, "ctr64 %s\n", CTR64_VERSION);
    }
    {   /* development aid: <data dir>/gfxdelay.txt sets the threaded display-list delay (VI/4) */
        extern unsigned ctr64_gfx_delay_q4;
        snprintf(path, sizeof(path), "%s/gfxdelay.txt", data_dir);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%u", &ctr64_gfx_delay_q4) != 1)
                ctr64_gfx_delay_q4 = 0;
            fclose(f);
        }
    }
    {   /* development aid: <data dir>/noaudiothread.txt keeps audio tasks on the CPU thread */
        extern bool ctr64_audio_thread_off;
        snprintf(path, sizeof(path), "%s/noaudiothread.txt", data_dir);
        FILE *f = fopen(path, "r");
        ctr64_audio_thread_off = f != NULL;
        if (f)
            fclose(f);
    }
    snprintf(path, sizeof(path), "%s/options.txt", data_dir);
    load_user_options(path);
    snprintf(path, sizeof(path), "%s/inputs.txt", data_dir);
    input_script_load(path);
    snprintf(path, sizeof(path), "%s/pokes.txt", data_dir);
    pokes_load(path);

    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();
    return true;
}

/* ---- cartridge save RAM ---------------------------------------------------------------------
 * The core keeps EEPROM, SRAM, FlashRAM and the controller paks in one block
 * (RETRO_MEMORY_SAVE_RAM), which the frontend persists as <save dir>/<ROM name>.srm, the same
 * raw format as RetroArch and the Windows build. It is written once a second when it changed,
 * and on exit, through a temporary file so a power-off mid-write keeps the previous save. */
static void srm_path_for(const char *rom_path)
{
    const char *base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    snprintf(H.rom_base, sizeof(H.rom_base), "%s", base);
    char *ext = strrchr(H.rom_base, '.');
    if (ext)
        *ext = 0;
    snprintf(H.srm_path, sizeof(H.srm_path), "%s/%s", H.save_dir, base);
    char *dot = strrchr(H.srm_path, '.');
    if (dot && dot > strrchr(H.srm_path, '/'))
        *dot = 0;
    strncat(H.srm_path, ".srm", sizeof(H.srm_path) - strlen(H.srm_path) - 1);
}

static void srm_load(void)
{
    unsigned char *mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !size)
        return;
    char tmp[310];
    snprintf(tmp, sizeof(tmp), "%s.tmp", H.srm_path);
    FILE *f = fopen(H.srm_path, "rb");
    if (!f && (f = fopen(tmp, "rb"))) /* power lost between removing the old file and the rename */
        rename(tmp, H.srm_path);
    if (f) {
        const size_t n = fread(mem, 1, size, f);
        fclose(f);
        if (H.log)
            fprintf(H.log, "save RAM: loaded %u of %u bytes from %s\n", (unsigned)n, (unsigned)size, H.srm_path);
    } else if (H.log) {
        fprintf(H.log, "save RAM: no %s yet\n", H.srm_path);
    }
    H.srm_shadow = malloc(size);
    if (H.srm_shadow)
        memcpy(H.srm_shadow, mem, size);
}

/* writes the save RAM copy in H.srm_shadow (tmp file + rename); true if it reached the card */
static bool srm_write(bool log)
{
    const size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    char tmp[310];
    snprintf(tmp, sizeof(tmp), "%s.tmp", H.srm_path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(H.srm_shadow, 1, size, f) == size;
    if (f)
        ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(H.srm_path);
        ok = rename(tmp, H.srm_path) == 0;
    }
    if (log && H.log) {
        fprintf(H.log, "save RAM: %s %s\n", ok ? "wrote" : "FAILED to write", H.srm_path);
        fflush(H.log);
    }
    return ok;
}

/* In play the write runs on a low-priority thread: an SD write on the emulation thread can
 * stall the game for a long time on hardware (see core_host_rb_tick). */
static Thread g_srm_thread;
static volatile bool g_srm_busy, g_srm_ok = true;

static void srm_thread(void *arg)
{
    (void)arg;
    g_srm_ok = srm_write(false); /* logged by srm_join, on the main thread */
    g_srm_busy = false;
}

static void srm_join(void)
{
    if (!g_srm_thread)
        return;
    threadJoin(g_srm_thread, U64_MAX);
    threadFree(g_srm_thread);
    g_srm_thread = NULL;
    if (H.log)
        fprintf(H.log, "save RAM: %s %s\n", g_srm_ok ? "wrote" : "FAILED to write", H.srm_path);
}

/* sync: write now on this thread (leaving the game) */
static void srm_flush(bool sync)
{
    const unsigned char *mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || !size || !H.srm_shadow)
        return;
    if (g_srm_busy && !sync)
        return; /* the previous write is still going: try again later */
    srm_join();
    if (g_srm_ok && !memcmp(H.srm_shadow, mem, size))
        return;
    memcpy(H.srm_shadow, mem, size); /* the copy being written (unchanged until the write ends) */
    if (!sync) {
        g_srm_busy = true;
        g_srm_thread = threadCreate(srm_thread, NULL, 8 * 1024, 0x3F, 1, false);
        if (!g_srm_thread)
            g_srm_thread = threadCreate(srm_thread, NULL, 8 * 1024, 0x3F, -2, false);
        if (g_srm_thread)
            return;
        g_srm_busy = false;
    }
    g_srm_ok = srm_write(true);
}

static void rb_load(void);

/* H.rom_id from the ROM header (any byte order): CRC1, CRC2 and the country code, the same for
 * every copy of one release whatever the file is called; revisions and hacks differ */
static void rom_id_from_header(const unsigned char *d, long size)
{
    unsigned char h[0x40];
    if (size < 0x40) {
        snprintf(H.rom_id, sizeof(H.rom_id), "%s", "unknown");
        return;
    }
    for (int i = 0; i < 0x40; i++) {
        if (d[0] == 0x80)                      /* .z64: big-endian */
            h[i] = d[i];
        else if (d[0] == 0x37)                 /* .v64: 16-bit byte-swapped */
            h[i] = d[i ^ 1];
        else                                   /* .n64: 32-bit little-endian */
            h[i] = d[i ^ 3];
    }
    snprintf(H.rom_id, sizeof(H.rom_id), "%02X%02X%02X%02X%02X%02X%02X%02X-%02X", h[0x10], h[0x11], h[0x12],
             h[0x13], h[0x14], h[0x15], h[0x16], h[0x17], h[0x3E]);
}

void ctr64_mem_prepare(long rom_bytes, const char *game_id);
const char *ctr64_mem_reserve_state(void);

bool core_host_load(const char *rom_path)
{
    struct retro_game_info info;
    FILE *f = fopen(rom_path, "rb");
    void *data;
    long size;

    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* N64 cartridges hold 4 KB (header and boot code) to 64 MB */
    if (size < 0x1000 || size > 64 * 1024 * 1024) {
        if (H.log)
            fprintf(H.log, "ROM %s: size %ld is not an N64 ROM\n", rom_path, size);
        fclose(f);
        return false;
    }
    {
        /* the game code (ROM header 0x3B, 4 characters, e.g. "NZSE"), in any byte order */
        unsigned char h[0x40] = {0};
        char id[5] = {0};
        if (fread(h, 1, sizeof(h), f) == sizeof(h)) {
            for (int i = 0; i < 4; i++) {
                int o = 0x3B + i;
                if (h[0] == 0x37) /* .v64: 16-bit words swapped */
                    o ^= 1;
                else if (h[0] == 0x40) /* .n64: 32-bit words reversed */
                    o ^= 3;
                id[i] = (char)h[o];
            }
        }
        fseek(f, 0, SEEK_SET);
        ctr64_mem_prepare(size, id); /* main.c: New 3DS memory reserve */
    }
    if (H.log) {
        extern u32 __ctru_heap_size, __ctru_linear_heap_size;
        struct mallinfo mi = mallinfo();
        fprintf(H.log, "memory: app %lu KB, heap %lu KB (used %u KB), linear %lu KB, reserve %s, ROM %ld KB\n",
                osGetMemRegionSize(MEMREGION_APPLICATION) / 1024, __ctru_heap_size / 1024, mi.uordblks / 1024,
                __ctru_linear_heap_size / 1024, ctr64_mem_reserve_state(), size / 1024);
        {
            extern char pica_fill_report[96];
            fprintf(H.log, "%s\n", pica_fill_report);
            extern char video_npot_report[96];
            fprintf(H.log, "%s\n", video_npot_report);
        }
        fflush(H.log);
    }
    /* Old 3DS: a ROM over 16 MB stays on the SD card and is read in pages (rom_stream.c) */
    bool stream = rom_stream_wanted(size, H.data_dir);
    data = stream ? NULL : malloc(size);
    if (!stream && !data)
        stream = true; /* no room for all of it: stream it after all */
    if (stream) {
        data = (void *)rom_stream_open(f, size, H.data_dir, rom_browser_loading_progress); /* takes f */
        if (H.log)
            fprintf(H.log, "ROM streamed from the SD card: %s (MD5 %s)\n", data ? "ok" : "FAILED", ctr64_rs_md5);
        if (!data)
            return false;
    } else {
        if (fread(data, 1, size, f) != (size_t)size) {
            fclose(f);
            free(data);
            return false;
        }
        fclose(f);
    }

    memset(&info, 0, sizeof(info));
    info.path = rom_path;
    info.data = data;
    info.size = size;
    /* Ownership of `data` passes to the core (see the __3DS__ branch in retro_load_game);
     * it frees the buffer once the ROM has been opened. */
    rom_id_from_header(data, size);
    {
        extern unsigned int ctr64_game_starts; /* core_compat/gln64_ctr.c: per-game choices (pica_ContextImpl) */
        ctr64_game_starts++;
    }
    if (!retro_load_game(&info)) {
        rom_stream_close();
        return false;
    }

    srm_path_for(rom_path);
    srm_load();
    retro_get_system_av_info(&H.av);
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    audio_ndsp_init((unsigned)(H.av.timing.sample_rate + 0.5));
    /* The core defers emulator start until its (here: virtual) GL context is reset. */
    if (ctr64_core_context_reset)
        ctr64_core_context_reset();
    if (H.log) {
        struct mallinfo mi = mallinfo();
        fprintf(H.log, "heap after load: %u KB used (ROM %ld KB)\n", mi.uordblks / 1024, size / 1024);
    }
    H.loaded = true;
    rb_load();
    return true;
}

/* Remembered blocks (new_dynarec.c ctr64_rb_*): <data dir>/jit/<ROM name>.blk lists the blocks
 * the game compiled; they are compiled again between frames when their code is in RDRAM.
 * Development aid: <data dir>/norb.txt turns it off (no list read, recorded or used). */
void ctr64_rb_load2(const char *shipped, const char *path);
int ctr64_rb_save(const char *path);
int ctr64_rb_unsaved(void);
static bool g_rb_on;

static void rb_path(char *out, size_t n)
{
    snprintf(out, n, "%s/jit/%s.blk", H.data_dir, H.rom_id);
}

static void rb_load(void)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/norb.txt", H.data_dir);
    FILE *f = fopen(path, "r");
    g_rb_on = f == NULL;
    if (f)
        fclose(f);
    if (!g_rb_on)
        return;
    char shipped[300];
    rb_path(path, sizeof(path));
    /* lists that come with ctr64 or were downloaded (read only); the player's own list adds to it */
    snprintf(shipped, sizeof(shipped), "%s/jit/shipped/%s.blk", H.data_dir, H.rom_id);
    ctr64_rb_load2(shipped, path);
    if (H.log) { /* the profiler copies core.log: whether a game had its precompile list */
        extern int ctr64_rb_count(void);
        FILE *s = fopen(shipped, "rb");
        fprintf(H.log, "precompile list: %s, %d blocks in all (shipped %s)\n", H.rom_id, ctr64_rb_count(),
                s ? "found" : "missing");
        if (s)
            fclose(s);
        fflush(H.log);
    }
}

/* appends newly recorded blocks to the file (also called on leaving the game and opening the menu) */
void core_host_rb_save(void)
{
    char path[300];
    if (!g_rb_on || !H.loaded)
        return;
    rb_path(path, sizeof(path));
    ctr64_rb_save(path);
}

void core_host_flush_log(void)
{
    if (H.log)
        fflush(H.log);
}

/* once per VI. Saving used to happen here every ~300 VIs, but an SD write on the emulation
 * thread stalled the game for 0.1-2.7 s on hardware (HW test 3); new blocks are now written
 * only when the menu opens, the game is left, or the system suspends/sleeps/exits (apt_hook). */
void core_host_rb_tick(void)
{
}

/* Development aid: <data dir>/loadstate.st (a Mupen64Plus-Next save state, e.g. made by the
 * Windows build's CTR64_REF_SAVE) is loaded after the first frame; frame numbering for frame
 * dumps and input scripts then restarts at 0, as on the Windows side.
 *
 * A state is 16 MB, mostly RDRAM (8 MB at STATE_RDRAM) and the TLB lookup tables (2 x 4 MB at
 * STATE_LUTS), which does not fit the heap next to a loaded game. The patched core loader
 * (savestates.c) takes the state with those two ranges cut out and pulls them in through
 * ctr64_state_fill(), which streams them from the file straight into emulator memory. */
#define STATE_RDRAM 444u                                       /* header 44 + registers 400 */
#define STATE_RDRAM_SIZE 0x800000u
#define STATE_LUTS (STATE_RDRAM + STATE_RDRAM_SIZE + 0x2000 + 64 + 24) /* + SP mem, PIF, flashram */
#define STATE_LUTS_SIZE 0x800000u

static FILE *g_state_file;
static unsigned g_state_fills;

void ctr64_state_fill(void *dst, size_t bytes)
{
    /* called for RDRAM, LUT_r, LUT_w in that order; the LUTs are adjacent in the file */
    if (g_state_fills == 0)
        fseek(g_state_file, STATE_RDRAM, SEEK_SET);
    else if (g_state_fills == 1)
        fseek(g_state_file, STATE_LUTS, SEEK_SET);
    g_state_fills++;
    if (fread(dst, 1, bytes, g_state_file) != bytes && H.log)
        fprintf(H.log, "loadstate.st: short read\n");
}

static bool read_at(FILE *f, long at, void *dst, size_t n)
{
    return fseek(f, at, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
}

static bool load_state_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    const long mid = STATE_RDRAM + STATE_RDRAM_SIZE, tail = STATE_LUTS + STATE_LUTS_SIZE;
    unsigned char *buf = size > tail ? malloc(size - STATE_RDRAM_SIZE - STATE_LUTS_SIZE) : NULL;
    const char *result = "too small or no memory";
    if (buf && read_at(f, 0, buf, STATE_RDRAM) &&
        read_at(f, mid, buf + STATE_RDRAM, STATE_LUTS - mid) &&
        read_at(f, tail, buf + STATE_RDRAM + (STATE_LUTS - mid), size - tail)) {
        g_state_file = f;
        g_state_fills = 0;
        bool ok = retro_unserialize(buf, size - STATE_RDRAM_SIZE - STATE_LUTS_SIZE);
        result = !ok ? "FAILED" : g_state_fills != 3 ? "FAILED (unexpected layout)" : "loaded";
        g_state_file = NULL;
    }
    if (H.log) {
        fprintf(H.log, "%s: %s (%ld bytes)\n", path, result, size);
        fflush(H.log);
    }
    free(buf);
    fclose(f);
    return result[0] == 'l';
}

static void load_test_state(void)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/loadstate.st", H.data_dir);
    if (load_state_file(path)) {
        ctr64_gfx_drain();
        ctr64_video_frames = 0;
        ctr64_vi_count = 0;
    }
}

/* ---- save state slots -------------------------------------------------------------------------
 * <data dir>/states/<ROM name>.state<slot>: the uncompressed Mupen64Plus-Next state, the same
 * bytes the Windows build writes. The patched core streams the 16 MB of RDRAM and TLB tables
 * out through ctr64_state_put() instead of staging the whole state in memory. */
static FILE *g_put_file;
static bool g_put_ok;

void ctr64_state_put(const void *src, size_t bytes)
{
    if (g_put_file && bytes && fwrite(src, 1, bytes, g_put_file) != bytes)
        g_put_ok = false;
}

static void state_path(int slot, char *out, size_t n)
{
    snprintf(out, n, "%s/states/%s.state%d", H.data_dir, H.rom_base, slot);
}

bool core_host_state_exists(int slot)
{
    char path[400];
    struct stat st;
    state_path(slot, path, sizeof(path));
    return stat(path, &st) == 0 && st.st_size > (off_t)(STATE_LUTS + STATE_LUTS_SIZE);
}

bool core_host_save_state(int slot)
{
    char path[400], tmp[410];
    if (!H.loaded)
        return false;
    state_path(slot, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    g_put_file = fopen(tmp, "wb");
    if (!g_put_file)
        return false;
    static char vbuf[64 * 1024];
    setvbuf(g_put_file, vbuf, _IOFBF, sizeof(vbuf));
    g_put_ok = true;
    char dummy[4]; /* the core's own buffer is not used on this path */
    g_put_ok = retro_serialize(dummy, sizeof(dummy)) && g_put_ok;
    g_put_ok = fclose(g_put_file) == 0 && g_put_ok;
    g_put_file = NULL;
    if (g_put_ok) {
        remove(path);
        g_put_ok = rename(tmp, path) == 0;
    } else {
        remove(tmp);
    }
    prof_mark("save state %d", slot);
    if (H.log) {
        fprintf(H.log, "save state %d: %s\n", slot, g_put_ok ? "written" : "FAILED");
        fflush(H.log);
    }
    return g_put_ok;
}

bool core_host_load_state(int slot)
{
    char path[400];
    state_path(slot, path, sizeof(path));
    prof_mark("load state %d", slot);
    return H.loaded && load_state_file(path);
}

void core_host_reset(void)
{
    prof_mark("reset");
    if (H.loaded)
        retro_reset();
}

const char *core_host_rom_name(void)
{
    return H.rom_base;
}

void core_host_run_frame(void)
{
    static unsigned runs;
    if (!H.loaded)
        return;
    retro_run();
    if (++runs == 1)
        load_test_state();
    if (runs % 30 == 0)
        rom_stream_trim();
    if (runs % 60 == 0) {
        srm_flush(false);
        if (runs % 1800 == 0 && H.log && rom_stream_report()[0]) {
            fputs(rom_stream_report(), H.log);
            fputc('\n', H.log);
            fflush(H.log);
        }
    }
    if (g_poke_n)
        pokes_apply(ctr64_vi_count);
}

void core_host_unload(void)
{
    if (!H.loaded)
        return;
    srm_flush(true);
    core_host_rb_save();
    free(H.srm_shadow);
    H.srm_shadow = NULL;
    retro_unload_game();
    ctr64_gfx_async_stop();
    ctr64_audio_async_stop();
    audio_ndsp_exit();
    if (H.log && rom_stream_report()[0])
        fprintf(H.log, "%s\n", rom_stream_report());
    rom_stream_close();
    H.loaded = false;
}

void core_host_shutdown(void)
{
    core_host_unload();
    retro_deinit();
    if (H.log)
        fclose(H.log);
    H.log = NULL;
}

double core_host_fps(void)
{
    return H.av.timing.fps > 1 ? H.av.timing.fps : 60.0;
}

unsigned core_host_sample_rate(void)
{
    return (unsigned)(H.av.timing.sample_rate + 0.5);
}

void core_host_block_input(bool blocked)
{
    H.input_release = H.input_blocked && !blocked;
    H.input_blocked = blocked;
}
