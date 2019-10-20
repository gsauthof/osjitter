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
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <x86intrin.h> // __rdtsc(), _mm_lfence(), ...
#include <sys/syscall.h>
#include <linux/futex.h>
#include <errno.h>

#include "util.h"
#include "tsc.h"

static atomic_bool start_work;

// make sure that both variables go into different cachelines
// (intel/amd CPUs have 64 byte cache lines)
// without C11 support
//static _Atomic uint64_t g_tsc   __attribute__ ((aligned (64)));
//static alignas(64) _Atomic uint64_t g_tsc;


struct Cell {
    alignas(64) _Atomic uint64_t tsc;
};
typedef struct Cell Cell;

static Cell g_cell[2];

// without C11 support:
// struct Item { ... } __attribute__ ((aligned (64)));

struct Item {
    // aligning the first field is equivalent to aligning the struct itself
    alignas(64) pthread_mutex_t mutex;
    pthread_cond_t cond_var;
    uint64_t tsc;
};
typedef struct Item Item;

static_assert(sizeof(Item) % 64 == 0, "Item is not aligned");

static Item g_item[2] = {
    { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER },
    { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER }
};

static_assert(alignof(g_item) == 64, "Item array is not aligned");

static int g_pipes[2][2];


struct Follicle {
    alignas(64) _Atomic int futex;
    uint64_t tsc;
};
typedef struct Follicle Follicle;
static Follicle g_follicle[2];

static int
atomic_futex(_Atomic int *uaddr, int futex_op, int val,
      const struct timespec *timeout, int *uaddr2, int val3)
{
    (void)uaddr2;
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr, val3);
}

static int futex_lock(_Atomic int *f)
{
    for (;;) {
        int zero = 0;
        if (atomic_compare_exchange_weak(f, &zero, 1))
            return 0;
        int r = atomic_futex(f, FUTEX_WAIT_PRIVATE, 1, NULL, NULL, 0);
        if (r == -1) {
            if (errno != EAGAIN)
                return r;
        }
    }
    return 0;
}

// returns 1 if one thread was woken up
static int futex_unlock(_Atomic int *f)
{
    int one = 1;
    if (atomic_compare_exchange_strong(f, &one, 0)) {
        int r = atomic_futex(f, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
        return r;
    } else {
        return -2;
    }
    return 0;
}



enum Method {
    METHOD_SPIN,
    METHOD_SPIN_PAUSE,
    METHOD_SPIN_PAUSE_MORE,
    METHOD_COND_VAR,
    METHOD_NULL,
    METHOD_PIPE,
    METHOD_FUTEX
};
typedef enum Method Method;
struct Args {
    uint32_t tsc_khz;
    uint32_t mult;
    uint32_t shift;
    unsigned n;    // number of iterations
    unsigned k; // number of pause iterations before each store
    unsigned p; // number of pause iterations after each test
    unsigned pin[2];
    bool json;
    Method method;
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
            "  --pin THREAD CPU  0 <= THREAD <= 1, pin each thread to a CPU/core\n"
            "                    (default: no pinning)\n"
            "  --json            write raw values to JSON file (default: false)\n"
            "  --spin            loop on an atomic variable (default)\n"
            "  --spin-pause      pause after each atomic load\n"
            "  -p                #pauses after each atomic load\n"
            "  --cv              use a condition variable for ping pong\n"
            "  --pipe            use a UNIX pipe for ping pong\n"
            "  --futex           use a Linux futex for ping pong\n"
            "  --null            signal nothing\n"
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
        } else if (!strcmp(argv[i], "-p")) {
            ++i;
            if (i >= argc) {
                fprintf(stderr, "-p argument is missing\n");
                return -1;
            }
            args->p = atoi(argv[i]);
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
        } else if (!strcmp(argv[i], "--spin")) {
            args->method = METHOD_SPIN;
        } else if (!strcmp(argv[i], "--spin-pause")) {
            args->method = METHOD_SPIN_PAUSE;
        } else if (!strcmp(argv[i], "--cv")) {
            args->method = METHOD_COND_VAR;
        } else if (!strcmp(argv[i], "--null")) {
            args->method = METHOD_NULL;
        } else if (!strcmp(argv[i], "--pipe")) {
            args->method = METHOD_PIPE;
        } else if (!strcmp(argv[i], "--futex")) {
            args->method = METHOD_FUTEX;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            exit(1);
        }
    }
    if (!args->n)
        args-> n = 1000 * 1000;
    if (!args->k)
        args-> k = 1000;
    if (args->method == METHOD_SPIN_PAUSE && args->p)
        args->method = METHOD_SPIN_PAUSE_MORE;
    return 0;
}

struct Worker {
    pthread_t worker_id;
    unsigned init; // 0 -> start with send, 1 -> start with retrieve
    unsigned n;    // number of iterations
    unsigned k;
    unsigned p;
    uint32_t *raw_ds;  // delta values
    uint32_t *ds;  // delta values
    unsigned ds_size; // #delta values
};
typedef struct Worker Worker;


