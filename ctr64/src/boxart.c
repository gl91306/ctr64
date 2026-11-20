/* Game list box art: see boxart.h.
 *
 * Threads: the main thread draws (and owns the textures it shows); a loader thread reads cached
 * images from the SD card into a fixed pool of texture buffers, nearest to the selection first;
 * a download thread fetches missing art over Wi-Fi (libcurl + mbedTLS: the system's HTTP service
 * cannot talk to GitHub's TLS), decodes it (libpng), shrinks it and writes the cache file. Both
 * run below the main thread's priority on its core, so drawing never waits for them.
 *
 * Cache file <key>.bin: "C64A", u16 width, u16 height, u32 version, u32 0, then the 256x256 RGBA5551
 * texture in the GPU's tiled layout (8x8 tiles, Morton order inside), image row 0 in memory row 0
 * (which the GPU samples at v = 1: v = 0 is the last row).
 * <key>.none: no art exists (all candidate names were 404); delete it to try again. */
#include "boxart.h"

#include <3ds.h>
#include <curl/curl.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <malloc.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "net_roots.h"
#include "pica/pica.h"
#include "ui/boxart_names.h"
#include "ui/ui.h"
#include "video.h"

#define ART_DIR "sdmc:/3ds/ctr64/boxart"
#define URL_BASE "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Nintendo_64/master/Named_Boxarts/"
#define TEX 256
#define TEX_BYTES (TEX * TEX * 2)
#define MAX_GAMES 200
#define KEEP 4                    /* textures kept on each side of the selection */
#define POOL (2 * KEEP + 3)       /* texture buffers: the kept ones plus room to load ahead */
#define LABELS 12                 /* title cards (A8 256x32) */
#define LABEL_W 256
#define LABEL_H 32
#define TITLE_W 512               /* the selected game's title under the carousel (A8 512x32) */
#define TITLE_H 32
#define MAX_DOWNLOAD (4u << 20)
#define SOC_BYTES 0x100000
#define FILE_MAGIC 0x41343643u    /* "C64A" */
#define FILE_VERSION 1

enum { SD_UNKNOWN, SD_FILE, SD_NOFILE, SD_NONE };

typedef struct {
    char key[64];         /* cache file name */
    char names[4][100];   /* candidate libretro names, in order */
    int nnames;
    char title[80];
    volatile int sd;      /* SD_* (written by the download/loader threads) */
    bool tried;           /* download attempted this session (download thread only) */
    int tex;              /* pool slot, -1 = not loaded */
    bool loading;
    u16 w, h;             /* image size in the texture */
    float in;             /* fade-in 0..1 once loaded */
    int label;            /* label slot, -1 = none */
} Art;

static struct {
    Art art[MAX_GAMES];
    int n;
    volatile int sel;
    float pos;            /* carousel position (animated towards sel) */
    LightLock lock;       /* art[].tex/loading/w/h, pool_used */
    u16 *pool[POOL];
    bool pool_used[POOL];
    int pool_free_after[POOL]; /* main thread: slots given back, reusable once the GPU is done */
    u8 *label_tex[LABELS];
    int label_game[LABELS];
    u8 *title_tex[2];
    int title_game[2];
    int title_cur;
    volatile bool quit;
    volatile bool dirty;  /* a texture arrived: draw again */
    bool drawn_once;
    Thread loader, downloader;
    LightEvent wake;      /* selection changed / new file: wake the threads */
    float proj[4][4];
    bool active;
} B;

/* ---- names ------------------------------------------------------------------------------ */

static const char *table_name(const char *id)
{
    int lo = 0, hi = (int)(sizeof(k_boxart_ids) / sizeof(k_boxart_ids[0])) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int c = memcmp(k_boxart_ids[mid].id, id, 4);
        if (!c)
            return k_boxart_pool + k_boxart_ids[mid].name;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static void add_name(Art *a, const char *n)
{
    if (!n || !n[0] || a->nnames >= 4)
        return;
    for (int i = 0; i < a->nnames; i++)
        if (!strcmp(a->names[i], n))
            return;
    snprintf(a->names[a->nnames++], sizeof(a->names[0]), "%s", n);
}

/* libretro thumbnail names: these characters become _ */
static void thumb_safe(char *s)
{
    for (; *s; s++)
        if (strchr("&*/:`<>?\\|\"", *s))
            *s = '_';
}

void boxart_begin(void)
{
    memset(&B, 0, sizeof(B));
    LightLock_Init(&B.lock);
    LightEvent_Init(&B.wake, RESET_ONESHOT);
    for (int i = 0; i < LABELS; i++)
        B.label_game[i] = -1;
    B.title_game[0] = B.title_game[1] = -1;
}

void boxart_add(const char *path, const char *title, const char *id, bool hack)
{
    if (B.n >= MAX_GAMES)
        return;
    Art *a = &B.art[B.n++];
    a->tex = a->label = -1;
    snprintf(a->title, sizeof(a->title), "%s", title);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char stem[100];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot)
        *dot = 0;
    thumb_safe(stem);
    if (id && id[0] && !hack) {
        snprintf(a->key, sizeof(a->key), "%s", id);
        add_name(a, table_name(id));
        /* the same game in another region (translation patches change the region letter) */
        static const char regions[] = "EPJUDFIS";
        for (const char *r = regions; *r; r++) {
            char alt[5] = {id[0], id[1], id[2], *r, 0};
            if (*r != id[3])
                add_name(a, table_name(alt));
        }
    } else {
        snprintf(a->key, sizeof(a->key), "f_%.60s", stem);
    }
    add_name(a, stem);
    for (char *k = a->key; *k; k++) /* FAT file names */
        if (strchr("\\/:*?\"<>|", *k))
            *k = '_';
}

