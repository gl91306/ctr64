#pragma once

#include <stdio.h>

/* Installs the crash reporter for the calling thread (see crash.c). `extra` adds lines to the
 * report, e.g. emulator state. */
void crash_install(void (*extra)(FILE *));