static void *spin_main_finalize(Worker *x, uint32_t *ds, unsigned j)
{
    assert(j <= x->n/2);
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

    for (unsigned i = 0; i < w.n; ++i) {
        if (i % 2 == w.init) { // sender
            unsigned k = i < 2 ? w.k : w.k * 2;
            for (unsigned j = 0; j < k; ++j)
                _mm_pause();
            uint64_t t;
            for (;;) {
                t = fenced_rdtsc();
                if (t <= tsc)
                    continue;
                atomic_store_explicit(&g_cell[!w.init].tsc, t,
                        memory_order_release);
                break;
            }
        } else { // retriever
            uint64_t new_tsc;
            for (;;) {
                new_tsc = atomic_load_explicit(&g_cell[w.init].tsc,
                        memory_order_consume);
                if (new_tsc > tsc) {
                    break;
                }
            }
            uint64_t now   = fenced_rdtscp();
            uint64_t delta = now - new_tsc;
            ds[j++] = delta;
            tsc = new_tsc;
        }
    }
    return spin_main_finalize(x, ds, j);
}


static void *spin_null_main(void *p)
{
    Worker *x = (Worker*) p;
    Worker w = *x;

    unsigned j = 0;
    uint32_t *ds = calloc(w.n/2, sizeof ds[0]);
    if (!ds) {
        fprintf(stderr, "Failed to allocate delta array in thread\n");
        return 0;
    }

    while(!atomic_load_explicit(&start_work, memory_order_consume)) {
        _mm_pause();
    }

    for (unsigned i = 0; i < w.n/2; ++i) {
        uint64_t new_tsc = fenced_rdtsc();
        uint64_t now     = fenced_rdtscp();
        uint64_t delta   = now - new_tsc;
        ds[j++] = delta;
    }
    return spin_main_finalize(x, ds, j);
}

static void *spin_pause_main(void *p)
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

    for (unsigned i = 0; i < w.n; ++i) {
        if (i % 2 == w.init) { // sender
            unsigned k = i < 2 ? w.k : w.k * 2;
            for (unsigned j = 0; j < k; ++j)
                _mm_pause();
            uint64_t t;
            for (;;) {
                t = fenced_rdtsc();
                if (t <= tsc)
                    continue;
                atomic_store_explicit(&g_cell[!w.init].tsc, t,
                        memory_order_release);
                break;
            }
        } else { // retriever
            uint64_t new_tsc;
            for (;;) {
                new_tsc = atomic_load_explicit(&g_cell[w.init].tsc,
                        memory_order_consume);
                if (new_tsc > tsc) {
                    break;
                }
                _mm_pause();
            }
            uint64_t now   = fenced_rdtscp();
            uint64_t delta = now - new_tsc;
            ds[j++] = delta;
            tsc = new_tsc;
        }
    }
    return spin_main_finalize(x, ds, j);
}

static void *spin_pause_more_main(void *p)
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

    for (unsigned i = 0; i < w.n; ++i) {
        if (i % 2 == w.init) { // sender
            unsigned k = i < 2 ? w.k : w.k * 2;
            for (unsigned j = 0; j < k; ++j)
                _mm_pause();
            uint64_t t;
            for (;;) {
                t = fenced_rdtsc();
                if (t <= tsc)
                    continue;
                atomic_store_explicit(&g_cell[!w.init].tsc, t,
                        memory_order_release);
                break;
            }
        } else { // retriever
            uint64_t new_tsc;
            for (;;) {
                new_tsc = atomic_load_explicit(&g_cell[w.init].tsc,
                        memory_order_consume);
                if (new_tsc > tsc) {
                    break;
                }
                for (unsigned j = 0; j < w.p; ++j)
                    _mm_pause();
            }
            uint64_t now   = fenced_rdtscp();
            uint64_t delta = now - new_tsc;
            ds[j++] = delta;
            tsc = new_tsc;
        }
    }
    return spin_main_finalize(x, ds, j);
}


static void *cv_main(void *p)
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

    for (unsigned i = 0; i < w.n; ++i) {
        if (i % 2 == w.init) { // sender
            unsigned k = i < 2 ? w.k : w.k * 2;
            for (unsigned j = 0; j < k; ++j)
                _mm_pause();
            uint64_t t;
            for (;;) {
                t = fenced_rdtsc();
                if (t <= tsc)
                    continue;
                int r = pthread_mutex_lock(&g_item[!w.init].mutex);
                if (r) {
                    perror_e(r, "sender: mutex lock");
                    return 0;
                }
                g_item[!w.init].tsc = t;
                r = pthread_mutex_unlock(&g_item[!w.init].mutex);
                if (r) {
                    perror_e(r, "sender: mutex unlock");
                    return 0;
                }
                r = pthread_cond_signal(&g_item[!w.init].cond_var);
                if (r) {
                    perror_e(r, "cond signal: mutex lock");
                    return 0;
                }
                break;
            }
        } else { // retriever
            int r = pthread_mutex_lock(&g_item[w.init].mutex);
            if (r) {
                perror_e(r, "retrieve: mutex lock");
                return 0;
            }
            while (g_item[w.init].tsc <= tsc) {
                r = pthread_cond_wait(&g_item[w.init].cond_var,
                        &g_item[w.init].mutex);
                if (r) {
                    perror_e(r, "cond_wait");
                    return 0;
                }
            }
            uint64_t new_tsc = g_item[w.init].tsc;
            r = pthread_mutex_unlock(&g_item[w.init].mutex);
            if (r) {
                perror_e(r, "retrieve: mutex unlock");
                return 0;
            }
            uint64_t now   = fenced_rdtscp();
            uint64_t delta = now - new_tsc;
            ds[j++] = delta;
            tsc = new_tsc;
        }
    }
    return spin_main_finalize(x, ds, j);
}