/* ---- images ----------------------------------------------------------------------------- */

static inline u32 morton8(u32 x, u32 y)
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static inline u32 tiled(u32 x, u32 y, u32 tw)
{
    return ((y >> 3) * (tw >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7);
}

/* weights of source pixels for one destination pixel of an axis scaled by s (dst = src * s):
 * box filter over the source interval it covers (at least one source pixel when enlarging) */
static void resample_axis(const float *src, int sn, int sstride, float *dst, int dn, int dstride, int ch)
{
    const float inv = (float)sn / dn;
    for (int d = 0; d < dn; d++) {
        float a = d * inv, b = (d + 1) * inv;
        if (b - a < 1.0f) { /* enlarging: one pixel wide around the centre */
            const float c = (a + b) * 0.5f;
            a = c - 0.5f;
            b = c + 0.5f;
        }
        if (a < 0)
            a = 0;
        if (b > sn)
            b = (float)sn;
        float acc[4] = {0, 0, 0, 0}, wsum = 0;
        for (int s = (int)a; s < sn && s < b; s++) {
            const float lo = s < a ? a : (float)s, hi = s + 1 > b ? b : (float)(s + 1);
            const float w = hi - lo;
            if (w <= 0)
                continue;
            const float *p = src + (size_t)s * sstride;
            for (int k = 0; k < ch; k++)
                acc[k] += p[k] * w;
            wsum += w;
        }
        float *q = dst + (size_t)d * dstride;
        for (int k = 0; k < ch; k++)
            q[k] = wsum > 0 ? acc[k] / wsum : 0;
    }
}

/* RGBA8 (w x h) -> the cache file at path; false if out of memory or the write failed */
static bool save_image(const u8 *rgba, int w, int h, const char *path)
{
    const float s = fminf((float)TEX / w, (float)TEX / h);
    int dw = (int)lroundf(w * s), dh = (int)lroundf(h * s);
    dw = dw < 1 ? 1 : dw > TEX ? TEX : dw;
    dh = dh < 1 ? 1 : dh > TEX ? TEX : dh;
    float *src = malloc((size_t)w * 4 * sizeof(float)); /* one source row */
    float *mid = malloc((size_t)dw * h * 4 * sizeof(float));
    float *dst = malloc((size_t)dw * dh * 4 * sizeof(float));
    u16 *tex = calloc(1, TEX_BYTES);
    bool ok = false;
    if (src && mid && dst && tex) {
        /* premultiplied alpha, so transparent pixels do not darken the edges */
        for (int y = 0; y < h; y++) {
            const u8 *row = rgba + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                const float al = row[x * 4 + 3] / 255.0f;
                src[x * 4 + 0] = row[x * 4 + 0] * al;
                src[x * 4 + 1] = row[x * 4 + 1] * al;
                src[x * 4 + 2] = row[x * 4 + 2] * al;
                src[x * 4 + 3] = al;
            }
            resample_axis(src, w, 4, mid + (size_t)y * dw * 4, dw, 4, 4);
        }
        for (int x = 0; x < dw; x++)
            resample_axis(mid + (size_t)x * 4, h, dw * 4, dst + (size_t)x * 4, dh, dw * 4, 4);
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                const float *p = dst + ((size_t)y * dw + x) * 4;
                const float al = p[3];
                u16 v = 0;
                if (al >= 0.5f) {
                    int r = (int)(p[0] / al + 0.5f), g = (int)(p[1] / al + 0.5f), b = (int)(p[2] / al + 0.5f);
                    r = r > 255 ? 255 : r;
                    g = g > 255 ? 255 : g;
                    b = b > 255 ? 255 : b;
                    v = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
                }
                tex[tiled(x, y, TEX)] = v;
            }
        char tmp[200];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            const u32 hdr[4] = {FILE_MAGIC, (u32)dw | ((u32)dh << 16), FILE_VERSION, 0};
            ok = fwrite(hdr, sizeof(hdr), 1, f) == 1 && fwrite(tex, TEX_BYTES, 1, f) == 1;
            ok = fclose(f) == 0 && ok;
            remove(path);
            ok = ok && rename(tmp, path) == 0;
            if (!ok)
                remove(tmp);
        }
    }
    free(src);
    free(mid);
    free(dst);
    free(tex);
    return ok;
}

