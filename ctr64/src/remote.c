/* Remote test mode (development): with <data dir>/remote.txt on the SD card, ctr64 starts as a small
 * TCP server instead of showing the game list, so tests can be run from a PC without anyone at the
 * console (tools/remote/ctr64remote.py). One client at a time, one line per command:
 *
 *   PING                       -> OK ctr64 remote
 *   PUT <size> <path>\n<bytes> -> OK                 (parent directories are created)
 *   GET <path>                 -> OK <size>\n<bytes>
 *   LS <dir>                   -> OK <n>\n then n lines "<size or -1 for a directory> <name>"
 *   RM <path>                  -> OK
 *   INSTALL <size>\n<bytes>    -> OK                 (a CIA, installed with AM; then ctr64 restarts)
 *   RUN <rom path>             -> OK                 (the game starts with the files on the SD card:
 *                                                     loadstate.st, inputs.txt, exitafter.txt, ...;
 *                                                     when it ends ctr64 restarts into this mode)
 *   RELAUNCH                   -> OK                 (ctr64 restarts)
 *   QUIT                       -> OK                 (ctr64 exits remote mode: the game list)
 * While a remote run's game is paused (the SELECT menu is open), port 5050 listens again for:
 *   PING, PUT, GET, LS, RM     as above
 *   STATE <slot>               -> OK <path>          (state saved, as the menu's Save does)
 *   SHOT                       -> OK <top> <bottom>\n<bytes> (top: BGR8 240x400, bottom: RGB565
 *                                                     240x320, both as the LCDs scan them)
 * Errors answer "ERR <reason>". Paths are absolute SD paths (sdmc:/...). Nothing here runs unless
 * remote.txt exists. */
#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "remote.h"
#include "core_host.h"
#include "pica/pica.h"
#include "ui/ui.h"

#define SOC_BUF_SIZE 0x100000
#define CHUNK (64 * 1024)

static u32 *g_soc_buf;
static int g_listen = -1;

static int send_all(int s, const void *p, size_t n)
{
    const char *c = (const char *)p;
    while (n) {
        const ssize_t k = send(s, c, n > CHUNK ? CHUNK : n, 0);
        if (k <= 0)
            return -1;
        c += k;
        n -= (size_t)k;
    }
    return 0;
}

/* waits until s can be read, serving the HOME button and power (APT) every 100 ms; -1 when the
 * system asks the app to close */
static int wait_readable(int s)
{
    for (;;) {
        struct pollfd pf = {s, POLLIN, 0};
        const int r = poll(&pf, 1, 100);
        if (r > 0)
            return 0;
        if (r < 0 && errno != EINTR)
            return -1;
        if (!aptMainLoop())
            return -1;
    }
}

static int recv_all(int s, void *p, size_t n)
{
    char *c = (char *)p;
    while (n) {
        if (wait_readable(s))
            return -1;
        const ssize_t k = recv(s, c, n > CHUNK ? CHUNK : n, 0);
        if (k <= 0)
            return -1;
        c += k;
        n -= (size_t)k;
    }
    return 0;
}

static int recv_line(int s, char *line, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char ch;
        if (wait_readable(s))
            return -1;
        const ssize_t k = recv(s, &ch, 1, 0);
        if (k <= 0)
            return -1;
        if (ch == '\n')
            break;
        if (ch != '\r')
            line[n++] = ch;
    }
    line[n] = 0;
    return 0;
}

static void reply(int s, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    send_all(s, buf, (size_t)n);
}

static void mkdirs(const char *path)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 6; *p; p++) /* after "sdmc:/" */
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
}

/* receives size bytes into a file; the bytes are always read, so the connection stays in step */
static int put_file(int s, const char *path, u32 size, char *buf)
{
    mkdirs(path);
    FILE *f = fopen(path, "wb");
    int ok = f != NULL;
    while (size) {
        const u32 k = size > CHUNK ? CHUNK : size;
        if (recv_all(s, buf, k))
            return -2;
        if (ok && fwrite(buf, 1, k, f) != k)
            ok = 0;
        size -= k;
    }
    if (f && fclose(f))
        ok = 0;
    return ok ? 0 : -1;
}

/* GET: "OK <size>" and the bytes */
static void cmd_get(int s, const char *path, char *buf)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        reply(s, "ERR cannot open %s", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    reply(s, "OK %ld", n);
    long left = n;
    while (left > 0) {
        const size_t k = fread(buf, 1, left > CHUNK ? CHUNK : (size_t)left, f);
        if (!k) {
            memset(buf, 0, CHUNK); /* keep the promised size */
            send_all(s, buf, left > CHUNK ? CHUNK : (size_t)left);
            left -= left > CHUNK ? CHUNK : left;
            continue;
        }
        if (send_all(s, buf, k))
            break;
        left -= (long)k;
    }
    fclose(f);
}