static void *pipe_main(void *p)
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

    for (unsigned i = 0; i < w.n; ++i) {
        if (i % 2 == w.init) { // sender
            unsigned k = i < 2 ? w.k : w.k * 2;
            for (unsigned j = 0; j < k; ++j)
                _mm_pause();
            uint64_t t;
            for (;;) {
                t = fenced_rdtsc();
                if (t <= tsc)
                    continue;
                ssize_t l = write(g_pipes[!w.init][1], &t, sizeof t);
                if (l == -1) {
                    perror("pipe write");
                    return 0;
                }
                if (l != sizeof t) {
                    fprintf(stderr, "written into pipe less than expected\n");
                    return 0;
                }
                break;
            }
        } else { // retriever
            uint64_t new_tsc;
            ssize_t l = read(g_pipes[w.init][0], &new_tsc, sizeof new_tsc);
            if (l == -1) {
                perror("pipe read");
                return 0;
            }
            if (l != sizeof new_tsc) {
                fprintf(stderr, "read from pipe less than expected\n");
                return 0;
            }
            uint64_t now   = fenced_rdtscp();
            uint64_t delta = now - new_tsc;
            ds[j++] = delta;
            tsc = new_tsc;
        }
    }
    return spin_main_finalize(x, ds, j);
}

// note that this lock/unlock scheme doesn't work with posix mutexes
// because unlocking a locked posix mutex from a different thread
// is undefined behaviour
static void *futex_main(void *p)
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

    for (unsigned i = 0; i < w.n; ++i) {
        if (i % 2 == w.init) { // sender
            int r = futex_lock(&g_follicle[w.init].futex);
            if (r == -1 ) {
                perror("futex wait");
                return 0;
            }

            unsigned k = i < 2 ? w.k : w.k * 2;
            for (unsigned j = 0; j < k; ++j)
                _mm_pause();
            uint64_t t;
            for (;;) {
                t = fenced_rdtsc();
                if (t <= tsc)
                    continue;
                g_follicle[!w.init].tsc = t;
                int r = futex_unlock(&g_follicle[!w.init].futex);
                if (r == -1) {
                    perror("futex wake");
                    return 0;
                }
                if (r == -2) {
                    fprintf(stderr, "%u: unexpectedly unlocked\n", w.init);
                    abort();
                }
                break;
            }
        } else { // receiver
            uint64_t new_tsc;

            int r = futex_lock(&g_follicle[w.init].futex);
            if (r == -1 ) {
                perror("futex wait");
                return 0;
            }
            new_tsc = g_follicle[w.init].tsc;

            uint64_t now   = fenced_rdtscp();
            uint64_t delta = now - new_tsc;
            ds[j++] = delta;
            tsc = new_tsc;

            r = futex_unlock(&g_follicle[w.init].futex);
            if (r == -1 ) {
                perror("futex wake");
                return 0;
            }
            if (r == -2) {
                fprintf(stderr, "%u: unexpectedly unlocked\n", w.init);
                abort();
            }
        }
    }
    return spin_main_finalize(x, ds, j);
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
        ws[i].p = args->p;
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
        switch (args->method) {
            case METHOD_SPIN:
                r = pthread_create(&ws[i].worker_id, &attr, spin_main, ws+i);
                break;
            case METHOD_SPIN_PAUSE:
                r = pthread_create(&ws[i].worker_id, &attr, spin_pause_main,
                        ws+i);
                break;
            case METHOD_SPIN_PAUSE_MORE:
                r = pthread_create(&ws[i].worker_id, &attr,
                        spin_pause_more_main, ws+i);
                break;
            case METHOD_COND_VAR:
                r = pthread_create(&ws[i].worker_id, &attr, cv_main, ws+i);
                break;
            case METHOD_PIPE:
                r = pipe(g_pipes[i]);
                if (r == -1) {
                    perror("pipe");
                    return 1;
                }
                r = pthread_create(&ws[i].worker_id, &attr, pipe_main, ws+i);
                break;
            case METHOD_FUTEX:
                g_follicle[i].futex = i;
                r = pthread_create(&ws[i].worker_id, &attr, futex_main, ws+i);
                break;
            case METHOD_NULL:
                r = pthread_create(&ws[i].worker_id, &attr, spin_null_main,
                        ws+i);
                break;
        }
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
