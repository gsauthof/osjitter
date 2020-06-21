This repository contains OSjitter, Pingpong and ptp-clock-offset.

OSjitter is a tool for measuring how much
the operating system interrupts programs. Such interruptions
increase the latency of a program while the variation in latency
is called jitter.

This tool can be used to quickly measure a lower bound for the
latency of a given system configuration. Note that the OS jitter
depends on the kind of load a real-time program is applying to a
system. Thus, one still needs to execute a domain specific
test-suite to the real-time program of interest after a tool like
OSjitter shows good results.

The Pingong utility measures the overhead of several thread
notification mechanisms such as spinning on a atomic variable
(with/without pauses), POSIX condition variables, semaphores,
pipes and raw Linux futexes.

The ptp-clock-offset utility is a small program for checking
the availability of different PTP offset ioctls and how they
perform. Rule of thumb: using any PTP offset ioctl is better than
having to use `clock_gettime()` and smaller delays are better.


2019, Georg Sauthoff <mail@gms.tf>, GPLv3+

## Example Session

Check out the help:

    $ ./osjitter -h

Isolating the last 3 cores on a 8 core system:

    $ cat /proc/cmdline
    [..] isolcpus=5-7 nohz=on nohz_full=5-7 rcu_nocbs=5-7 rcu_nocb_poll \
    nowatchdog mce=ignore_ce acpi_irq_nobalance pcie_aspm=off tsc=reliable

This system is a Supermicro one (running Fedora 29) with an Atom CPU:

    $ cat /proc/cpuinfo | grep model' name' | head -n 1
    model name	: Intel(R) Atom(TM) CPU C3758 @ 2.20GHz

First OSjitter run:

    $ ./osjitter  -t 60
     CPU  TSC_khz  #intr  #delta  ovfl_ns  invol_ctx  sum_intr_ns  iratio  rt_s  loop_ns  median_ns  p20_ns  p80_ns  p90_ns  p99_ns  p99.9_ns   max_ns  mad_ns
       0  2200000  60240   60240        0       8065    283228653   0.005    60       22       3151    2989    4354    6047    7218    443376  9380037     220
       1  2200000  60192   60192        0       9809    216975033   0.004    60       22       2710    2339    3740    5314    6322     11774  4614206     432
       2  2200000  60199   60199        0       5942    180783353   0.003    60       22       2424    2219    3399    4847    7888     14611  1465586     223
       3  2200000  60193   60193        0       5465    171929486   0.003    60       22       2426    2236    3087    4246    6388     11487   592769     187
       4  2200000  60320   60320        0       6173    212338516   0.004    60       22       2548    2358    3468    5005    6280     40044  2262400     211
       5  2200000    156     156        0          1       576392   0.000    60       22       3681    2801    4044    4388   11667     12138    12286     428
       6  2200000    156     156        0          1       581260   0.000    60       22       3565    2788    3964    4270   12278     20279    28125     451
       7  2200000    126     126        0          1       450470   0.000    60       22       3703    2467    4003    4205    9163     11859    12198     352

=> The threads on the isolated CPUs are much less interrupted the
other ones.

Move all interrupts away from the isolated CPUs:

    # tuna -q '*' -c 0-4 -m -x

OSjitter:

    $ ./osjitter  -t 60
     CPU  TSC_khz  #intr  #delta  ovfl_ns  invol_ctx  sum_intr_ns  iratio  rt_s  loop_ns  median_ns  p20_ns  p80_ns  p90_ns  p99_ns  p99.9_ns   max_ns  mad_ns
       0  2200000  60342   60342        0       6207    272600031   0.005    60       22       3105    2980    4141    5898    7205    442155  4772690     144
       1  2200000  60334   60334        0       6483    196708372   0.003    60       22       2488    2293    3530    5014    6335     13491  4684815     236
       2  2200000  60330   60330        0       8479    211832782   0.004    60       22       2528    2296    3651    5269    9299     15708  5513140     347
       3  2200000  60256   60256        0       7973    237326578   0.004    60       22       2477    2261    3617    5155    7186     39479  5602172     325
       4  2200000  60280   60280        0       5149    197355746   0.003    60       22       2532    2345    3020    4026    6309     16298  2630389     175
       5  2200000      8       8        0          1        41371   0.000    60       22       3340    1869    8570   11288   11288     11288    11616    1470
       6  2200000      8       8        0          1        41025   0.000    60       22       3291    1706    8616   11429   11429     11429    11609    1585
       7  2200000     10      10        0          1        46852   0.000    60       22       2886    1927    8794   11968   11968     11968    12126     959

