
// OSjitter - measure program interruptions
//
// 2019, Georg Sauthoff <mail@gms.tf>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <x86intrin.h> // __rdtsc(), __rdtscp(), _mm_lfence()
#include <xmmintrin.h> // __mm_pause()


static atomic_bool start_work  = false;
static atomic_bool quit_thread = false;


struct Args {
    uint32_t  cpus;
    cpu_set_t cpu_set;

    int sched_policy;
    int sched_prio;

    uint32_t runtime_s;
    uint32_t thresh_ns;
 
    uint32_t tsc_khz;
    uint32_t mult;
    uint32_t shift;
    uint32_t tsc_thresh;
    uint64_t tsc_runtime;
    uint64_t samples;

    unsigned pid;
    size_t tid_off;
};
typedef struct Args Args;

static void help(FILE *f, const char *argv0)
{
    fprintf(f, "%s - measure involuntary program interruptions\n"
        "\n"
        "Options:\n"
        "  -t SEC     measurement period in s (default: 10 s)\n"
        "  -d NS      threshold for an interruption in ns (default: 100 ns)\n"
        "  --cpu X    CPU (Cores) that are part of the measurement (default: all);\n"
        "  --cpu X-Y  count from zero, single core or range\n"
        "  --sched X  scheduling policy for measurement threads (default: OTHER);\n"
        "             1:FIFO, 2:RR etc. WARNING: only specify a subset with --cpu\n"
        "             when setting a realtime policy\n"
        "  --prio X   realtime priority (default: 1)\n"
        "  --khz  X   frequency of TSC in kHz (default: read from\n"
        "             /sys/devices/system/cpu/cpu0/tsc_freq_khz if available or\n"
        "             journalctl --boot)\n"
        "\n"
        "How it works: a measurement thread is pinned on each selected CPU\n"
        "where it loops without making system calls and periodically reads\n"
        "the TSC to detect external interruptions. Thus, it detects latency\n"
        "introducing interruptions by the OS and possibly even by the SMM.\n"
        "\n"
        "Output columns:\n"
        "  CPU         - CPU/Core number, count from 0, cf. /proc/cpuinfo and lscpu\n"
        "  TSC_KHZ     - frequency of the Time Stamp Counter (TSC)\n"
        "                might be different from the CPU's base frequency\n"
        "  #intr       - number of interruptions (above the threshold, cf. -d)\n"
        "  #delta      - number of recorded interruptions (might overflow)\n"
        "  ovfl_ns     - time after which interrupt recording overflowed\n"
        "  invol_ctx   - number of involuntary context switches\n"
        "                (i.e. due to scheduling)\n"
        "  sum_intr_ns - sum of all interruptions in ns\n"
        "  iratio      - ratio of interruption time to runtime\n"
        "                (IOW off-program to program time)\n"
        "  rt_s        - measurement time in s (cf. -t)\n"
        "  loop_ns     - smallest loop runtime (likely of an uninterrupted iteration\n"
        "                is used to better approximate interruption time\n"
        "  median_ns   - Median of all recorded interruptions\n"
        "  pX_ns       - X/100 percentile\n"
        "  max_ns      - the longest interruption\n"
        "  mad_ns      - median absolute deviation of all recorded interruptions\n"
        "\n"
        "How much happens in a nanosecond?\n"
        "A CPU running at 3.6 GHz progresses by 3.6 cycles in 1 ns. And a\n"
        "modern pipelined super-scalar CPU may execute up to 3 instructions\n"
        "or so per cycle, on average.\n"
        "\n"
        "2019, Georg Sauthoff <mail@gms.tf>, GPLv3+\n"
        , argv0);
}