/* LS: "OK <n>" and n lines "<0 file / -1 directory> <name>" */
static void cmd_ls(int s, const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        reply(s, "ERR cannot open %s", path);
        return;
    }
    /* one pass into a text buffer (rewinddir does not work on the SD card), then sent */
    size_t cap_t = 4096, len = 0;
    int n = 0;
    char *txt = (char *)malloc(cap_t);
    struct dirent *e;
    while (txt && (e = readdir(d))) {
        /* no stat() per file: slow on the SD card (the profile folder holds hundreds of
         * screenshots); 0 = a file, -1 = a directory (GET gives a file's size) */
        const long sz = e->d_type == DT_DIR ? -1 : 0;
        if (len + 400 > cap_t) {
            char *t2 = (char *)realloc(txt, cap_t * 2);
            if (!t2)
                break;
            txt = t2;
            cap_t *= 2;
        }
        len += (size_t)snprintf(txt + len, cap_t - len, "%ld %s\n", sz, e->d_name);
        n++;
    }
    closedir(d);
    reply(s, "OK %d", n);
    if (txt && len)
        send_all(s, txt, len);
    free(txt);
}

static int install_cia(int s, u32 size, char *buf, char *why, size_t why_cap)
{
    Handle h = 0;
    Result r = amInit();
    if (R_FAILED(r)) {
        snprintf(why, why_cap, "amInit %08lX", r);
        /* still read the bytes */
    }
    if (R_SUCCEEDED(r)) {
        r = AM_StartCiaInstall(MEDIATYPE_SD, &h);
        if (R_FAILED(r))
            snprintf(why, why_cap, "AM_StartCiaInstall %08lX", r);
    }
    u64 off = 0;
    while (size) {
        const u32 k = size > CHUNK ? CHUNK : size;
        if (recv_all(s, buf, k)) {
            if (h)
                AM_CancelCIAInstall(h);
            amExit();
            return -2;
        }
        if (h && R_SUCCEEDED(r)) {
            u32 written = 0;
            r = FSFILE_Write(h, &written, off, buf, k, 0);
            if (R_FAILED(r))
                snprintf(why, why_cap, "FSFILE_Write %08lX", r);
        }
        off += k;
        size -= k;
    }
    if (h) {
        if (R_SUCCEEDED(r)) {
            r = AM_FinishCiaInstall(h);
            if (R_FAILED(r))
                snprintf(why, why_cap, "AM_FinishCiaInstall %08lX", r);
        } else {
            AM_CancelCIAInstall(h);
        }
    }
    amExit();
    return R_SUCCEEDED(r) ? 0 : -1;
}

static bool g_on;

bool remote_wanted(const char *data_dir)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/remote.txt", data_dir);
    FILE *f = fopen(path, "r");
    if (f)
        fclose(f);
    return f != NULL;
}

bool remote_active(void)
{
    return g_on;
}

RemoteAction remote_serve(const char *data_dir, char *rom, size_t cap)
{
    g_on = true;
    RemoteAction action = REMOTE_QUIT;
    g_soc_buf = (u32 *)memalign(0x1000, SOC_BUF_SIZE);
    if (!g_soc_buf || R_FAILED(socInit(g_soc_buf, SOC_BUF_SIZE))) {
        printf("remote: no network\n");
        free(g_soc_buf);
        return REMOTE_QUIT;
    }
    char *buf = (char *)malloc(CHUNK);
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(REMOTE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!buf || g_listen < 0 || bind(g_listen, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(g_listen, 1) < 0) {
        printf("remote: cannot listen (%d)\n", errno);
        goto done;
    }
    {
        struct in_addr ip;
        ip.s_addr = gethostid();
        printf("\x1b[2J\x1b[1;1Hctr64 remote test mode\n\n%s port %d\n\nwaiting for the PC...\n"
               "(delete remote.txt to leave this mode)\n", inet_ntoa(ip), REMOTE_PORT);
    }
    bool stop = false;
    while (!stop && aptMainLoop()) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        if (wait_readable(g_listen))
            break;
        const int s = accept(g_listen, (struct sockaddr *)&ca, &cl);
        if (s < 0) {
            svcSleepThread(100000000LL);
            continue;
        }
        char line[400];
        while (!stop && recv_line(s, line, sizeof(line)) == 0) {
            char path[320];
            unsigned long size = 0;
            if (!strcmp(line, "PING")) {
                reply(s, "OK ctr64 remote");
            } else if (sscanf(line, "PUT %lu %319[^\n]", &size, path) == 2) {
                const int r = put_file(s, path, (u32)size, buf);
                if (r == -2)
                    break;
                reply(s, r ? "ERR cannot write %s" : "OK", path);
            } else if (sscanf(line, "GET %319[^\n]", path) == 1) {
                cmd_get(s, path, buf);
            } else if (sscanf(line, "LS %319[^\n]", path) == 1) {
                cmd_ls(s, path);
            } else if (sscanf(line, "RM %319[^\n]", path) == 1) {
                reply(s, remove(path) == 0 ? "OK" : "ERR cannot remove %s", path);
            } else if (sscanf(line, "INSTALL %lu", &size) == 1) {
                char why[120] = "";
                printf("installing %lu bytes...\n", size);
                const int r = install_cia(s, (u32)size, buf, why, sizeof(why));
                if (r == -2)
                    break;
                if (r) {
                    reply(s, "ERR %s", why);
                } else {
                    reply(s, "OK");
                    action = REMOTE_RELAUNCH;
                    stop = true;
                }
            } else if (sscanf(line, "RUN %299[^\n]", rom) == 1) {
                reply(s, "OK");
                action = REMOTE_RUN;
                stop = true;
            } else if (!strcmp(line, "RELAUNCH")) {
                reply(s, "OK");
                action = REMOTE_RELAUNCH;
                stop = true;
            } else if (!strcmp(line, "QUIT")) {
                reply(s, "OK");
                action = REMOTE_QUIT;
                g_on = false;
                stop = true;
            } else {
                reply(s, "ERR unknown command");
            }
        }
        close(s);
    }
done:
    if (g_listen >= 0)
        close(g_listen);
    g_listen = -1;
    free(buf);
    socExit();
    free(g_soc_buf);
    g_soc_buf = NULL;
    (void)data_dir;
    (void)cap;
    return action;
}