/* ---- download thread -------------------------------------------------------------------- */

typedef struct {
    u8 *p;
    size_t n, cap;
} Buf;

static size_t on_data(char *p, size_t s, size_t n, void *user)
{
    Buf *b = user;
    const size_t k = s * n;
    if (b->n + k > MAX_DOWNLOAD)
        return 0;
    if (b->n + k > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 512 * 1024;
        while (cap < b->n + k)
            cap *= 2;
        u8 *q = realloc(b->p, cap);
        if (!q)
            return 0;
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, k);
    b->n += k;
    return k;
}

static int on_progress(void *user, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
    return B.quit; /* non-zero aborts the transfer */
}

static void url_for(char *out, size_t n, const char *name)
{
    size_t k = snprintf(out, n, "%s", URL_BASE);
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *s = (const unsigned char *)name; *s && k + 4 < n; s++) {
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || strchr("-_.~", *s))
            out[k++] = (char)*s;
        else {
            out[k++] = '%';
            out[k++] = hex[*s >> 4];
            out[k++] = hex[*s & 15];
        }
    }
    snprintf(out + k, n - k, ".png");
}

/* boxart.log: what the download thread did (Wi-Fi, network, each request), kept short */
static int g_log_lines;
static char g_curl_err[CURL_ERROR_SIZE];
static void blog(const char *fmt, ...)
{
    if (g_log_lines++ > 400)
        return;
    FILE *f = fopen(ART_DIR "/boxart.log", g_log_lines == 1 ? "w" : "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Certificates are checked against the roots and the host name, but not their dates: a 3DS clock
 * set to the wrong day (seen on hardware: every certificate "not valid yet") must not stop box
 * art. mbedTLS calls this for each certificate of the chain; curl runs on_ssl_ctx on its config. */
static int on_verify(void *user, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_EXPIRED);
    return 0;
}

static CURLcode on_ssl_ctx(CURL *c, void *ssl_config, void *user)
{
    mbedtls_ssl_conf_verify((mbedtls_ssl_config *)ssl_config, on_verify, NULL);
    return CURLE_OK;
}

/* 200: body in b; other HTTP codes as returned; -1: network error or quit */
static long fetch(CURL *c, const char *name, Buf *b)
{
    char url[512];
    url_for(url, sizeof(url), name);
    b->n = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
    const CURLcode r = curl_easy_perform(c);
    long code = -1;
    if (r == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    blog("%s: %s, HTTP %ld, %lu bytes%s%s", name, curl_easy_strerror(r), code, (unsigned long)b->n,
         r != CURLE_OK && g_curl_err[0] ? ": " : "", r != CURLE_OK ? g_curl_err : "");
    g_curl_err[0] = 0;
    return code;
}

/* one game: true when done (art saved, or none exists), false on a network error */
static bool download_one(CURL *c, Art *a, Buf *b)
{
    char path[160];
    for (int i = 0; i < a->nnames && !B.quit; i++) {
        long code = fetch(c, a->names[i], b);
        if (code == 200 && b->n > 8 && b->n < 300 && memcmp(b->p, "\x89PNG", 4)) {
            /* a git symlink: GitHub serves the link text (the target's file name) */
            char target[300];
            memcpy(target, b->p, b->n);
            target[b->n] = 0;
            target[strcspn(target, "\r\n")] = 0;
            char *png = strstr(target, ".png");
            if (!png)
                continue;
            *png = 0;
            code = fetch(c, target, b);
        }
        if (code == 404)
            continue;
        if (code != 200)
            return false;
        png_image im;
        memset(&im, 0, sizeof(im));
        im.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_memory(&im, b->p, b->n))
            continue;
        if (im.width == 0 || im.height == 0 || im.width > 4096 || im.height > 4096) {
            png_image_free(&im);
            continue;
        }
        im.format = PNG_FORMAT_RGBA;
        u8 *rgba = malloc(PNG_IMAGE_SIZE(im));
        bool ok = rgba && png_image_finish_read(&im, NULL, rgba, 0, NULL);
        png_image_free(&im);
        if (ok) {
            snprintf(path, sizeof(path), ART_DIR "/%s.bin", a->key);
            ok = save_image(rgba, (int)im.width, (int)im.height, path);
        }
        free(rgba);
        if (ok) {
            a->sd = SD_FILE;
            B.dirty = true;
            LightEvent_Signal(&B.wake);
            return true;
        }
        return false; /* out of memory or SD full: try again next time */
    }
    if (B.quit)
        return false;
    snprintf(path, sizeof(path), ART_DIR "/%s.none", a->key);
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
    a->sd = SD_NONE;
    return true;
}

static void sleep_ms(int ms)
{
    for (int t = 0; t < ms && !B.quit; t += 50)
        svcSleepThread(50 * 1000000LL);
}

static void downloader_main(void *arg)
{
    u32 *soc_buf = NULL;
    bool ac = false, soc = false, curl_ok = false;
    CURL *c = NULL;
    Buf b = {0};
    while (!B.quit) {
        /* the missing art nearest to the selection */
        int best = -1, bestd = 1 << 30;
        for (int i = 0; i < B.n; i++) {
            const Art *a = &B.art[i];
            if (a->sd != SD_NOFILE || a->tried)
                continue;
            const int d = abs(i - B.sel);
            if (d < bestd) {
                bestd = d;
                best = i;
            }
        }
        if (best < 0)
            break; /* nothing left to fetch this session */
        if (!c) {
            if (!ac) {
                const Result r = acInit();
                ac = R_SUCCEEDED(r);
                if (!ac)
                    blog("acInit failed: %08lx", (unsigned long)r);
            }
            u32 wifi = 0;
            const Result wr = ac ? ACU_GetWifiStatus(&wifi) : 0;
            static u32 last_wifi = 0xFFFFFFFF;
            if (wifi != last_wifi || R_FAILED(wr))
                blog("Wi-Fi status %lu (%08lx)", (unsigned long)wifi, (unsigned long)wr);
            last_wifi = wifi;
            if (!ac || R_FAILED(wr) || wifi == 0) {
                sleep_ms(3000); /* no Wi-Fi (yet) */
                continue;
            }
            if (!soc) {
                soc_buf = memalign(0x1000, SOC_BYTES);
                const Result sr = soc_buf ? socInit(soc_buf, SOC_BYTES) : -1;
                soc = R_SUCCEEDED(sr);
                blog("socInit %08lx", (unsigned long)sr);
                if (!soc) {
                    free(soc_buf);
                    soc_buf = NULL;
                    break;
                }
            }
            if (!curl_ok)
                curl_ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
            c = curl_ok ? curl_easy_init() : NULL;
            blog("curl %s: %s", curl_version(), c ? "ready" : "init failed");
            if (!c)
                break;
            curl_easy_setopt(c, CURLOPT_ERRORBUFFER, g_curl_err);
            curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, on_ssl_ctx);
            {
                const time_t now = time(NULL);
                struct tm tm;
                gmtime_r(&now, &tm);
                blog("console clock %04d-%02d-%02d %02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min);
            }
            struct curl_blob roots = {(void *)k_net_roots_pem, sizeof(k_net_roots_pem), CURL_BLOB_NOCOPY};
            curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &roots);
            curl_easy_setopt(c, CURLOPT_USERAGENT, "ctr64/" CTR64_VERSION);
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
            curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
            curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
        }
        Art *a = &B.art[best];
        a->tried = true;
        if (!download_one(c, a, &b))
            sleep_ms(1000); /* network trouble: the next game after a pause (this one next session) */
    }
    if (c)
        curl_easy_cleanup(c);
    free(b.p);
    if (curl_ok)
        curl_global_cleanup();
    if (soc)
        socExit();
    free(soc_buf);
    if (ac)
        acExit();
}

