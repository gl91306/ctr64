#include "rom_title.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Display names: a table of well-known games by the two-letter game id of the ROM header
 * (bytes 0x3C-0x3D; 0x3B is the media type, 0x3E the region, 0x3F the revision). Unknown games
 * use a No-Intro style file name ("Name (Region) (Rev 1).z64") if the file has one, else the
 * header's internal name in title case. */

static const struct {
    char id[3];
    const char *title;
    /* words of the game's own internal names (header 0x20, letters and digits only, any region),
     * '|' between alternatives: a ROM with this id but none of them in its name is a hack (Smash
     * Remix keeps Super Smash Bros.' id) and is listed under its own name */
    const char *names;
} k_games[] = {
    {"SM", "Super Mario 64", "MARIO64"},
    {"ZL", "The Legend of Zelda: Ocarina of Time", "ZELDA"},
    {"ZS", "The Legend of Zelda: Majora's Mask", "ZELDA|MUJURA"},
    {"KT", "Mario Kart 64", "MARIOKART"},
    {"AL", "Super Smash Bros.", "SMASHBROTHERS|DAIRANTOU"},
    {"MQ", "Paper Mario", "PAPERMARIO|MARIOSTORY"},
    {"GE", "GoldenEye 007", "GOLDENEYE"},
    {"PD", "Perfect Dark", "PERFECTDARK"},
    {"BK", "Banjo-Kazooie", "BANJO"},
    {"B7", "Banjo-Tooie", "BANJO"},
    {"DY", "Diddy Kong Racing", "DIDDY"},
    {"DO", "Donkey Kong 64", "DONKEYKONG"},
    {"FZ", "F-Zero X", "FZERO"},
    {"YS", "Yoshi's Story", "YOSHI"},
    {"K4", "Kirby 64: The Crystal Shards", "KIRBY"},
    {"FU", "Conker's Bad Fur Day", "CONKER"},
    {"WR", "Wave Race 64", "WAVERACE"},
    {"FX", "Star Fox 64", "STARFOX|LYLATWARS"},
    {"TE", "1080\xC2\xB0 Snowboarding", "1080"},
    {"PF", "Pok\xC3\xA9mon Snap", "SNAP"},
    {"PO", "Pok\xC3\xA9mon Stadium", "STADIUM"},
    {"P3", "Pok\xC3\xA9mon Stadium 2", "STADIUM"},
    {"M8", "Mario Tennis", "TENNIS"},
    {"MF", "Mario Golf", "GOLF"},
    {"MW", "Mario Party 2", "MARIOPARTY"},
    {"MV", "Mario Party 3", "MARIOPARTY"},
    {"PW", "Pilotwings 64", "PILOTWINGS"},
    {"MX", "Excitebike 64", "EXCITEBIKE"},
    {"JF", "Jet Force Gemini", "JETFORCE"},
    {"BC", "Blast Corps", "BLASTCORPS|BLASTDOZER"},
    {"N6", "Dr. Mario 64", "DRMARIO"},
    {"SW", "Star Wars: Shadows of the Empire", "SHADOW"},
    {"RS", "Star Wars: Rogue Squadron", "ROGUE"},
};

static const char *region_name(char c)
{
    switch (c) {
    case 'E': return "USA";
    case 'J': return "Japan";
    case 'P': case 'X': case 'Y': return "Europe";
    case 'D': return "Germany";
    case 'F': return "France";
    case 'I': return "Italy";
    case 'S': return "Spain";
    case 'U': return "Australia";
    default: return NULL;
    }
}

/* "Legend of Zelda, The - Ocarina of Time (USA) (Rev 2).z64" -> "The Legend of Zelda: Ocarina of
 * Time" and "USA, Rev 2"; false if the name has no " (" tag (not a No-Intro style name) */