/* ---- in-game listener (pause menu of a remote run) ---- */

/* as in remote mode (1 MB): with 256 KB a 16 MB GET of a state hung the 3DS on hardware. Smaller
 * only when the game's heap has no room. */
static const u32 k_pause_soc_sizes[] = {0x100000, 0x80000};

static u32 *g_psoc;
static int g_pl = -1;
static char *g_pbuf;

void remote_pause_end(void)
{
    if (g_pl >= 0)
        close(g_pl);
    g_pl = -1;
    if (g_psoc) {
        socExit();
        free(g_psoc);
        g_psoc = NULL;
    }
    free(g_pbuf);
    g_pbuf = NULL;
}

void remote_pause_begin(void)
{
    if (!g_on || g_psoc)
        return;
    for (unsigned i = 0; i < sizeof(k_pause_soc_sizes) / sizeof(k_pause_soc_sizes[0]) && !g_psoc; i++) {
        g_psoc = (u32 *)memalign(0x1000, k_pause_soc_sizes[i]);
        if (g_psoc && R_FAILED(socInit(g_psoc, k_pause_soc_sizes[i]))) {
            free(g_psoc);
            g_psoc = NULL;
            return;
        }
    }
    if (!g_psoc)
        return;
    g_pbuf = (char *)malloc(CHUNK);
    g_pl = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(REMOTE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!g_pbuf || g_pl < 0 || bind(g_pl, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(g_pl, 1) < 0) {
        remote_pause_end();
        return;
    }
    fcntl(g_pl, F_SETFL, fcntl(g_pl, F_GETFL, 0) | O_NONBLOCK);
}

static void pause_shot(int s)
{
    unsigned tb = 0, bb = 0;
    const void *top = pica_top_latest(&tb);
    const u16 *bot = ui_canvas(&bb);
    if (!top) {
        reply(s, "ERR no top-screen image yet");
        return;
    }
    reply(s, "OK %u %u", tb, bb);
    if (send_all(s, top, tb) == 0)
        send_all(s, bot, bb);
}

void remote_pause_poll(void)
{
    if (g_pl < 0)
        return;
    struct pollfd pf = {g_pl, POLLIN, 0};
    if (poll(&pf, 1, 0) <= 0)
        return;
    struct sockaddr_in ca;
    socklen_t cl = sizeof(ca);
    const int s = accept(g_pl, (struct sockaddr *)&ca, &cl);
    if (s < 0)
        return;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) & ~O_NONBLOCK);
    char line[400];
    while (recv_line(s, line, sizeof(line)) == 0) {
        char path[320];
        unsigned long size = 0;
        int slot = 0;
        if (!strcmp(line, "PING")) {
            reply(s, "OK ctr64 paused");
        } else if (sscanf(line, "PUT %lu %319[^\n]", &size, path) == 2) {
            const int r = put_file(s, path, (u32)size, g_pbuf);
            if (r == -2)
                break;
            reply(s, r ? "ERR cannot write %s" : "OK", path);
        } else if (sscanf(line, "GET %319[^\n]", path) == 1) {
            cmd_get(s, path, g_pbuf);
        } else if (sscanf(line, "LS %319[^\n]", path) == 1) {
            cmd_ls(s, path);
        } else if (sscanf(line, "RM %319[^\n]", path) == 1) {
            reply(s, remove(path) == 0 ? "OK" : "ERR cannot remove %s", path);
        } else if (sscanf(line, "STATE %d", &slot) == 1) {
            if (slot < 0 || slot > 9) {
                reply(s, "ERR slot 0..9");
                continue;
            }
            core_host_state_path(slot, path, sizeof(path));
            reply(s, core_host_save_state(slot) ? "OK %s" : "ERR saving %s failed", path);
        } else if (!strcmp(line, "SHOT")) {
            pause_shot(s);
        } else {
            reply(s, "ERR not while paused: %s", line);
        }
    }
    close(s);
}
