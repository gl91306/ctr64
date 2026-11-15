#pragma once
#include <3ds.h>
#include <stdio.h>

/* Calibration benchmark of the Detailed profiler (bench.c): "# bench" lines into `out` */
void bench_run(FILE *out);