static int parse_args(Args *args, int argc, char **argv)
{
    *args = (const Args){0};
    CPU_ZERO(&args->cpu_set);

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu")) {
            ++i;
            if (i >= argc) {
                fprintf(stderr, "--cpu argument is missing\n");
                return -1;
            }
            char *p = strchr(argv[i], '-');
            if (p) { 
                *p = 0;
                unsigned b = atoi(argv[i]);
                unsigned e = atoi(p+1);
                if (b >= 1024 || e >= 1024) {
                    fprintf(stderr, "--cpu range out of range\n");
                    return -1;
                }
                for (unsigned k = b; k <= e; ++k) {
                    CPU_SET(k, &args->cpu_set);
                }
            } else {
                CPU_SET(atoi(argv[i]), &args->cpu_set);
            }
        } else if (!strcmp(argv[i], "-t")) {
            ++i;
            if (i >= argc) {
                fprintf(stderr, "-t argument is missing\n");
                return -1;
            }
            args->runtime_s = atoi(argv[i]);
        } else if (!strcmp(argv[i], "-d")) {
            ++i;
            if (i >= argc) {
                fprintf(stderr, "-d argument is missing\n");
                return -1;
            }
            args->thresh_ns = atoi(argv[i]);
        } else if (!strcmp(argv[i], "--sched")) {
            ++i;
            if (i >= argc) {
                fprintf(stderr, "--sched argument is missing\n");
                return -1;
            }
            args->sched_policy = atoi(argv[i]);
            if (!args->sched_prio)
                args->sched_prio = 1;
        } else if (!strcmp(argv[i], "--prio")) {
            ++i;
            if (i >= argc) {
                fprintf(stderr, "--prio argument is missing\n");
                return -1;
            }
            args->sched_prio = atoi(argv[i]);
        } else if (!strcmp(argv[i], "--khz")) {
            ++i;
            if (i >= argc) {
                fprintf(stderr, "--khz argument is missing\n");
                return -1;
            }
            args->tsc_khz = atoi(argv[i]);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            help(stdout, argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return -1;
        }
    }

    if (!args->runtime_s)
        args->runtime_s = 10;
    if (!args->thresh_ns)
        args->thresh_ns = 100;
    if (!args->samples)
        args->samples = args->runtime_s * 105000;

    return 0;
}

// This function is copied from
// https://elixir.bootlin.com/linux/v5.2.12/source/kernel/time/clocksource.c#L21
// File license: GPL-2.0+
// slightly modified
/**
 * clocks_calc_mult_shift - calculate mult/shift factors for scaled math of clocks
 * @mult:	pointer to mult variable
 * @shift:	pointer to shift variable
 * @from:	frequency to convert from
 * @to:		frequency to convert to
 * @maxsec:	guaranteed runtime conversion range in seconds
 *
 * The function evaluates the shift/mult pair for the scaled math
 * operations of clocksources and clockevents.
 *
 * @to and @from are frequency values in HZ. For clock sources @to is
 * NSEC_PER_SEC == 1GHz and @from is the counter frequency. For clock
 * event @to is the counter frequency and @from is NSEC_PER_SEC.
 *
 * The @maxsec conversion range argument controls the time frame in
 * seconds which must be covered by the runtime conversion with the
 * calculated mult and shift factors. This guarantees that no 64bit
 * overflow happens when the input value of the conversion is
 * multiplied with the calculated mult factor. Larger ranges may
 * reduce the conversion accuracy by chosing smaller mult and shift
 * factors.
 */
static void clocks_calc_mult_shift(
        uint32_t *mult, uint32_t *shift, uint32_t from, uint32_t to,
        uint32_t maxsec)
{
	uint64_t tmp;
	uint32_t sft, sftacc= 32;

	/*
	 * Calculate the shift factor which is limiting the conversion
	 * range:
	 */
	tmp = ((uint64_t)maxsec * from) >> 32;
	while (tmp) {
		tmp >>=1;
		sftacc--;
	}

	/*
	 * Find the conversion shift/mult pair which has the best
	 * accuracy and fits the maxsec conversion range:
	 */
	for (sft = 32; sft > 0; sft--) {
		tmp = (uint64_t) to << sft;
		tmp += from / 2;
		// do_div(tmp, from);
                tmp = tmp / (uint64_t) from;
                
		if ((tmp >> sftacc) == 0)
			break;
	}
	*mult = tmp;
	*shift = sft;
}

// Linux Kernel has a function that is named the same
static uint64_t mul_u64_u32_shr(uint64_t cyc, uint32_t mult, uint32_t shift)
{
    __uint128_t x = cyc;
    x *= mult;
    x >>= shift;
    return x;
}

