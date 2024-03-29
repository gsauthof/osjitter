// Check what methods are available for PTP offset calculation
// and how they perform.
//
// 2020, Georg Sauthoff <mail@gms.tf>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>


#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <linux/ptp_clock.h>
#include "ptp-clock-future.h"


// for Solarflare private clock offset ioctl
#include <linux/sockios.h> // SIOCDEVPRIVATE
#include <net/if.h>        // ifreq
#include <sys/types.h>     // socket()
#include <sys/socket.h>    // socket()
#include <string.h>        // strcpy()
#include <unistd.h>        // close()


#include <linux/ethtool.h> // ethtool_ts_info
#include <linux/sockios.h> // SIOCETHTOOL


#include "tsc.h"
#include "util.h"


// as of 2020
static int64_t tai_off_ns = 37000000000l;

#ifndef PCO_READ_PERF
static uint32_t tsc_khz;
#endif
static uint32_t tsc_mult;
static uint32_t tsc_shift;





static int64_t pct2ns(const struct ptp_clock_time *ptc)
{
    return (int64_t)(ptc->sec * 1000000000) + (int64_t)ptc->nsec;
}
static int64_t pct2ns_tai(const struct ptp_clock_time *ptc)
{
    return pct2ns(ptc) + tai_off_ns;
}

static int64_t ts2ns(const struct timespec *ts)
{
    return (int64_t)(ts->tv_sec * 1000000000) + (int64_t)ts->tv_nsec;
}
static int64_t ts2ns_tai(const struct timespec *ts)
{
    return ts2ns(ts) + tai_off_ns;
}

static uint64_t tsc2ns(uint64_t cyc)
{
    return mul_u64_u32_shr(cyc, tsc_mult, tsc_shift);
}

// these 2 lines are from linuxptp's missing.h
#define CLOCKFD 3
#define FD_TO_CLOCKID(fd)	((clockid_t) ((((unsigned int) ~fd) << 3) | CLOCKFD))

static int read_clock_offset(int fd)
{
    int r[3];
    struct timespec ts[3];
    clockid_t clk_id = FD_TO_CLOCKID(fd);
    for (int i = 0; i < 5; ++i) {
        r[0] = clock_gettime(CLOCK_REALTIME, ts);
        r[1] = clock_gettime(clk_id, ts+1);
        r[2] = clock_gettime(CLOCK_REALTIME, ts+2);
        if (r[0] == -1) {
            perror("clock_gettime CLOCK_REALTIME 1");
            return 1;
        }
        if (r[1] == -1) {
            perror("clock_gettime ptp");
            return 1;
        }
        if (r[2] == -1) {
            perror("clock_gettime CLOCK_REALTIME 2");
            return 1;
        }
        int64_t delay = ts2ns_tai(ts + 2) - ts2ns_tai(ts);
        int64_t off  = (ts2ns_tai(ts) + ts2ns_tai(ts + 2)) / 2 - ts2ns(ts + 1);
        printf("clock_gettime no %u: %" PRId64 " ns, delay: %" PRId64 " ns\n",
                i+1, off, delay);
    }
    return 0;
}

static int read_ptp_offset(int fd)
{
    struct ptp_sys_offset pso = { .n_samples =  5};
    uint64_t b = fenced_rdtsc();
    int r = ioctl(fd, PTP_SYS_OFFSET, &pso);
    uint64_t e = fenced_rdtscp();
    if (r) {
        perror("PTP_SYS_OFFSET");
        return 1;
    }
    uint64_t sc_delay = tsc2ns(e - b);
    unsigned k = 1;
    for (unsigned i = 0; i < pso.n_samples * 2; i+=2, ++k) {
        int64_t delay = pct2ns_tai(pso.ts + i+2) - pct2ns_tai(pso.ts + i);
        int64_t off  = (pct2ns_tai(pso.ts + i) + pct2ns_tai(pso.ts + i+2)) / 2 - pct2ns(pso.ts + i+1);
        printf("PTP_SYS_OFFSET no %u: %" PRId64 " ns, delay: %" PRId64 " ns, syscall: %" PRIu64 " ns\n",
                k, off, delay, sc_delay);
    }
    return 0;
}

static int read_ptp_offset_extended(int fd)
{
    struct ptp_sys_offset_extended psoe = { .n_samples =  5};
    uint64_t b = fenced_rdtsc();
    int r = ioctl(fd, PTP_SYS_OFFSET_EXTENDED, &psoe);
    uint64_t e = fenced_rdtscp();
    if (r) {
        perror("PTP_SYS_OFFSET_EXTENDED");
        return 1;
    }
    uint64_t sc_delay = tsc2ns(e - b);
    for (unsigned i = 0; i < psoe.n_samples; ++i) {
        int64_t delay = pct2ns_tai(&psoe.ts[i][2]) - pct2ns_tai(&psoe.ts[i][0]);
        int64_t off  = (pct2ns_tai(&psoe.ts[i][0]) + pct2ns_tai(&psoe.ts[i][2])) / 2
                           - pct2ns(&psoe.ts[i][1]);
        printf("PTP_SYS_OFFSET_EXTENDED no %u: %" PRId64 " ns, delay: %" PRId64 " ns, sycall: %" PRIu64 " ns\n",
                i+1, off, delay, sc_delay);
    }
    return 0;
}

