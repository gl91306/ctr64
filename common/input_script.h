#pragma once

/* Scripted controller input for deterministic test runs (development aid), shared by the
 * Windows and 3DS frontends so both can drive a game through the same sequence and their
 * frames can be compared.
 *
 * Script lines: "<from> <to> <inputs...>", frames counted in presented video frames (from 1),
 * inclusive. Inputs: A B Z START L R DU DD DL DR CU CD CL CR, and SX=<n> SY=<n> for the
 * analog stick in N64 units (-80..80, up is positive). '#' starts a comment. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#define INPUT_SCRIPT_MAX 512

typedef struct {
    unsigned from, to;
    unsigned buttons; /* RetroPad bits as Mupen64Plus-Next maps the N64 pad */
    int sx, sy, cx, cy;
    int has_stick;
} InputScriptEntry;

static struct {
    InputScriptEntry e[INPUT_SCRIPT_MAX];
    int n;
    int loaded; /* 1 if a script is active */
} g_input_script;

static void input_script_load(const char *path)
{
    FILE *f = path ? fopen(path, "r") : NULL;
    char line[256];

    g_input_script.n = 0;
    g_input_script.loaded = 0;
    if (!f)
        return;
    while (fgets(line, sizeof(line), f) && g_input_script.n < INPUT_SCRIPT_MAX) {
        InputScriptEntry *e = &g_input_script.e[g_input_script.n];
        char tokens[16][32];
        int ntok = 0;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        memset(e, 0, sizeof(*e));
        for (char *p = line; *p && ntok < 16;) {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                p++;
            size_t len = strcspn(p, " \t\r\n");
            if (!len)
                break;
            snprintf(tokens[ntok++], sizeof(tokens[0]), "%.*s", (int)len, p);
            p += len;
        }
        if (ntok < 2)
            continue;
        e->from = (unsigned)strtoul(tokens[0], NULL, 10);
        e->to = (unsigned)strtoul(tokens[1], NULL, 10);
        for (int t = 2; t < ntok; t++) {
            const char *tok = tokens[t];
            static const struct { const char *name; unsigned id; } map[] = {
                {"A", RETRO_DEVICE_ID_JOYPAD_B},      {"B", RETRO_DEVICE_ID_JOYPAD_Y},
                {"Z", RETRO_DEVICE_ID_JOYPAD_L2},     {"START", RETRO_DEVICE_ID_JOYPAD_START},
                {"L", RETRO_DEVICE_ID_JOYPAD_L},      {"R", RETRO_DEVICE_ID_JOYPAD_R},
                {"DU", RETRO_DEVICE_ID_JOYPAD_UP},    {"DD", RETRO_DEVICE_ID_JOYPAD_DOWN},
                {"DL", RETRO_DEVICE_ID_JOYPAD_LEFT},  {"DR", RETRO_DEVICE_ID_JOYPAD_RIGHT},
            };
            int matched = 0;
            for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
                if (!strcmp(tok, map[i].name)) {
                    e->buttons |= 1u << map[i].id;
                    matched = 1;
                }
            if (matched)
                continue;
            if (!strncmp(tok, "SX=", 3)) { e->sx = atoi(tok + 3); e->has_stick = 1; }
            else if (!strncmp(tok, "SY=", 3)) { e->sy = atoi(tok + 3); e->has_stick = 1; }
            else if (!strcmp(tok, "CU")) e->cy = 80;
            else if (!strcmp(tok, "CD")) e->cy = -80;
            else if (!strcmp(tok, "CL")) e->cx = -80;
            else if (!strcmp(tok, "CR")) e->cx = 80;
        }
        g_input_script.n++;
    }
    fclose(f);
    g_input_script.loaded = 1;
}

/* RetroPad state for `frame` (button mask, analog index/id), or 0. */
static int16_t input_script_state(unsigned frame, unsigned device, unsigned index, unsigned id)
{
    unsigned buttons = 0;
    int ax[2] = {0, 0}, ay[2] = {0, 0};
    for (int i = 0; i < g_input_script.n; i++) {
        const InputScriptEntry *e = &g_input_script.e[i];
        if (frame < e->from || frame > e->to)
            continue;
        buttons |= e->buttons;
        if (e->has_stick) {
            ax[0] = e->sx;
            ay[0] = e->sy;
        }
        if (e->cx || e->cy) {
            ax[1] = e->cx;
            ay[1] = e->cy;
        }
    }
    switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            return (int16_t)buttons;
        return (buttons >> id) & 1;
    case RETRO_DEVICE_ANALOG:
        if (index > RETRO_DEVICE_INDEX_ANALOG_RIGHT)
            return 0;
        /* N64 -80..80 (up positive) -> RetroPad -32767..32767 (down positive) */
        if (id == RETRO_DEVICE_ID_ANALOG_X)
            return (int16_t)(ax[index] * 32767 / 80);
        if (id == RETRO_DEVICE_ID_ANALOG_Y)
            return (int16_t)(-ay[index] * 32767 / 80);
        return 0;
    }
    return 0;
}