/* ---- loader thread ---------------------------------------------------------------------- */

static int pool_take(void)
{
    for (int i = 0; i < POOL; i++)
        if (!B.pool_used[i] && B.pool_free_after[i] == 0) {
            B.pool_used[i] = true;
            return i;
        }
    return -1;
}

static void loader_main(void *arg)
{
    char path[160];
    while (!B.quit) {
        int best = -1, slot = -1;
        LightLock_Lock(&B.lock);
        const int sel = B.sel;
        for (int d = 0; d <= KEEP && best < 0; d++)
            for (int s = -1; s <= 1 && best < 0; s += 2) {
                const int i = sel + d * s;
                if (i < 0 || i >= B.n || (d == 0 && s > 0))
                    continue;
                Art *a = &B.art[i];
                if (a->sd == SD_FILE && a->tex < 0 && !a->loading)
                    best = i;
            }
        if (best >= 0 && (slot = pool_take()) >= 0)
            B.art[best].loading = true;
        LightLock_Unlock(&B.lock);
        if (best < 0 || slot < 0) {
            LightEvent_WaitTimeout(&B.wake, 100 * 1000000LL);
            continue;
        }
        Art *a = &B.art[best];
        snprintf(path, sizeof(path), ART_DIR "/%s.bin", a->key);
        FILE *f = fopen(path, "rb");
        u32 hdr[4] = {0};
        bool ok = f && fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == FILE_MAGIC && hdr[2] == FILE_VERSION &&
                  fread(B.pool[slot], TEX_BYTES, 1, f) == 1;
        if (f)
            fclose(f);
        const u16 w = hdr[1] & 0xFFFF, h = hdr[1] >> 16;
        ok = ok && w >= 1 && w <= TEX && h >= 1 && h <= TEX;
        if (ok)
            GSPGPU_FlushDataCache(B.pool[slot], TEX_BYTES);
        LightLock_Lock(&B.lock);
        a->loading = false;
        if (ok) {
            a->tex = slot;
            a->w = w;
            a->h = h;
            a->in = 0;
        } else {
            B.pool_used[slot] = false;
            remove(path);
            a->sd = SD_NOFILE; /* bad file: download again */
        }
        LightLock_Unlock(&B.lock);
        B.dirty = true;
    }
}

