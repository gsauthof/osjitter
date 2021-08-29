
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: © 2021 Georg Sauthoff <mail@gms.tf>

#include <benchmark/benchmark.h>

#include <unistd.h>
#include <sys/types.h>
#include <sched.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <sys/prctl.h>

#include <assert.h>


static void bench_getuid(benchmark::State& state) {
    for (auto _ : state) {
        getuid();
    }
}

BENCHMARK(bench_getuid);

static void bench_getpid(benchmark::State& state) {
    for (auto _ : state) {
        getpid();
    }
}

BENCHMARK(bench_getpid);

static void bench_close(benchmark::State& state) {
    for (auto _ : state) {
        close(999);
    }
}

BENCHMARK(bench_close);

static void bench_syscall(benchmark::State& state) {
    for (auto _ : state) {
        syscall(423);
    }
}

BENCHMARK(bench_syscall);

static void bench_sched_yield(benchmark::State& state) {
    for (auto _ : state) {
        sched_yield();
    }
}

BENCHMARK(bench_sched_yield);

static void bench_clock_gettime(benchmark::State& state) {
    struct timespec ts = {0};
    for (auto _ : state) {
        clock_gettime(CLOCK_REALTIME, &ts);
    }
}

BENCHMARK(bench_clock_gettime);

static void bench_clock_gettime_tai(benchmark::State& state) {
    struct timespec ts = {0};
    for (auto _ : state) {
        clock_gettime(CLOCK_TAI, &ts);
    }
}

BENCHMARK(bench_clock_gettime_tai);

static void bench_clock_gettime_monotonic(benchmark::State& state) {
    struct timespec ts = {0};
    for (auto _ : state) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
    }
}

BENCHMARK(bench_clock_gettime_monotonic);

static void bench_clock_gettime_monotonic_raw(benchmark::State& state) {
    struct timespec ts = {0};
    for (auto _ : state) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    }
}

BENCHMARK(bench_clock_gettime_monotonic_raw);

static void bench_nanosleep0(benchmark::State& state) {
    struct timespec ts = {0};
    for (auto _ : state) {
        int r = nanosleep(&ts, 0);
        assert(!r);
    }
}

BENCHMARK(bench_nanosleep0);

static void bench_nanosleep0_slack1(benchmark::State& state) {
    int r = prctl(PR_SET_TIMERSLACK, 1, 0, 0, 0);
    assert(!r);
    struct timespec ts = {0};
    for (auto _ : state) {
        int r = nanosleep(&ts, 0);
        assert(!r);
    }
}

BENCHMARK(bench_nanosleep0_slack1);

static void bench_nanosleep1_slack1(benchmark::State& state) {
    int r = prctl(PR_SET_TIMERSLACK, 1, 0, 0, 0);
    assert(!r);
    struct timespec ts = { .tv_nsec = 1 };
    for (auto _ : state) {
        int r = nanosleep(&ts, 0);
        assert(!r);
    }
}

BENCHMARK(bench_nanosleep1_slack1);

static void bench_pthread_cond_signal(benchmark::State& state) {
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    for (auto _ : state) {
        pthread_cond_signal(&cv);
    }
}

BENCHMARK(bench_pthread_cond_signal);

static void bench_assign(benchmark::State& state) {
    double f = 0;
    for (auto _ : state) {
        f = 23;
        benchmark::DoNotOptimize(f);
    }
}

BENCHMARK(bench_assign);

static void bench_sqrt(benchmark::State& state) {
    double f = 23;
    double g = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(f);
        g = sqrt(f);
        benchmark::DoNotOptimize(g);
    }
}

BENCHMARK(bench_sqrt);

static void bench_sqrtrec(benchmark::State& state) {
    double f = 23;
    for (auto _ : state) {
        f = sqrt(f);
    }
}

BENCHMARK(bench_sqrtrec);

static void bench_nothing(benchmark::State& state) {
    unsigned i = 0;
    for (auto _ : state) {
        ++i;
    }
}

BENCHMARK(bench_nothing);

BENCHMARK_MAIN();
