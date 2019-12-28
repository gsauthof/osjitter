
// 2019, Georg Sauthoff <mail@gms.tf>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include "util.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

void perror_e(int r, const char *msg)
{
    char buf[1024];
    fprintf(stderr, "%s: %s\n", msg, strerror_r(r, buf, sizeof buf));
}

static bool is_sorted(const uint32_t *xs, size_t n)
{
    if (!n)
        return true;
    uint32_t a = xs[0];
    for (size_t i = 1; i < n; ++i) {
        if (a > xs[i])
            return false;
        a = xs[i];
    }
    return true;
}

uint32_t percentile_u32(const uint32_t *x, size_t n, size_t a, size_t b)
{
    assert(is_sorted(x, n));

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
uint32_t mad_u32(const uint32_t *x, uint32_t *y, size_t n)
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
void clocks_calc_mult_shift(
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

static int get_tsc_khz_cmd(const char *cmd, uint32_t *tsc_khz)
{
    FILE *f = popen(cmd, "re");
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
    if (l < 11)
        return 0;
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

static int get_tsc_khz_journal(uint32_t *tsc_khz)
{

    const char cmd[] = "journalctl --boot 2>/dev/null | grep 'kernel: tsc:' -i "
            "| cut -d' ' -f5- | grep -o ' [0-9]\\+\\.[0-9]\\{3\\} MHz' "
            "| tail -n 1 ";
    return get_tsc_khz_cmd(cmd, tsc_khz);
}

// fall-back to dmesg on systems without journald or ones
// where the user doesn't have enough permissions for journalctl --boot.
// pitfall: the message might be already rotated out of the dmesg buffer,
// on a long running system
static int get_tsc_khz_dmesg(uint32_t *tsc_khz)
{
    const char cmd[] = "dmesg  | grep '\\] tsc:' -i"
            "| cut -d' ' -f5- | grep -o ' [0-9]\\+\\.[0-9]\\{3\\} MHz' "
            "| tail -n 1 ";
    return get_tsc_khz_cmd(cmd, tsc_khz);
}

// see also https://stackoverflow.com/a/57835630/427158 for
// some ways to get the tick rate of the TSC
int get_tsc_khz(uint32_t *tsc_khz)
{
    int r = get_tsc_khz_proc(tsc_khz);
    if (r < 0)
        return r;
    if (!*tsc_khz) {
        int r = get_tsc_khz_journal(tsc_khz);
        if (r < 0)
            return r;
    }
    if (!*tsc_khz) {
        int r = get_tsc_khz_dmesg(tsc_khz);
        if (r < 0)
            return r;
    }
    if (!*tsc_khz) {
        fprintf(stderr, "Couldn't determine TSC rate\n");
        return -1;
    }
    return 0;
}