// as of Kernel 5.2.7 /sys/devices/system/cpu/cpu0/tsc_freq_khz
// isn't provided by the mainline kernel
// see https://github.com/trailofbits/ 
// or even better https://github.com/trailofbits/tsc_freq_khz/pull/1
// for a simple kernel module that provides this file
static int get_tsc_khz_proc(uint32_t *tsc_khz)
{
    int fd = open("/sys/devices/system/cpu/cpu0/tsc_freq_khz", O_RDONLY);
    if (fd == -1) {
        if (errno == ENOENT)
            return 1;
        perror("opening /sys/devices/system/cpu/cpu0/tsc_freq_khz");
        return -1;
    }
    char buf[16];
    ssize_t r = read(fd, buf, sizeof buf - 1);
    if (r == -1) {
        perror("reading /sys/devices/system/cpu/cpu0/tsc_freq_khz");
        close(fd);
        return -1;
    }
    buf[r] = 0;
    if (r && buf[r-1] == '\n')
        buf[r-1] = 0;
    *tsc_khz = atoi(buf);
    int t = close(fd);
    if (t == -1) {
        perror("closing /sys/devices/system/cpu/cpu0/tsc_freq_khz");
        return -1;
    }
    return 0;
}

static int get_tsc_khz_journal(uint32_t *tsc_khz)
{
    FILE *f = popen("journalctl --boot | grep 'kernel: tsc:' -i "
            "| cut -d' ' -f5- | grep -o ' [0-9]\\+\\.[0-9]\\{3\\} MHz' "
            "| tail -n 1 ", "re");
    if (!f) {
        perror("reading TSC khz from journalctl failed");
        return 1;
    }
    char *line = 0;
    size_t n = 0;
    ssize_t l = getline(&line, &n, f);
    if (l == -1) {
        if (!feof(f)) {
            perror("journal getline");
            pclose(f);
            return -1;
        }
    }
    if (l > 15 + 7) {
        fprintf(stderr, "buffer for TSC khz from journal too small\n");
        return -1;
    }
    char buf[16];
    char *t = mempcpy(buf, line+1, l-1-8-1);
    t = mempcpy(t, line+(l-7-1), 3);
    *t = 0;
    *tsc_khz = atoi(buf);
    int r = pclose(f);
    if (r == -1) {
        perror("pclose journal");
        return -1;
    }
    return 0;
}

static int is_cpu_online(uint32_t cpu, bool *b)
{
    char filename[64];
    snprintf(filename, sizeof filename, "/sys/devices/system/cpu/cpu%u/online",
            cpu);
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        // CPU not hot-plugable
        if (errno == ENOENT) {
            *b = true;
            return 0;
        }
        perror("opening /sys/devices/system/cpu/cpu%u/online");
        return -1;
    }
    char buf[2] = {0};
    ssize_t r = read(fd, buf, sizeof buf);
    if (r == -1) {
        perror("reading /sys/devices/system/cpu/cpu0/tsc_freq_khz");
        close(fd);
        return -1;
    }
    *b = buf[0] == '1' && buf[1] == '\n';
    int t = close(fd);
    if (t == -1) {
        perror("closing /sys/devices/system/cpu/cpu%u/online");
        return -1;
    }
    return 0;
}


// cf. gdb> ptype pthread
// (requires glibc debuginfo installed)
static size_t get_tid_off(void)
{
    pthread_t t = pthread_self();
    const char *b;
    memcpy(&b, (void*)t, sizeof b);
    const char *e = b + 1024;
    unsigned pid = getpid();
    for (const char *p = b + 128; p < e; p+=4) {
        unsigned x;
        memcpy(&x, p, sizeof x);
        if  (x == pid)
            return p - b;
    }
    return 0;
}

// alternative to calling gettid() in each child
static unsigned pthread_to_tid(pthread_t t, size_t off)
{
    const char *p;
    memcpy(&p, (void*)t, sizeof p);
    unsigned tid;
    memcpy(&tid, p + off, sizeof tid);
    return tid;
}

static int set_params(Args *args)
{
    args->pid = getpid();
    args->tid_off = get_tid_off();

    args->cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (!CPU_COUNT(&args->cpu_set)) {
        for (unsigned k = 0; k <= args->cpus; ++k) {
            bool b = false;
            int r = is_cpu_online(k, &b);
            if (r)
                return r;
            if (b)
                CPU_SET(k, &args->cpu_set);
        }
    }

    // see also https://stackoverflow.com/a/57835630/427158 for
    // some ways to get the tick rate of the TSC
    if (!args->tsc_khz) {
        int r = get_tsc_khz_proc(&args->tsc_khz);
        if (r < 0)
            return r;
    }
    if (!args->tsc_khz) {
        int r = get_tsc_khz_journal(&args->tsc_khz);
        if (r < 0)
            return r;
    }

    if (!args->tsc_khz) {
        fprintf(stderr, "Couldn't determine TSC rate\n");
        return -1;
    }
    clocks_calc_mult_shift(&args->mult, &args->shift,
            args->tsc_khz, 1000000l, 0);
    {
        double d = 1000000000l;
        d /= args->thresh_ns;
        double e = args->tsc_khz;
        e *= 1000;
        e /= d;
        args->tsc_thresh = (uint32_t) e;
    }
    {
        double d = args->tsc_khz;
        d *= 1000;
        d *= args->runtime_s;
        args->tsc_runtime = (uint64_t) d;
    }
    return 0;
}