/* ---- start / stop ----------------------------------------------------------------------- */

void boxart_start(void)
{
    B.active = true;
    mkdir(ART_DIR, 0777);
    char path[160];
    struct stat st;
    for (int i = 0; i < B.n; i++) {
        Art *a = &B.art[i];
        snprintf(path, sizeof(path), ART_DIR "/%s.bin", a->key);
        if (stat(path, &st) == 0)
            a->sd = SD_FILE;
        else {
            snprintf(path, sizeof(path), ART_DIR "/%s.none", a->key);
            a->sd = stat(path, &st) == 0 ? SD_NONE : SD_NOFILE;
        }
    }
    for (int i = 0; i < POOL; i++)
        B.pool[i] = linearMemAlign(TEX_BYTES, 128);
    for (int i = 0; i < LABELS; i++)
        B.label_tex[i] = linearMemAlign(LABEL_W * LABEL_H, 128);
    B.title_tex[0] = linearMemAlign(TITLE_W * TITLE_H, 128);
    B.title_tex[1] = linearMemAlign(TITLE_W * TITLE_H, 128);
    for (int i = 0; i < POOL; i++)
        if (!B.pool[i])
            B.pool_used[i] = true; /* never handed out */
    pica_mtx_ortho_tilt(B.proj, 0, 400, 240, 0, 0.0f, 1.0f);
    B.pos = (float)B.sel;
    B.dirty = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    B.loader = threadCreate(loader_main, NULL, 16 * 1024, prio + 1, -2, false);
    if (B.n)
        B.downloader = threadCreate(downloader_main, NULL, 128 * 1024, prio + 2, -2, false); /* TLS */
}

void boxart_redraw(void)
{
    B.dirty = true;
}

void boxart_select(int index)
{
    if (index == B.sel)
        return;
    LightLock_Lock(&B.lock);
    B.sel = index;
    LightLock_Unlock(&B.lock);
    /* a jump across the list (wrap-around): start close to the target instead of flying past */
    if (fabsf(B.pos - index) > 3.0f)
        B.pos = index + (B.pos < index ? -3.0f : 3.0f);
    LightEvent_Signal(&B.wake);
}

void boxart_end(void)
{
    if (!B.active)
        return;
    B.quit = true;
    LightEvent_Signal(&B.wake);
    if (B.loader) {
        threadJoin(B.loader, 3000000000LL);
        threadFree(B.loader);
    }
    if (B.downloader) {
        /* curl gives up within a second (progress callback); a DNS lookup may take longer */
        if (R_SUCCEEDED(threadJoin(B.downloader, 8000000000LL)))
            threadFree(B.downloader);
    }
    pica_wait_idle();
    for (int i = 0; i < POOL; i++)
        if (B.pool[i])
            linearFree(B.pool[i]);
    for (int i = 0; i < LABELS; i++)
        if (B.label_tex[i])
            linearFree(B.label_tex[i]);
    for (int i = 0; i < 2; i++)
        if (B.title_tex[i])
            linearFree(B.title_tex[i]);
    B.active = false;
}

/* ---- drawing ---------------------------------------------------------------------------- */

static u32 rgb_of(UiCol c)
{
    const u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
}

