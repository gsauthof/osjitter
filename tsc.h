// 2019, Georg Sauthoff <mail@gms.tf>
//
// SPDX-License-Identifier: GPL-3.0-or-later


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
