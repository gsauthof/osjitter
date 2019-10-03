
// 2019, Georg Sauthoff <mail@gms.tf>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef OSJITTER_UTIL_H
#define OSJITTER_UTIL_H

#include <stdint.h>
#include <stddef.h>

static inline int cmp_u32(const void *a, const void *b)
{
    const uint32_t *x = a;
    const uint32_t *y = b;

    if (*x < *y)
        return -1;
    if (*x > *y)
        return 1;
    return 0;
}

// Linux Kernel has a function that is named the same
static inline uint64_t mul_u64_u32_shr(uint64_t cyc, uint32_t mult, uint32_t shift)
{
    __uint128_t x = cyc;
    x *= mult;
    x >>= shift;
    return x;
}

void perror_e(int r, const char *msg);

uint32_t percentile_u32(const uint32_t *x, size_t n, size_t a, size_t b);
uint32_t mad_u32(const uint32_t *x, uint32_t *y, size_t n);

int get_tsc_khz(uint32_t *tsc_khz);

void clocks_calc_mult_shift(
        uint32_t *mult, uint32_t *shift, uint32_t from, uint32_t to,
        uint32_t maxsec);

#endif