static Args global_args;

struct Worker {
    pthread_t worker_id;
    unsigned  tid;
    uint32_t  cpu_id;

    uint32_t *deltas;       // array of interruptions
    uint64_t samples;       // #used array entries
    uint64_t thresh_cnt;    // counted interruptions

    uint64_t tsc_start;     // start of measurements
    uint64_t tsc_overflow;  // when it overflowed (or 0 for no overflow)
    uint64_t tsc_total_int; // sum of interruptions
    uint64_t tsc_delta_min; // minimum loop time

    uint64_t invol_switch;  // involuntary context switches
};
typedef struct Worker Worker;

static int check_cpuinfo(void)
{
    FILE *f = popen("grep '^flags' /proc/cpuinfo | tr ' ' '\\n'"
            " | grep '^\\(constant\\|nonstop\\)_tsc$'", "re");
    if (!f) {
        perror("popen");
        return -1;
    }
    char *line = 0;
    size_t n = 0;
    bool constant_tsc = false;
    bool nonstop_tsc  = false;
    for (;;) {
        ssize_t l = getline(&line, &n, f);
        if (l == -1) {
            if (feof(f)) {
                break;
            } else {
                perror("getline");
                pclose(f);
                return -1;
            }
        }
        if (!strcmp(line, "constant_tsc\n"))
            constant_tsc = true;
        if (!strcmp(line, "nonstop_tsc\n"))
            nonstop_tsc = true;
    }
    int r = pclose(f);
    if (r == -1) {
        perror("pclose");
        return -1;
    }
    r = 0;
    if (!constant_tsc) {
        fprintf(stderr, "CPU doesn't support a constant TSC\n");
        r = 1;
    }
    if (!nonstop_tsc) {
        fprintf(stderr, "CPU's TSC stops in sleep states\n");
        r = 1;
    }
    return r;
}

// Note that /proc/%u/task/%u/sched is gone after the thread
// returned from its main function,
// i.e. even before the parent called pthread_join()
static int read_proc_sched(unsigned pid, unsigned tid, Worker *w)
{
    char filename[64];
    snprintf(filename, sizeof filename, "/proc/%u/task/%u/sched", pid, tid);
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        perror("opening /proc/%u/task/%u/sched");
        return -1;
    }
    char buf[4*1024] = {0};
    ssize_t n = read(fd, buf, sizeof buf);
    if (n == -1) {
        perror("reading /proc/%u/task/%u/sched");
        close(fd);
        return -1;
    }
    const char q[] = "nr_involuntary_switches";
    char *p = memmem(buf, n, q, sizeof q - 1);
    if (!p) {
        fprintf(stderr,
                "Couldn't find involuntary switches in /proc/.../sched\n");
        return -1;
    }
    p += sizeof q - 1;
    char *e = memchr(p, '\n', n - (p-buf));
    if (!e) {
        fprintf(stderr, "Couldn't find end in /proc/.../sched\n");
        return -1;
    }
    *e = 0;
    char *m = memrchr(p, ' ', e-p);
    if (!m) {
        fprintf(stderr, "Couldn't find begin in /proc/.../sched\n");
        return -1;
    }
    ++m;
    w->invol_switch = atol(m);
    int r = close(fd);
    if (r == -1) {
        perror("closing /proc/%u/task/%u/sched");
        return -1;
    }
    return 0;
}