static bool from_file(const char *file, char *title, size_t tn, char *tags, size_t gn)
{
    char t[128];
    snprintf(t, sizeof(t), "%s", file);
    char *dot = strrchr(t, '.');
    if (dot)
        *dot = 0;
    char *par = strstr(t, " (");
    if (!par)
        return false;
    /* tags */
    size_t n = 0;
    for (char *p = par; *p && n + 3 < gn;) {
        if (*p == '(' || *p == ')' || *p == '[' || *p == ']' || (*p == ' ' && n == 0)) {
            p++;
            continue;
        }
        if (*p == ' ' && p[1] == '(') {
            tags[n++] = ',';
            tags[n++] = ' ';
            p += 2;
            continue;
        }
        tags[n++] = *p++;
    }
    tags[n] = 0;
    *par = 0;
    /* "Name, The - Sub" -> "The Name: Sub" */
    char *dash = strstr(t, " - ");
    char sub[96] = "";
    if (dash) {
        snprintf(sub, sizeof(sub), "%s", dash + 3);
        *dash = 0;
    }
    size_t l = strlen(t);
    if (l > 5 && !strcmp(t + l - 5, ", The")) {
        t[l - 5] = 0;
        snprintf(title, tn, "The %s%s%s", t, sub[0] ? ": " : "", sub);
    } else {
        snprintf(title, tn, "%s%s%s", t, sub[0] ? ": " : "", sub);
    }
    return true;
}

/* "SUPER MARIO 64  " -> "Super Mario 64"; short all-capital words (CPU, NBA) stay as they are */
static void from_header(const unsigned char *h, char *title, size_t tn)
{
    char raw[21];
    memcpy(raw, h + 0x20, 20);
    raw[20] = 0;
    for (int i = 19; i >= 0 && (raw[i] == ' ' || raw[i] == 0); i--)
        raw[i] = 0;
    size_t n = 0;
    bool start = true;
    for (const char *p = raw; *p && n + 1 < tn;) {
        const char *e = p;
        while (*e && *e != ' ')
            e++;
        const size_t wl = (size_t)(e - p);
        const bool small = !start && ((wl == 2 && !strncmp(p, "OF", 2)) ||
                                      (wl == 3 && (!strncmp(p, "THE", 3) || !strncmp(p, "AND", 3))));
        for (size_t i = 0; i < wl && n + 1 < tn; i++) {
            char c = p[i];
            if (small || (wl > 3 && i > 0 && p[i - 1] != '-'))
                c = (char)tolower((unsigned char)c);
            title[n++] = c;
        }
        start = false;
        p = e;
        while (*p == ' ' && n + 1 < tn) {
            title[n++] = ' ';
            p++;
        }
    }
    title[n] = 0;
}

/* the header's internal name (letters and digits, upper case) contains one of names */
/* Known dumps: the header checksums (0x10-0x17, CRC1 CRC2) of every ROM in the core's ROM catalog
 * (mupen64plus.ini, built into the core as `inifile`: ~3200 entries, retail games plus known
 * translations and fixes). A hack that changes the first megabyte of the game must fix these
 * checksums or the boot code refuses it, so a ROM with a well-known game's ID but checksums that are
 * not in the catalog is a hack even when it kept the internal name (TTYD64 is "PAPER MARIO"). */
extern char inifile[];
static uint64_t *g_known;
static int g_known_n = -1;

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static bool crc_known(const unsigned char *h)
{
    if (g_known_n < 0) { /* first call: collect and sort the catalog's "CRC=XXXXXXXX YYYYYYYY" lines */
        g_known_n = 0;
        int cap = 0;
        for (const char *p = inifile; (p = strstr(p, "\nCRC=")) != NULL; p += 5) {
            char *e;
            const unsigned long a = strtoul(p + 5, &e, 16);
            if (e != p + 13 || *e != ' ')
                continue;
            const unsigned long b = strtoul(e + 1, &e, 16);
            if (g_known_n == cap) {
                cap = cap ? cap * 2 : 4096;
                uint64_t *n = realloc(g_known, (size_t)cap * sizeof(*n));
                if (!n)
                    break;
                g_known = n;
            }
            g_known[g_known_n++] = (uint64_t)a << 32 | (uint32_t)b;
        }
        qsort(g_known, (size_t)g_known_n, sizeof(*g_known), cmp_u64);
    }
    if (g_known_n == 0)
        return true; /* no catalog: cannot tell */
    const uint64_t k = (uint64_t)h[0x10] << 56 | (uint64_t)h[0x11] << 48 | (uint64_t)h[0x12] << 40 |
                       (uint64_t)h[0x13] << 32 | (uint64_t)h[0x14] << 24 | (uint64_t)h[0x15] << 16 |
                       (uint64_t)h[0x16] << 8 | h[0x17];
    return bsearch(&k, g_known, (size_t)g_known_n, sizeof(*g_known), cmp_u64) != NULL;
}