=> Even less interruptions on the isolated CPU's

Move all moveable kernel threads away from the isolated CPUs:

    # tuna -U -t '*' -c 0-4 -m

OSjitter:

    $ ./osjitter  -t 60
     CPU  TSC_khz  #intr  #delta  ovfl_ns  invol_ctx  sum_intr_ns  iratio  rt_s  loop_ns  median_ns  p20_ns  p80_ns  p90_ns  p99_ns  p99.9_ns   max_ns  mad_ns
       0  2200000  60246   60246        0       4333    231374600   0.004    60       22       3177    3040    3595    4465   10924     29714   469030     134
       1  2200000  60403   60403        0       5965    198823307   0.003    60       22       2490    2274    3425    4865    6387     16643  4743847     229
       2  2200000  60445   60445        0       5020    186508000   0.003    60       22       2402    2172    2959    3740    5762     12846  1716645     209
       3  2200000  60490   60490        0      10195    234402816   0.004    60       22       2825    2308    4398    5358    6915    112854  3997080     576
       4  2200000  60276   60276        0       7274    212001750   0.004    60       22       2531    2328    3668    5061    5747     13550  6431210     275
       5  2200000      8       8        0          1        34188   0.000    60       22       3197    1765    5095    8923    8923      8923    11685    1114
       6  2200000      8       8        0          1        39910   0.000    60       22       3218    1616    8130   11231   11231     11231    11793    1601
       7  2200000      5       5        0          0        16998   0.000    60       22       2091    2079    8506    8506    8506      8506     8506     574

=> Isolated CPUs: Improvements in interruptions, few improvements
in median, max and median absolute deviation (MAD).

Switch from throughput-performance based tuned profile to a latency-performance
based one (i.e. disable CPU frequency scaling, longer stat interval, writeback
cpubask etc.):

    # tuned-adm profile gs-latency

OSjitter:

    $ ./osjitter  -t 60
     CPU  TSC_khz  #intr  #delta  ovfl_ns  invol_ctx  sum_intr_ns  iratio  rt_s  loop_ns  median_ns  p20_ns  p80_ns  p90_ns  p99_ns  p99.9_ns   max_ns  mad_ns
       0  2200000  60250   60250        0        686    213519597   0.004    60       22       3125    3008    3250    3323   13616     37892  1871887      97
       1  2200000  60223   60223        0      26628    287996052   0.005    60       22       3118    2914    6182    6266    7117     17085  5240030     777
       2  2200000  60241   60241        0      26289    272751612   0.005    60       22       3079    2889    6183    6260    6480      9952  1231324     728
       3  2200000  60193   60193        0        167    163954807   0.003    60       22       2360    2123    2470    2526    3210     13830  8119388     124
       4  2200000  60223   60223        0        120    161220610   0.003    60       22       2427    2231    2514    2566    3060     13410  1885120      99
       5  2200000      5       5        0          1        14843   0.000    60       21       2255    1897    6112    6112    6112      6112     6112     402
       6  2200000      5       5        0          0        17074   0.000    60       22       2144    1852    8859    8859    8859      8859     8859     389
       7  2200000      5       5        0          0        16665   0.000    60       22       1922    1808    8630    8630    8630      8630     8630     234

=> Isolated CPUs: less interruptions, less total interruptions, improvements in median, max and MAD

## How it works

OSjitter creates a measurement thread for each selected CPU that
polls the CPU's [Time Stamp Counter (TSC)][tsc]. In each
iteration the previous counter value is subtracted from the
previous one and if that duration is above the threshold
(default: 100 ns) it's counted as an interruption.