// Read Time-Stamp Counter
extern __inline uint64_t __attribute__((__gnu_inline__, __always_inline__, __artificial__))
fenced_rdtsc(void)
{
    uint64_t x;
    asm volatile (
        ".intel_syntax noprefix  \n\t" // switch to prettier syntax
        // 'If software requires RDTSC to be executed only after all previous
        // instructions have executed and all previous loads and stores are
        // globally visible, it can execute the sequence MFENCE;LFENCE
        // immediately before RDTSC.'
        // https://www.felixcloutier.com/x86/rdtsc
        "mfence                  \n\t"
        "lfence                  \n\t"
        // similar effect, execute CPUID before RDTSC
        // cf. https://www.intel.de/content/dam/www/public/us/en/documents/white-papers/ia-32-ia-64-benchmark-code-execution-paper.pdf
        //"cpuid                   \n\t" // writes to EAX, EBX, ECX, EDX
        "rdtsc                   \n\t" // counter into EDX:EAX
        "shl     rdx, 0x20       \n\t" // shift higher-half left
        "or      rax, rdx        \n\t" // combine them
        ".att_syntax prefix      \n\t" // switch back to the default syntax

        : "=a" (x)       // output operands,
                         // i.e. overwrites (=)  R'a'X which is mapped to x
        :                // input operands
        : "rdx");        // additional clobbers (with cpuid also: rbx, rcx)
    return x;
}
// Read Time-Stamp Counter and Processor ID
// 'The RDTSCP instruction is not a serializing instruction, but it does wait
// until all previous instructions have executed and all previous loads are
// globally visible.'
// https://www.felixcloutier.com/x86/rdtscp
extern __inline uint64_t __attribute__((__gnu_inline__, __always_inline__, __artificial__))
fenced_rdtscp(void)
{
    uint64_t x;
    asm volatile (
        ".intel_syntax noprefix  \n\t"
        "rdtscp                  \n\t" // counter into EDX:EAX, id into ECX
        // 'If software requires RDTSCP to be executed prior to execution of
        // any subsequent instruction (including any memory accesses), it can
        // execute LFENCE immediately after RDTSCP.'
        // https://www.felixcloutier.com/x86/rdtscp
        "lfence                  \n\t" // better than CPUID
        // alternatively call CPUID (clobbers more registers, though)
        // cf. https://www.intel.de/content/dam/www/public/us/en/documents/white-papers/ia-32-ia-64-benchmark-code-execution-paper.pdf
        "shl     rdx, 0x20       \n\t" // shift higher-half left
        "or      rax, rdx        \n\t" // combine them
        ".att_syntax prefix      \n\t"

        : "=a" (x)       // output operands,
                         // i.e. overwrites (=)  R'a'X which is mapped to x
        :                // input operands
        : "rdx", "rcx"); // additional clobbers
    return x;
}

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t *x = a;
    const uint32_t *y = b;
    return *x - *y;
}

static void *worker_main(void *p)
{
    Worker *w = p;
    Args args = global_args;
    size_t n  = args.samples;
    // uint32_t is big enough to store interruptions of up to ~ 1 s
    // when using a TSC that runs at 4 GHz
    uint32_t *ds = calloc(n, sizeof ds[0]);
    if (!ds) {
        fprintf(stderr, "Failed to allocate delta array on core %" PRIu32 "\n",
                w->cpu_id);
        return NULL;
    }
    size_t i =  0;
    while(!atomic_load_explicit(&start_work, memory_order_consume)) {
        _mm_pause();
    }
    for (unsigned i = 0; i < 1000; ++i)
        _mm_pause();

    uint64_t tsc_total_int = 0;
    uint64_t tsc_overflow  = 0;
    uint64_t tsc_thresh    = args.tsc_thresh;
    uint64_t tsc_delta_min = UINT64_MAX;

    uint64_t start = fenced_rdtsc();
    uint64_t limit = start + args.tsc_runtime;
    uint64_t tsc   = start;

    // unroll the loop one time for a more 'realistic' tsc_delta_min
    if (tsc < limit) {
        uint64_t t     = fenced_rdtscp();
        uint64_t delta = t - tsc;
        tsc = t;
        if (delta > tsc_thresh) {
            tsc_total_int += delta;
            if (i < n) {
                ds[i] = delta > UINT32_MAX ? UINT32_MAX : delta;
            } else if (!tsc_overflow) {
                tsc_overflow = t;
            }
            ++i;
        }
        if  (delta < tsc_delta_min)
            tsc_delta_min = delta;
    }
    tsc_delta_min = UINT64_MAX; // throw the first tsc_delta_min away
    while (tsc < limit) {
        uint64_t t     =  fenced_rdtscp();
        uint32_t delta = t - tsc;
        tsc = t;
        if (delta > tsc_thresh) {
            tsc_total_int += delta;
            if (i < n) {
                ds[i] = delta > UINT32_MAX ? UINT32_MAX : delta;
            } else if (!tsc_overflow) {
                tsc_overflow = t;
            }
            ++i;
        }
        if  (delta < tsc_delta_min)
            tsc_delta_min = delta;
    }

    while(!atomic_load_explicit(&quit_thread, memory_order_consume)) {
        _mm_pause();
    }

    w->deltas        = ds;
    w->samples       = i < n ? i : n;
    w->thresh_cnt    = i;
    w->tsc_start     = start;
    w->tsc_overflow  = tsc_overflow;
    w->tsc_total_int = tsc_total_int - (tsc_delta_min*i);
    w->tsc_delta_min = tsc_delta_min;

    for (size_t i = 0; i < w->samples; ++i) {
        // Assuming that we have some loop iterations without any interruption
        w->deltas[i] -= w->tsc_delta_min;
    }
    qsort(w->deltas, w->samples, sizeof w->deltas[0], cmp_u32);

    // no need release/consume/aquire those values because
    // the main thread calls pthread_join() before reading those values
    // which acts as a memory barrier

    return w;
}

