#ifndef PTP_CLOCK_FUTURE_H
#define PTP_CLOCK_FUTURE_H

#include <linux/ptp_clock.h>


// Note that PTP_SYS_OFFSET_EXTENDED is missing on some RHEL 7 versions although
// PTP_SYS_OFFSET_PRECISE is even available.


// imported from https://sourceforge.net/p/linuxptp/code/ci/61c6a708980217119e829e4b41ea2504e673e4fb/
#ifndef PTP_SYS_OFFSET_EXTENDED

#define PTP_SYS_OFFSET_EXTENDED \
	_IOWR(PTP_CLK_MAGIC, 9, struct ptp_sys_offset_extended)

struct ptp_sys_offset_extended {
	unsigned int n_samples; /* Desired number of measurements. */
	unsigned int rsv[3];    /* Reserved for future use. */
	/*
	 * Array of [system, phc, system] time stamps. The kernel will provide
	 * 3*n_samples time stamps.
	 */
	struct ptp_clock_time ts[PTP_MAX_SAMPLES][3];
};

#endif /* PTP_SYS_OFFSET_EXTENDED */



#endif