static bool internal_name_matches(const unsigned char *h, const char *names)
{
    char n[24];
    size_t k = 0;
    for (int i = 0x20; i < 0x34 && k + 1 < sizeof(n); i++)
        if (isalnum(h[i]))
            n[k++] = (char)toupper(h[i]);
    n[k] = 0;
    if (!k)
        return true; /* no name to go by */
    for (const char *p = names; *p;) {
        const char *e = strchr(p, '|');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        char w[24];
        snprintf(w, sizeof(w), "%.*s", (int)l, p);
        if (strstr(n, w))
            return true;
        p += l + (e ? 1 : 0);
    }
    return false;
}

void rom_title_from_header(const unsigned char hdr[64], const char *file, char *title, size_t tn, char *tags, size_t gn)
{
    unsigned char h[64];
    if (hdr[0] == 0x37 && hdr[1] == 0x80) /* .v64: 16-bit swapped */
        for (int i = 0; i < 64; i++)
            h[i] = hdr[i ^ 1];
    else if (hdr[0] == 0x40 && hdr[1] == 0x12) /* .n64: 32-bit little endian */
        for (int i = 0; i < 64; i++)
            h[i] = hdr[(i & ~3) | (3 - (i & 3))];
    else
        memcpy(h, hdr, 64);
    title[0] = tags[0] = 0;
    const char id[3] = {(char)h[0x3C], (char)h[0x3D], 0};
    for (unsigned i = 0; i < sizeof(k_games) / sizeof(k_games[0]); i++)
        if (!strcmp(id, k_games[i].id)) {
            if (!internal_name_matches(h, k_games[i].names)) { /* a hack: listed by its own name */
                from_header(h, title, tn);
                if (title[0]) {
                    snprintf(tags, gn, "%s hack", k_games[i].title);
                    return;
                }
            }
            if (file && !crc_known(h)) { /* a hack with the game's own name: listed by its file name */
                char ft[16];
                if (!from_file(file, title, tn, ft, sizeof(ft))) {
                    snprintf(title, tn, "%s", file);
                    char *dot = strrchr(title, '.');
                    if (dot)
                        *dot = 0;
                }
                snprintf(tags, gn, "%s hack", k_games[i].title);
                return;
            }
            snprintf(title, tn, "%s", k_games[i].title);
            const char *r = region_name((char)h[0x3E]);
            if (h[0x3F] && r)
                snprintf(tags, gn, "%s, Rev %d", r, h[0x3F]);
            else if (r)
                snprintf(tags, gn, "%s", r);
            return;
        }
    if (file && from_file(file, title, tn, tags, gn))
        return;
    from_header(h, title, tn);
    if (!title[0] && file) {
        snprintf(title, tn, "%s", file);
        char *dot = strrchr(title, '.');
        if (dot)
            *dot = 0;
    }
}

void rom_title_read(const char *path, char *title, size_t tn, char *tags, size_t gn)
{
    rom_title_read_id(path, title, tn, tags, gn, NULL);
}

void rom_title_read_id(const char *path, char *title, size_t tn, char *tags, size_t gn, char id[5])
{
    unsigned char h[64] = {0};
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fread(h, 1, sizeof(h), f) != sizeof(h))
            memset(h, 0, sizeof(h));
        fclose(f);
    }
    rom_title_from_header(h, base, title, tn, tags, gn);
    if (id) {
        unsigned char k[64];
        if (h[0] == 0x37 && h[1] == 0x80)
            for (int i = 0; i < 64; i++)
                k[i] = h[i ^ 1];
        else if (h[0] == 0x40 && h[1] == 0x12)
            for (int i = 0; i < 64; i++)
                k[i] = h[(i & ~3) | (3 - (i & 3))];
        else
            memcpy(k, h, 64);
        for (int i = 0; i < 4; i++)
            id[i] = isalnum(k[0x3B + i]) ? (char)toupper(k[0x3B + i]) : 0;
        id[4] = 0;
        if (!id[0] || !id[1] || !id[2] || !id[3])
            id[0] = 0;
    }
}
