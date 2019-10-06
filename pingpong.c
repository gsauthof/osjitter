// pingpong - measure thread notification overhead
//
// 2019, Georg Sauthoff <mail@gms.tf>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <pthread.h>
#include <x86intrin.h> // __rdtsc(), _mm_lfence(), ...

#include "util.h"

static atomic_bool start_work;

static _Atomic uint64_t g_tsc;

extern __inline uint64_t __attribute__((__gnu_inline__, __always_inline__, __artificial__))
double_fenced_rdtsc(void)
{
    // https://www.felixcloutier.com/x86/rdtsc
    // If software requires RDTSC to be executed only after all previous
    // instructions have executed and all previous loads and stores are
    // globally visible, it can execute the sequence MFENCE;LFENCE immediately
    // before RDTSC.
    // If software requires RDTSC to be executed prior to execution of any
    // subsequent instruction (including any memory accesses), it can execute
    // the sequence LFENCE immediately after RDTSC.
    _mm_mfence();
    _mm_lfence();
    uint64_t r = __rdtsc();
    _mm_lfence();
    return r;
}
extern __inline uint64_t __attribute__((__gnu_inline__, __always_inline__, __artificial__))
far_fenced_rdtsc(void)
{
    // https://www.felixcloutier.com/x86/rdtsc
    // If software requires RDTSC to be executed prior to execution of any
    // subsequent instruction (including any memory accesses), it can execute
    // the sequence LFENCE immediately after RDTSC.
    uint64_t r = __rdtsc();
    _mm_lfence();
    return r;
}

struct Args {
    uint32_t tsc_khz;
    uint32_t mult;
    uint32_t shift;
    unsigned n;    // number of iterations
    unsigned k; // number of pause iterations before each store
    unsigned pin[2];
    bool json;
};
typedef struct Args Args;

static void help(FILE *f, const char *argv0)
{
    fprintf(f, "pingpong - measure inter thread notification overhead\n"
            "\n"
            "call: %s [OPT..]\n"
            "\n"
            "Options:\n"
            "  --khz             TSC frequency (default: parse journalctl, read /proc)\n"
            "  -n                ping-pong iterations (default: 10^6)\n"
            "  -k                #iterations pause before storing (default: 1000)\n"
            "  -pin THREAD CPU   0 <= THREAD <= 1, pin each thread to a CPU/core\n"
            "                    (default: no pinning)\n"
            "  --json            write raw values to JSON file (default: false)\n"
            "\n"
            "2019, Georg Sauthoff <mail@gms.tf>, GPLv3+\n"
            , argv0);
}

static int parse_args(Args *args, int argc, char **argv)
{
    *args = (const Args){0};
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            help(stdout, argv[0]);
            exit(0);
        } else if (!strcmp(argv[i], "--khz")) {
            ++i;
            if (i >= argc) {
                fprintf(stderr, "--khz argument is missing\n");
                return -1;
            }
            args->tsc_khz = atoi(argv[i]);
        } else if (!strcmp(argv[i], "-n")) {
            ++i;
            if (i >= argc) {
                fprintf(stderr, "-n argument is missing\n");
                return -1;
            }
            args->n = atoi(argv[i]);
        } else if (!strcmp(argv[i], "-k")) {
            ++i;
            if (i >= argc) {
                fprintf(stderr, "-k argument is missing\n");
                return -1;
            }
            args->k = atoi(argv[i]);
        } else if (!strcmp(argv[i], "--pin")) {
            if (i+2 >= argc) {
                fprintf(stderr, "--pin THREAD CPU arguments are missing\n");
                return -1;
            }
            unsigned j = atoi(argv[++i]);
            unsigned cpu = atoi(argv[++i]);
            if (j > 1) {
                fprintf(stderr, "--pin THREAD CPU - 0 <= THREAD <= 1\n");
                return -1;
            }
            args->pin[j] = cpu + 1;
        } else if (!strcmp(argv[i], "--json")) {
            args->json = true;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            exit(1);
        }
    }
    if (!args->n)
        args-> n = 1000 * 1000;
    if (!args->k)
        args-> k = 1000;
    return 0;
}

struct Worker {
    pthread_t worker_id;
    unsigned init; // 0 -> start with send, 1 -> start with retrieve
    unsigned n;    // number of iterations
    unsigned k;
    uint32_t *raw_ds;  // delta values
    uint32_t *ds;  // delta values
    unsigned ds_size; // #delta values
};
typedef struct Worker Worker;

static void *spin_main(void *p)
{
    Worker *x = (Worker*) p;
    Worker w = *x;

    uint64_t tsc = 1;
    unsigned j = 0;
    uint32_t *ds = calloc(w.n/2, sizeof ds[0]);
    if (!ds) {
        fprintf(stderr, "Failed to allocate delta array in thread\n");
        return 0;
    }

    while(!atomic_load_explicit(&start_work, memory_order_consume)) {
        _mm_pause();
    }

    for (unsigned i = w.init; i < w.n; ++i) {
        if (i % 2 == 0) { // sender
            unsigned k = i ? w.k : w.k * 2;
            for (unsigned i = 0; i < k; ++i)
                _mm_pause();
            tsc = double_fenced_rdtsc();
            atomic_store_explicit(&g_tsc, tsc, memory_order_release);
        } else { // retriever
            uint64_t new_tsc;
            for (;;) {
                new_tsc = atomic_load_explicit(&g_tsc, memory_order_consume);
                if (new_tsc >= tsc) {
                    break;
                }
                // _mm_pause();
            }
            uint64_t now   = far_fenced_rdtsc();
            uint64_t delta = now - new_tsc;
            ds[j++] = delta;
        }
    }
    assert(j <= w.n/2);
    uint32_t *raw_ds = malloc(j * sizeof raw_ds[0]);
    if (!raw_ds) {
        fprintf(stderr, "Failed to allocate delta array in thread\n");
        return 0;
    }
    memcpy(raw_ds, ds, j * sizeof ds[0]);
    qsort(ds, j, sizeof ds[0], cmp_u32);
    x->ds = ds;
    x->raw_ds = raw_ds;
    x->ds_size = j;
    return x;
}