static void perror_e(int r, const char *msg)
{
    char buf[1024];
    fprintf(stderr, "%s: %s\n", msg, strerror_r(r, buf, sizeof buf));
}

static uint32_t percentile_u32(const uint32_t *x, size_t n, size_t a, size_t b)
{
    if (!n)
        return 0;
    size_t i = n * a / b;
    assert(i < n);
    if (n % 2 || !i) {
        return x[i];
    } else {
        assert(i);
        return (x[i] + x[i-1])/2;
    }
}

// median absolute deviation
// a measure of dispersion (like the standard deviation)
static uint32_t mad_u32(const uint32_t *x, uint32_t *y, size_t n)
{
    if (!n)
        return 0;
    uint32_t median = percentile_u32(x, n, 1, 2);
    for (size_t i = 0; i < n; ++i) {
        y[i] = labs((long)x[i] - (long)median);
    }
    qsort(y, n, sizeof y[0], cmp_u32);
    uint32_t mad = percentile_u32(y, n, 1, 2);
    return mad;
}

static int pp_results(const Worker *ws, FILE *f)
{
    Args *args = &global_args;
    fprintf(f, " CPU  TSC_khz  #intr  #delta  ovfl_ns  invol_ctx  sum_intr_ns  iratio  rt_s  loop_ns  median_ns  p20_ns  p80_ns  p90_ns  p99_ns  p99.9_ns   max_ns  mad_ns\n");
    uint32_t *ys = 0;
    for (unsigned cpu = 0; cpu < args->cpus; ++cpu) {
        if (!CPU_ISSET(cpu, &args->cpu_set))
            continue;
        const Worker *w = ws+cpu;
        uint64_t intr_ns = mul_u64_u32_shr(w->tsc_total_int,
                args->mult, args->shift);
        ys = realloc(ys, w->samples * sizeof ys[0]);
        if (!ys) {
            fprintf(stderr, "realloc in pp_results failed\n");
            return -1;
        }
        uint32_t mad = mad_u32(w->deltas, ys, w->samples);
        fprintf(f, "%4u %8" PRIu32 " %6" PRIu64 " %7" PRIu64
                " %8" PRIu64
                " %10" PRIu64
                " %12" PRIu64 " %7.3f"
                " %5" PRIu32
                " %8" PRIu64
                " %10" PRIu64
                " %7" PRIu64
                " %7" PRIu64
                " %7" PRIu64
                " %7" PRIu64
                " %9" PRIu64
                " %8" PRIu64
                " %7" PRIu64
                "\n",
                cpu, args->tsc_khz, w->thresh_cnt, w->samples,
                w->tsc_overflow ? mul_u64_u32_shr(w->tsc_overflow - w->tsc_start,
                    args->mult, args->shift) : 0,
                w->invol_switch,
                intr_ns, (double)intr_ns/((double)args->runtime_s*1000000000),
                args->runtime_s,
                mul_u64_u32_shr(w->tsc_delta_min, args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->deltas, w->samples, 1, 2),
                    args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->deltas, w->samples, 1, 5),
                    args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->deltas, w->samples, 4, 5),
                    args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->deltas, w->samples, 90, 100),
                    args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->deltas, w->samples, 99, 100),
                    args->mult, args->shift),
                mul_u64_u32_shr(percentile_u32(w->deltas, w->samples, 999, 1000),
                    args->mult, args->shift),
                mul_u64_u32_shr(w->samples ? w->deltas[w->samples - 1] : 0,
                        args->mult, args->shift),
                mul_u64_u32_shr(mad, args->mult, args->shift)
               );
    }
    free(ys);
    return 0;
}

