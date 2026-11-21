/* Fast answers for files that are not there.
 *
 * ctr64 and its GLideN64 backend look for about 80 optional files in the data folder (development
 * switches such as noguard.txt, settings, logs). On the 3DS each fopen of a missing file there
 * costs ~65 ms (HW 2026-11-21: the ~30 checks in the graphics init took 2 s; picking a game to its
 * first frame took 15-20 s). fopen is wrapped at link time (-Wl,--wrap=fopen): the data folder is
 * listed once, and a read of a file directly in it that the listing does not have fails at once.
 * Files ctr64 creates there itself (fopen for writing) are added to the list. */
#include <3ds.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define DEV_DIR "sdmc:/3ds/ctr64/"
#define MAX_NAMES 256
#define NAME_LEN 64

FILE *__real_fopen(const char *path, const char *mode);

static char g_names[MAX_NAMES][NAME_LEN];
static int g_count;
static bool g_listed, g_overflow;
static LightLock g_lock = 1; /* unlocked (LightLock_Init's value) */

static void add_name(const char *name)
{
    for (int i = 0; i < g_count; i++)
        if (strcasecmp(g_names[i], name) == 0)
            return;
    if (g_count == MAX_NAMES || strlen(name) >= NAME_LEN) {
        g_overflow = true; /* the list is incomplete: no shortcut from here on */
        return;
    }
    strcpy(g_names[g_count++], name);
}

static void list_dir(void)
{
    g_listed = true;
    DIR *d = opendir("sdmc:/3ds/ctr64");
    if (!d)
        return; /* no folder yet: nothing in it */
    struct dirent *e;
    while ((e = readdir(d)))
        add_name(e->d_name);
    closedir(d);
}

/* the file name when path is a file directly in the data folder, else NULL */
static const char *top_name(const char *path)
{
    const size_t n = sizeof(DEV_DIR) - 1;
    if (strncasecmp(path, DEV_DIR, n) != 0)
        return NULL;
    const char *name = path + n;
    return name[0] && !strchr(name, '/') ? name : NULL;
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    const char *name = path && mode ? top_name(path) : NULL;
    if (!name)
        return __real_fopen(path, mode);
    const bool read_only = mode[0] == 'r' && !strchr(mode, '+');
    LightLock_Lock(&g_lock);
    if (!g_listed)
        list_dir();
    bool known = g_overflow;
    for (int i = 0; i < g_count && !known; i++)
        known = strcasecmp(g_names[i], name) == 0;
    if (!read_only && !known)
        add_name(name); /* being created */
    LightLock_Unlock(&g_lock);
    if (read_only && !known) {
        errno = ENOENT;
        return NULL;
    }
    return __real_fopen(path, mode);
}