Since the 1990ies, x86 CPUs feature a TSC, which can be read with
a special instruction from any user-space program. The TSC on
relatively modern CPUs is supposed to run constant and reliable,
i.e. even during CPU-frequency changes and power-saving state
changes. That means that the TSC frequency (although constant)
may be different from the base frequency of the CPU. Since the
TSC is integrated into the CPU, can be accessed like a register
(with low overhead) and has a high accuracy it's well suited for
measuring even short interruptions.

When a program is interrupted by the operating system the TSC
ticks continue and thus after the program execution continues
(otherwise transparently to the program) it can derive how long
it was interrupted by looking at the current TSC value.

The actual TSC frequency is required to convert TSC counts to
nanoseconds. OSjitter obtains the TSC frequency from the kernel,
i.e. from `/sys/devices/system/cpu/cpu0/tsc_freq_khz` (if
available) or it parses it from `journalctl --boot` ([relevant
stackoverflow answer][2]).

## How to build

    $ make

## Related Work

There is [sysjitter][sj] (1.4, GPLv3) which also reads the [TSC][tsc] in
a loop to detect external interruptions. Some differences are:

- Sysjitter calibrates the TSC frequency against `gettimeofday()`
  whereas OSJitter just obtains the Kernel's TSC frequency
  (the Kernel is in a better position to calibrate the TSC
  frequency and Linux contains a well-engineered calibration
  logic including possible refinements after the first
  calibration)
- Sysjitter just invokes the RDTSC instruction while OSjitter
  invokes RDTSC and RDTSCP in combination with fencing
  instructions
- OSjitter uses ISO C atomic operations while Sysjitter uses GCC
  atomic intrinsics
- In contrast to OSjitter, sysjitter doesn't allow to specify the
  scheduling class/priority of the measurement threads
- OSjitter's output includes a measure for dispersion (MAD)
- Besides TSC on x86, sysjitter also support reading a timestamp
  counter on POWER CPUs.

The Linux Kernel contains a [hardware latency detector][hwl] to
check for interruptions caused outside of the operating system
such as the [System Management Mode][smm] (SMM). It also queries
the TSC in a loop.

The SMM is triggered by System Management Interrupts (SMI)
which are transparent to the kernel and can only be detected
indirectly. An alternative to the TSC approach for detecting and
measuring SMIs is to query CPU counters the SMI changes
([relevant stackoverflow answer][1]).

[Cyclictest][cyc] measures OS latency by [setting
timers][cyc2] and comparing the actual sleep time with the
configured one.

## Pingpong Results

The doc directory contains some example Pingpong results for
different configurations.

The results for condition variable, semaphore and futex are quite
similar because, on Linux, condition variables and semaphores are
implemented in terms of futex.

Notifying via a traditional UNIX pipe is more expensive than
using a futex but it's the same order of magnitude.

Inserting a PAUSE instruction while spinning on an atomic
variable increases the median absolute deviation (MAD) just a
little bit, but yields similar median while reducing the number
of executed instructions.

As documented in the kernel documentation, comparing the results
with and without `full_hz=` show how this features increases
context-switch overhead and thus increases latency for the
syscall methods (e.g. by 0.6 us or so in the median, a few us in
the other percentiles and maximum). On the other hand, more
context-switch overhead isn't relevant for spinning on an atomic
variable, thus, `full_hz=` really pays off for this use-case
because the process is interrupted much less.


[sj]: https://www.openonload.org/download.html
[hwl]: https://www.kernel.org/doc/html/latest/trace/hwlat_detector.html
[smm]: https://en.wikipedia.org/wiki/System_Management_Mode
[1]: https://stackoverflow.com/a/57961772/427158
[tsc]: https://en.wikipedia.org/wiki/Time_Stamp_Counter
[cyc]: https://git.kernel.org/pub/scm/linux/kernel/git/clrkwllms/rt-tests.git
[cyc2]: http://people.redhat.com/williams/latency-howto/rt-latency-howto.txt
[2]: https://stackoverflow.com/a/57835630/427158
