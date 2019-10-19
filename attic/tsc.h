
#include <x86intrin.h> // __rdtsc(), _mm_lfence(), ...


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