static int create_workers(Worker *ws)
{
    Args *args = &global_args;
    for (unsigned cpu = 0; cpu < args->cpus; ++cpu) {
        ws[cpu].cpu_id = cpu;
        // => no need to synchronize this thread parameter because pthread_join
        // acts as a memory barrier
        if (!CPU_ISSET(cpu, &args->cpu_set))
            continue;

        pthread_attr_t attr;
        int r = pthread_attr_init(&attr);
        if (r) {
            perror_e(r, "pthread_attr_init failed");
            return 1;
        }
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(cpu, &cpus);
        r = pthread_attr_setaffinity_np(&attr, sizeof cpus, &cpus);
        if (r) {
            perror_e(r, "pthread_attr_setaffinity_np failed");
            return 1;
        }
        if (args->sched_policy) {
            r = pthread_attr_setschedpolicy(&attr, args->sched_policy);
            if (r) {
                perror_e(r, "pthread_attr_setschedpolicy failed");
                return 1;
            }
            // without any prio pthread_create complains about 'Invalid argument'
            struct sched_param param = { .sched_priority = args->sched_prio };
            r = pthread_attr_setschedparam(&attr, &param);
            if (r) {
                perror_e(r, "pthread_attr_setschedparam failed");
                return 1;
            }
            r = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
            if (r) {
                perror_e(r, "pthread_attr_setinheritsched failed");
                return 1;
            }
        }
        r = pthread_create(&ws[cpu].worker_id, &attr, worker_main, ws+cpu);
        if (r) {
            perror_e(r, "pthread_create failed");
            return 1;
        }
        ws[cpu].tid = pthread_to_tid(ws[cpu].worker_id, args->tid_off);
        if (!ws[cpu].tid) {
            fprintf(stderr, "Couldn't get TID of created thread\n");
            return 1;
        }
        r = pthread_attr_destroy(&attr);
        if (r) {
            perror_e(r, "pthread_attr_init failed");
            return 1;
        }
    }
    return 0;
}

static int join_workers(Worker *ws)
{
    Args *args = &global_args;
    bool error_in_thread = false;
    for (unsigned cpu = 0; cpu < args->cpus; ++cpu) {
        if (!CPU_ISSET(cpu, &args->cpu_set))
            continue;
        void *w_ret = 0;
        int r = pthread_join(ws[cpu].worker_id, &w_ret);
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
    return 0;
}


int main(int argc, char **argv)
{
    int r = check_cpuinfo();
    if (r) {
        fprintf(stderr, "CPU doesn't have constant_tsc+nonstop_tsc features\n");
        return 1;
    }
    Args *args = &global_args;
    r = parse_args(args, argc, argv);
    if (r) {
        fprintf(stderr, "Parsing arguments failed\n");
        return 1;
    }
    r = set_params(args);
    if (r) {
        fprintf(stderr, "Setting parameters failed\n");
        return 1;
    }


    Worker *ws = calloc(args->cpus, sizeof ws[0]);
    if (!ws) {
        perror("workers allocation");
        return 1;
    }
    r = create_workers(ws);
    if (r) {
        return 1;
    }

    atomic_store_explicit(&start_work, true, memory_order_release);

    struct timespec ts = { .tv_sec = args->runtime_s, .tv_nsec = 100 * 1000};
    r = nanosleep(&ts, NULL);
    if (r == -1) {
        perror("sleep of control thread was interrupted");
        return 1;
    }

    for (unsigned cpu = 0; cpu < args->cpus; ++cpu) {
        if (!CPU_ISSET(cpu, &args->cpu_set))
            continue;
        int r = read_proc_sched(args->pid, ws[cpu].tid, ws + cpu);
        if (r) {
            return 1;
        }
    }

    atomic_store_explicit(&quit_thread, true, memory_order_release);

    r = join_workers(ws);
    if (r) {
        return 1;
    }

    r = pp_results(ws, stdout);
    if (r) {
        return 1;
    }

    free(ws);

    return 0;
}