static int print_json(const Args *args, const Worker *ws, FILE *f)
{
    fprintf(f, "[\n");
    for (unsigned i = 0; i < 2; ++i) {
        const Worker *w = ws + i;
        fprintf(f, "    [");
        if (w->ds_size) {
            fprintf(f, " %" PRIu64,
                    mul_u64_u32_shr(w->raw_ds[0], args->mult, args->shift));
        }
        for (unsigned j = 1; j < w->ds_size; ++j) {
            fprintf(f, ", %" PRIu64,
                    mul_u64_u32_shr(w->raw_ds[j], args->mult, args->shift));
        }
        fprintf(f, "]");
        if (!i)
            fprintf(f, ",\n");
    }
    fprintf(f, "\n]\n");
    return 0;
}

static int pp_results(const Args *args, const Worker *ws, FILE *f)
{
    fprintf(f, "Thread  TSC_khz  #delta  min_ns  max_ns  median_ns  p20_ns  p80_ns  p90_ns  p99_ns  p99.9_ns  mad_ns\n");
    uint32_t *ys = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const Worker *w = ws + i;
        ys = realloc(ys, w->ds_size * sizeof ys[0]);
        if (!ys) {
            fprintf(stderr, "realloc in pp_results failed\n");
            return -1;
        }
        uint32_t mad = mad_u32(w->ds, ys, w->ds_size);
        if (!w->ds_size)
            continue;
        fprintf(f, "%6u %8" PRIu32  " %7u "
                "%7" PRIu64 " "
                "%7" PRIu64 " "
                "%10" PRIu64 " "
                "%7" PRIu64 " "
                "%7" PRIu64 " "
                "%7" PRIu64 " "
                "%7" PRIu64 " "
                "%9" PRIu64 " "
                "%7" PRIu64 " "
                "\n",
                i, args->tsc_khz, w->ds_size, 
                mul_u64_u32_shr(w->ds[0],
                    args->mult, args->shift),
                mul_u64_u32_shr(w->ds[w->ds_size - 1],
                    args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->ds, w->ds_size, 1, 2),
                    args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->ds, w->ds_size, 1, 5),
                    args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->ds, w->ds_size, 4, 5),
                    args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->ds, w->ds_size, 90, 100),
                    args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->ds, w->ds_size, 99, 100),
                    args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->ds, w->ds_size, 999, 1000),
                    args->mult, args->shift),
                mul_u64_u32_shr(mad, args->mult, args->shift)
               );
    }
    free(ys);
    return 0;
}

static int spin_pingpong(const Args *args)
{
    Worker ws[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        ws[i].n = args->n;
        ws[i].k = args->k;
        ws[i].init = i;
        pthread_attr_t attr;
        int r = pthread_attr_init(&attr);
        if (r) {
            perror_e(r, "pthread_attr_init failed");
            return 1;
        }
        if (args->pin[i]) {
            cpu_set_t cpus;
            CPU_ZERO(&cpus);
            CPU_SET(args->pin[i] - 1, &cpus);
            r = pthread_attr_setaffinity_np(&attr, sizeof cpus, &cpus);
            if (r) {
                perror_e(r, "pthread_attr_setaffinity_np failed");
                return 1;
            }
        }
        r = pthread_create(&ws[i].worker_id, &attr, spin_main, ws+i);
        if (r) {
            perror_e(r, "pthread_create failed");
            return 1;
        }
        r = pthread_attr_destroy(&attr);
        if (r) {
            perror_e(r, "pthread_attr_init failed");
            return 1;
        }
    }

    atomic_store_explicit(&start_work, true, memory_order_release);

    bool error_in_thread = false;
    for (unsigned i = 0; i < 2; ++i) {
        void *w_ret = 0;
        int r = pthread_join(ws[i].worker_id, &w_ret);
        if (r) {
            perror_e(r, "pthread_join failed");
            return 1;
        }
        if (!w_ret)
            error_in_thread = true;
    }
    if (error_in_thread) {
        fprintf(stderr, "One thread reported an error\n");
        return 1;
    }
    if (args->json)
        print_json(args, ws, stdout);
    else
        pp_results(args, ws, stdout);
    for (unsigned i = 0; i < 2; ++i) {
        free(ws[i].ds);
        free(ws[i].raw_ds);
    }
    return 0;
}


int main(int argc, char **argv)
{
    Args args;
    int r = parse_args(&args, argc, argv);
    if (r) {
        return 1;
    }
    if (!args.tsc_khz) {
        int r = get_tsc_khz(&args.tsc_khz);
        if (r < 0)
            return 1;
    }
    clocks_calc_mult_shift(&args.mult, &args.shift,
            args.tsc_khz, 1000000l, 0);

    r = spin_pingpong(&args);
    if (r)
        return 1;
    return 0;
}