static int read_ptp_offset_precise(int fd)
{
    struct ptp_sys_offset_precise psop = { 0 };
    uint64_t b = fenced_rdtsc();
    int r = ioctl(fd, PTP_SYS_OFFSET_PRECISE, &psop);
    uint64_t e = fenced_rdtscp();
    if (r) {
        perror("PTP_SYS_OFFSET_PRECISE");
        return 1;
    }
    uint64_t sc_delay = tsc2ns(e - b);
    int64_t off  = pct2ns_tai(&psop.sys_realtime) - pct2ns(&psop.device);
    printf("PTP_SYS_OFFSET_PRECISE: %" PRId64 " ns, delay: 0 ns, syscall: %" PRIu64 " ns\n",
                off, sc_delay);
    return 0;
}


static int mk_if_fd()
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1)
        perror("creating if fd");
    return fd;
}


static int get_ptp_dev(int fd, const char *if_name, const char **dev, bool *is_sfc)
{
    struct ethtool_ts_info tsi = {
        .cmd = ETHTOOL_GET_TS_INFO,
        .phc_index = 23
    };

    struct ifreq ifr = {
        .ifr_data = (void*) &tsi
    };
    strcpy(ifr.ifr_name, if_name);

    int r = ioctl(fd, SIOCETHTOOL, &ifr);
    if (r == -1) {
        perror("ioctl SIOCETHTOOL ETHTOOL_GET_TS_INFO");
        return -1;
    }

    if (tsi.phc_index == -1) {
        fprintf(stderr, "%s has no PTP hardware clock device\n", if_name);
        return -1;
    }
    char *s = 0;
    r = asprintf(&s, "/dev/ptp%d", tsi.phc_index);
    if (r == -1) {
        perror("asprintf");
        return -1;
    }
    *dev = s;

    struct ethtool_drvinfo di = {
        .cmd = ETHTOOL_GDRVINFO
    };
    ifr.ifr_data = (void*) &di;

    r = ioctl(fd, SIOCETHTOOL, &ifr);
    if (r == -1) {
        perror("ioctl SIOCETHTOOL ETHTOOL_GDRVINFO");
        return 1;
    }

    if (!strcmp(di.driver, "sfc"))
        *is_sfc = true;

    return 0;
}


struct sfc_ts {
    int64_t sec;
    int32_t nsec;
};

static int64_t sfcts2ns(const struct sfc_ts *ts)
{
    return (int64_t)(ts->sec * 1000000000lu) + (int64_t)ts->nsec;
}

const unsigned long SIOCEFX = SIOCDEVPRIVATE + 3;
const uint16_t EFX_TS_SYNC = 0xef16;

static int read_sfc_offset(int fd, const char *name)
{
    struct ts_req {
        uint16_t command;
        uint16_t pad;
        struct sfc_ts ts;
    } __attribute__ ((packed));
    struct ts_req d = {
        .command = EFX_TS_SYNC
    };
    struct ifreq ifr = {
        .ifr_data = (void*) &d
    };
    strcpy(ifr.ifr_name, name);



    uint64_t b = fenced_rdtsc();
    int r = ioctl(fd, SIOCEFX, &ifr);
    uint64_t e = fenced_rdtscp();
    if (r) {
        perror("SFC SIOCEFX");
        return 1;
    }
    uint64_t sc_delay = tsc2ns(e - b);
    struct sfc_ts t = d.ts;
    int64_t off = sfcts2ns(&t);

    printf("SFC_OFFSET: %" PRId64 " ns, delay: ? ns, syscall: %" PRIu64 " ns\n",
            off, sc_delay);


    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "call: %s /dev/ptpX|ifname\n", argv[0]);
        return 1;
    }

#ifndef PCO_READ_PERF
    int r = get_tsc_khz(&tsc_khz);
    if (r) {
        return 1;
    }
    clocks_calc_mult_shift(&tsc_mult, &tsc_shift,
            tsc_khz, 1000000l, 0);
#else
    int r = get_tsc_perf(&tsc_mult, &tsc_shift);
    if (r == -1)
        return 1;
#endif


    bool is_sfc = false;
    const char *if_name = 0;
    int if_fd = -1;
    const char *dev = argv[1];


    if (*dev != '/') {
        if_name = dev;
        if_fd = mk_if_fd();
        if (if_fd == -1)
            return 1;
        int r = get_ptp_dev(if_fd, if_name, &dev, &is_sfc);
        if (r == -1)
            return 1;
    }

    int fd = open(dev, O_RDWR);
    if (fd == -1) {
        perror("open PTP device");
        return 1;
    }

    printf("## Testing clock_gettime\n");
    read_clock_offset(fd);

    printf("## Testing PTP_SYS_OFFSET ioctl (%#lx)\n", PTP_SYS_OFFSET);
    read_ptp_offset(fd);
    printf("## Testing PTP_SYS_OFFSET_EXTENDED ioctl (%#lx)\n", PTP_SYS_OFFSET_EXTENDED);
    read_ptp_offset_extended(fd);
    printf("## Testing PTP_SYS_OFFSET_PRECISE ioctl (%#lx)\n", PTP_SYS_OFFSET_PRECISE);
    read_ptp_offset_precise(fd);

    if (is_sfc) {
        printf("## Testing Solarflare SIOCEFX / EFX_TS_SYNC ioctl (%#lx / %#" PRIx16 ")\n", SIOCEFX, EFX_TS_SYNC);
        read_sfc_offset(if_fd, if_name);
    }

    if (if_fd != -1)
        close(if_fd);
    close(fd);

    return 0;
}