static u32 scale_rgb(u32 rgb, float k)
{
    const u32 r = (u32)(((rgb >> 16) & 255) * k), g = (u32)(((rgb >> 8) & 255) * k), b = (u32)((rgb & 255) * k);
    return r << 16 | g << 8 | b;
}

static void vtx(VideoVertex *v, float x, float y, u32 rgb, u8 a, float u, float t)
{
    v->x = x;
    v->y = y;
    v->z = -0.5f;
    v->r = rgb >> 16;
    v->g = rgb >> 8;
    v->b = rgb;
    v->a = a;
    v->u = u;
    v->v = t;
}

/* a quad with a colour/alpha per edge (top, bottom), texture coordinates u0..u1, v0..v1 */
static void quad(float x0, float y0, float x1, float y1, u32 rgb_top, u8 a_top, u32 rgb_bot, u8 a_bot,
                 float u0, float v0, float u1, float v1)
{
    VideoBatch b;
    if (!video_batch_begin(&b, 6))
        return;
    vtx(&b.v[0], x0, y0, rgb_top, a_top, u0, v0);
    vtx(&b.v[1], x0, y1, rgb_bot, a_bot, u0, v1);
    vtx(&b.v[2], x1, y1, rgb_bot, a_bot, u1, v1);
    vtx(&b.v[3], x0, y0, rgb_top, a_top, u0, v0);
    vtx(&b.v[4], x1, y1, rgb_bot, a_bot, u1, v1);
    vtx(&b.v[5], x1, y0, rgb_top, a_top, u1, v0);
    b.n = 6;
    video_batch_draw(&b);
}

enum { MODE_SOLID, MODE_TEX, MODE_MASK };

