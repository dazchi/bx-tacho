// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Runs the fuzz target (fuzz_load_classify.c) without libFuzzer, which MSYS2's
// clang doesn't have on Windows: seeded random inputs, or the files given.
//
//   fuzz_load_classify [-seed N] [-runs N] [-max_len N] [file ...]
//
// A failing input (a property, or a sanitizer error) is written to
// crash-<seed>-<run>.bin first, and can be run again by passing that file.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sanitizer/common_interface_defs.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
extern unsigned long fuzz_checks, fuzz_checks_relearned;

#define DEFAULT_RUNS     100000
#define DEFAULT_MAX_LEN  256
#define MAX_LEN_LIMIT    4096
#define PROGRESS_EVERY   10000
#define CRASH_NAME_LEN   64
#define XORSHIFT_A       13
#define XORSHIFT_B       17
#define XORSHIFT_C       5

static uint8_t s_input[MAX_LEN_LIMIT];
static size_t s_len;
static unsigned long s_seed, s_run;
static int s_from_file;

static uint32_t s_rng;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << XORSHIFT_A;
    s_rng ^= s_rng >> XORSHIFT_B;
    s_rng ^= s_rng << XORSHIFT_C;
    return s_rng;
}

// Called on a sanitizer error before the process dies, and on abort()
static void save_input(void)
{
    if (s_from_file) {
        return;
    }
    char name[CRASH_NAME_LEN];
    snprintf(name, sizeof(name), "crash-%lu-%lu.bin", s_seed, s_run);
    FILE *f = fopen(name, "wb");
    if (f) {
        fwrite(s_input, 1, s_len, f);
        fclose(f);
        fprintf(stderr, "input written to %s (%u bytes)\n", name, (unsigned)s_len);
    }
}

static void on_abort(int sig)
{
    (void)sig;
    save_input();
    _Exit(EXIT_FAILURE);
}

#include <signal.h>

int main(int argc, char **argv)
{
    unsigned long runs = DEFAULT_RUNS;
    size_t max_len = DEFAULT_MAX_LEN;
    s_seed = (unsigned long)time(NULL);
    int files = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-seed") && i + 1 < argc) {
            s_seed = strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-runs") && i + 1 < argc) {
            runs = strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-max_len") && i + 1 < argc) {
            max_len = strtoul(argv[++i], NULL, 0);
            max_len = max_len > MAX_LEN_LIMIT ? MAX_LEN_LIMIT : max_len;
        } else {
            files++;
        }
    }
    __sanitizer_set_death_callback(save_input);
    signal(SIGABRT, on_abort);

    if (files) {
        // Run the given inputs (a crash found earlier)
        s_from_file = 1;
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                i++;
                continue;
            }
            FILE *f = fopen(argv[i], "rb");
            if (!f) {
                fprintf(stderr, "%s: can't open\n", argv[i]);
                return EXIT_FAILURE;
            }
            s_len = fread(s_input, 1, sizeof(s_input), f);
            fclose(f);
            printf("%s: %u bytes\n", argv[i], (unsigned)s_len);
            LLVMFuzzerTestOneInput(s_input, s_len);
        }
        printf("ok\n");
        return EXIT_SUCCESS;
    }

    printf("seed %lu, %lu runs, inputs up to %u bytes\n", s_seed, runs, (unsigned)max_len);
    s_rng = (uint32_t)s_seed ? (uint32_t)s_seed : 1u;
    for (s_run = 0; s_run < runs; s_run++) {
        s_len = rnd() % (max_len + 1);
        for (size_t i = 0; i < s_len; i++) {
            s_input[i] = (uint8_t)rnd();
        }
        LLVMFuzzerTestOneInput(s_input, s_len);
        if ((s_run + 1) % PROGRESS_EVERY == 0) {
            printf("  %lu runs\n", s_run + 1);
            fflush(stdout);
        }
    }
    printf("ok: %lu runs, no failure; \"never stuck\" checked %lu times, %lu of them on a re-learned launcher\n",
           runs, fuzz_checks, fuzz_checks_relearned);
    return EXIT_SUCCESS;
}