static void mode(int m)
{
    PicaTev tev;
    pica_tev_default(&tev);
    if (m == MODE_SOLID) {
        tev.src_rgb = GPU_TEVSOURCES(GPU_PRIMARY_COLOR, 0, 0);
        tev.src_alpha = GPU_TEVSOURCES(GPU_PRIMARY_COLOR, 0, 0);
    } else if (m == MODE_TEX) {
        tev.src_rgb = GPU_TEVSOURCES(GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        tev.src_alpha = GPU_TEVSOURCES(GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        tev.func_rgb = GPU_MODULATE;
        tev.func_alpha = GPU_MODULATE;
    } else { /* text coverage (A8) in the vertex colour */
        tev.src_rgb = GPU_TEVSOURCES(GPU_PRIMARY_COLOR, 0, 0);
        tev.src_alpha = GPU_TEVSOURCES(GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        tev.func_alpha = GPU_MODULATE;
    }
    pica_tev(0, &tev);
    pica_tev_default(&tev);
    for (int i = 1; i < 6; i++)
        pica_tev(i, &tev);
}

static void tex_bind(const void *tex, u16 w, u16 h, GPU_TEXCOLOR fmt)
{
    pica_texture(0, tex, w, h, fmt,
                 GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                     GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE));
}

/* coverage (row-major) -> A8 texture, tiled */
static void upload_mask(u8 *tex, const u8 *mask, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            tex[tiled(x, y, w)] = mask[y * w + x];
    GSPGPU_FlushDataCache(tex, w * h);
}

/* the title card texture of game i (made on first use; at most one per frame) */
static int label_for(int i, bool *made)
{
    Art *a = &B.art[i];
    if (a->label >= 0 && B.label_game[a->label] == i)
        return a->label;
    if (*made)
        return -1;
    int slot = -1, far = -1;
    for (int k = 0; k < LABELS; k++) {
        if (!B.label_tex[k])
            continue;
        const int g = B.label_game[k];
        const int d = g < 0 ? 1 << 20 : abs(g - B.sel);
        if (d > far) {
            far = d;
            slot = k;
        }
    }
    if (slot < 0)
        return -1;
    static u8 mask[LABEL_W * LABEL_H];
    if (B.drawn_once)
        pica_wait_idle(); /* the slot may be in the last frame, still on the GPU */
    ui_text_mask(UI_TITLE, a->title, LABEL_W - 16, mask, LABEL_W, LABEL_H);
    upload_mask(B.label_tex[slot], mask, LABEL_W, LABEL_H);
    if (B.label_game[slot] >= 0)
        B.art[B.label_game[slot]].label = -1;
    B.label_game[slot] = i;
    a->label = slot;
    *made = true;
    return slot;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* horizontal centre of the box at distance d from the focus */
static float box_x(float d)
{
    const float a = fabsf(d), s1 = 168.0f, s2 = 96.0f;
    const float x = a < 1.0f ? a * s1 : s1 + (a - 1.0f) * s2;
    return 200.0f + (d < 0 ? -x : x);
}

/* the boxes around the position, far ones first (the focused one is drawn last, on top) */
static int visible(int *order)
{
    const int c = (int)lroundf(B.pos);
    int n = 0;
    for (int d = 3; d >= 0; d--) {
        if (c - d >= 0 && c - d < B.n)
            order[n++] = c - d;
        if (d && c + d >= 0 && c + d < B.n)
            order[n++] = c + d;
    }
    return n;
}

void boxart_frame(void)
{
    if (!B.active)
        return;
    /* animation: ease towards the selection */
    const float target = (float)B.sel;
    const float diff = target - B.pos;
    const bool moving = fabsf(diff) > 0.001f;
    B.pos = moving ? B.pos + diff * 0.2f : target;
    bool fading = false, freed = false;
    LightLock_Lock(&B.lock);
    /* slots given back are reusable a few VBlanks later, when the GPU has finished with them */
    for (int i = 0; i < POOL; i++)
        if (B.pool_free_after[i] && ++B.pool_free_after[i] > 3)
            B.pool_free_after[i] = 0;
    /* textures far from the selection go back to the pool */
    for (int i = 0; i < B.n; i++) {
        Art *a = &B.art[i];
        if (a->tex >= 0 && abs(i - B.sel) > KEEP + 1 && fabsf(i - B.pos) > 3.5f) {
            B.pool_free_after[a->tex] = 1;
            B.pool_used[a->tex] = false;
            a->tex = -1;
            freed = true;
        }
        if (a->tex >= 0 && a->in < 1.0f)
            fading = true;
    }
    LightLock_Unlock(&B.lock);
    const bool dirty = B.dirty;
    if (!moving && !fading && !dirty && B.drawn_once && !freed)
        return;
    B.dirty = false;

    int order[8];
    const int no = visible(order);
    /* textures made on the CPU first (a title card, the title), outside the GPU frame */
    bool made = false;
    for (int k = 0; k < no; k++)
        if (B.art[order[k]].tex < 0 && fabsf(order[k] - B.pos) <= 3.2f && label_for(order[k], &made) < 0)
            B.dirty = true; /* one title card per frame: draw again for the next */
    const int near = (int)lroundf(B.pos);
    int title = -1;
    if (near >= 0 && near < B.n && B.title_tex[0] && B.title_tex[1]) {
        title = B.title_game[B.title_cur] == near ? B.title_cur : -1;
        if (title < 0) {
            title = B.title_cur ^ 1;
            static u8 mask[UI_W * TITLE_H], wide[TITLE_W * TITLE_H];
            if (B.drawn_once)
                pica_wait_idle();
            ui_text_mask(UI_TITLE, B.art[near].title, 300, mask, UI_W, TITLE_H);
            /* the mask is UI_W wide: centred in the 512-wide texture */
            memset(wide, 0, sizeof(wide));
            for (int y = 0; y < TITLE_H; y++)
                memcpy(&wide[y * TITLE_W + (TITLE_W - UI_W) / 2], &mask[y * UI_W], UI_W);
            upload_mask(B.title_tex[title], wide, TITLE_W, TITLE_H);
            B.title_game[title] = near;
            B.title_cur = title;
            made = true;
        }
    }

    const bool dark = ui_is_dark();
    const u32 bg_top = rgb_of(UI_BG_TOP), bg_bot = rgb_of(UI_BG_BOT);
    const u32 card = rgb_of(UI_CARD), card_bot = rgb_of(UI_CARD_BOT), text = rgb_of(UI_TEXT);
    const u32 sel_rgb = rgb_of(UI_SEL);
    const float floor_y = 176.0f, h0 = 150.0f;

    pica_frame_begin();
    const PicaTarget *scr = video_screen();
    pica_bind_target(scr);
    pica_viewport(0, 0, scr->width, scr->height);
    pica_scissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    pica_depth_test(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    pica_depth_map(-1.0f, 0.0f);
    pica_cull(GPU_CULL_NONE);
    pica_alpha_test(false, GPU_ALWAYS, 0);
    pica_blend_off();
    video_use_program(B.proj);
    for (int unit = 1; unit < 3; unit++)
        pica_texture(unit, NULL, 0, 0, GPU_RGBA8, 0);
    if (made || dirty || freed)
        pica_texture_flush_cache();

    /* background: the theme's gradient, a darker floor under the boxes */
    mode(MODE_SOLID);
    pica_texture(0, NULL, 0, 0, GPU_RGBA8, 0);
    quad(0, 0, 400, floor_y, bg_top, 255, bg_bot, 255, 0, 0, 0, 0);
    const u32 floor_rgb = dark ? 0x0B0C0F : bg_bot;
    quad(0, floor_y, 400, 240, floor_rgb, 255, bg_bot, 255, 0, 0, 0, 0);
    pica_blend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);

    for (int k = 0; k < no; k++) {
        const int i = order[k];
        Art *a = &B.art[i];
        const float d = i - B.pos, ad = fabsf(d);
        if (ad > 3.2f)
            continue;
        const float h = h0 * powf(0.72f, ad);
        const float aspect = a->tex >= 0 ? (float)a->w / a->h : 1.38f;
        float bw = h * aspect, bh = h;
        if (bw > h * 1.5f) { /* very wide art: fit the width */
            bw = h * 1.5f;
            bh = bw / aspect;
        }
        const float cx = box_x(d);
        const float x0 = cx - bw / 2, x1 = cx + bw / 2, y0 = floor_y - bh, y1 = floor_y;
        const float bright = 1.0f - 0.42f * clampf(ad, 0, 1) - 0.18f * clampf(ad - 1, 0, 1);
        const float edge = clampf(3.2f - ad, 0, 1); /* fade out towards the screen edges */
        const u32 shade = (u32)(255 * bright);
        const u32 tint = shade << 16 | shade << 8 | shade;
        const u8 alpha = (u8)(255 * edge);
        const float focus = clampf(1.0f - ad * 2.0f, 0, 1);
        if (focus > 0) { /* selection frame */
            mode(MODE_SOLID);
            pica_texture(0, NULL, 0, 0, GPU_RGBA8, 0);
            const u8 fa = (u8)(255 * focus);
            quad(x0 - 3, y0 - 3, x1 + 3, y1 + 3, sel_rgb, fa, sel_rgb, fa, 0, 0, 0, 0);
        }
        if (a->tex >= 0) {
            const float in = a->in;
            if (a->in < 1.0f)
                a->in = clampf(a->in + 0.12f, 0, 1);
            if (in < 1.0f) { /* the card under the art while it fades in */
                mode(MODE_SOLID);
                pica_texture(0, NULL, 0, 0, GPU_RGBA8, 0);
                quad(x0, y0, x1, y1, card, alpha, card_bot, alpha, 0, 0, 0, 0);
            }
            mode(MODE_TEX);
            tex_bind(B.pool[a->tex], TEX, TEX, GPU_RGBA5551);
            const float u1 = (float)a->w / TEX, v1 = (float)a->h / TEX;
            const u8 al = (u8)(alpha * in);
            quad(x0, y0, x1, y1, tint, al, tint, al, 0, 1.0f, u1, 1.0f - v1); /* memory row 0 is v = 1 */
            /* reflection on the floor: the bottom of the box, mirrored, fading out */
            const float rh = bh * 0.22f;
            const u8 ra = (u8)(al * (dark ? 0.30f : 0.22f));
            quad(x0, y1 + 2, x1, y1 + 2 + rh, tint, ra, tint, 0, 0, 1.0f - v1, u1, 1.0f - v1 * (1.0f - 0.22f));
        } else {
            /* no art (yet): a plain dark box with the title, in both themes */
            mode(MODE_SOLID);
            pica_texture(0, NULL, 0, 0, GPU_RGBA8, 0);
            const u32 edge_c = scale_rgb(0x5A6270, bright), top_c = scale_rgb(0x3A4250, bright);
            const u32 bot_c = scale_rgb(0x1E232B, bright);
            quad(x0, y0, x1, y1, edge_c, alpha, edge_c, alpha, 0, 0, 0, 0);
            quad(x0 + 1, y0 + 1, x1 - 1, y1 - 1, top_c, alpha, bot_c, alpha, 0, 0, 0, 0);
            const int l = a->label >= 0 && B.label_game[a->label] == i ? a->label : -1;
            if (l >= 0) {
                mode(MODE_MASK);
                tex_bind(B.label_tex[l], LABEL_W, LABEL_H, GPU_A8);
                const float lw = bw - 10, lh = lw * LABEL_H / LABEL_W, ly = (y0 + y1) / 2 - lh / 2;
                quad(cx - lw / 2, ly, cx + lw / 2, ly + lh, tint, alpha, tint, alpha, 0, 1, 1, 0);
            }
        }
    }

    /* the title of the game at the centre, faded out while moving between two games */
    if (title >= 0) {
        const float ta = clampf(1.0f - fabsf(B.pos - near) * 2.2f, 0, 1);
        mode(MODE_MASK);
        tex_bind(B.title_tex[title], TITLE_W, TITLE_H, GPU_A8);
        const u8 al = (u8)(255 * ta);
        quad(200 - TITLE_W / 2, 204, 200 + TITLE_W / 2, 204 + TITLE_H, text, al, text, al, 0, 1, 1, 0);
    }
    pica_blend_off();
    video_present();
    B.drawn_once = true;
}
