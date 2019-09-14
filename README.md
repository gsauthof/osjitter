This repository contains OSjitter - a tool for measuring how much
the operating system interrupts programs. Such interruptions
increase the latency of a program while the variation in latency
is called jitter.

This tool can be used to quickly measure a lower bound for the
latency of a given system configuration. Note that the OS jitter
depends on the kind of load a real-time program is applying to a
system. Thus, one still needs to execute a domain specific
test-suite to the real-time program of interest after a tool like
OSjitter shows good results.


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
       0  2200000  60196   60196        0       6416    275543362   0.005    60       22       3200    3048    4178    6020    7140    459807  1474803     174
       1  2200000  60266   60266        0       4818    206335029   0.003    60       22       2459    2284    2969    3730    6160     38577  4149922     160
       2  2200000  60202   60202        0       8229    192165331   0.003    60       22       2520    2290    3653    5223    6523     11995  2550735     332
       3  2200000  60232   60232        0       6581    201093244   0.003    60       22       2418    2190    3490    4922    6019     14058  4513274     266
       4  2200000  60203   60203        0       9261    204795305   0.003    60       22       2585    2322    3702    5260    9245     15116  3322313     381
       5  2200000    158     158        0          1       575715   0.000    60       22       3689    2695    4030    4280   10230     12374    13912     481
       6  2200000    160     160        0          1       619641   0.000    60       22       3597    2886    4033    4367   17424     19032    19154     446
       7  2200000    125     125        0          1       439794   0.000    60       22       3585    2656    3946    4120   12048     12378    12378     412

=> The threads on the isolated CPUs are much less interrupted the
other ones.

Move all interrupts away from the isolated CPUs:

    # tuna -q '*' -c 0-4 -m -x

OSjitter:

    $ ./osjitter  -t 60
     CPU  TSC_khz  #intr  #delta  ovfl_ns  invol_ctx  sum_intr_ns  iratio  rt_s  loop_ns  median_ns  p20_ns  p80_ns  p90_ns  p99_ns  p99.9_ns   max_ns  mad_ns
       0  2200000  60205   60205        0       7427    269091478   0.004    60       22       3210    3035    4301    6113    6639    442755  2093361     212
       1  2200000  60231   60231        0       8693    192603708   0.003    60       22       2537    2296    3645    5186    6223      8956  3876124     364
       2  2200000  60256   60256        0       7000    180918164   0.003    60       22       2482    2285    3505    5040    6627     11577  1297573     250
       3  2200000  60309   60309        0       5488    234876480   0.004    60       22       2403    2176    3080    4364    6246    209336  4732562     220
       4  2200000  60208   60208        0       6161    188113661   0.003    60       22       2474    2276    3514    4936    8337     15319   503289     222
       5  2200000      8       8        0          1        43580   0.000    60       22       3779    1715    8810   12151   12151     12151    12853    2063
       6  2200000      8       8        0          1        43780   0.000    60       22       3342    1713    8967   12913   12913     12913    13655    1629
       7  2200000      8       8        0          1        42980   0.000    60       22       3600    1610    8834   12274   12274     12274    12879    1990


=> Even less interruptions on the isolated CPU's

Move all moveable kernel threads away from the isolated CPUs:

    # tuna -U -t '*' -c 0-4 -m

OSjitter:

    $ ./osjitter  -t 60
     CPU  TSC_khz  #intr  #delta  ovfl_ns  invol_ctx  sum_intr_ns  iratio  rt_s  loop_ns  median_ns  p20_ns  p80_ns  p90_ns  p99_ns  p99.9_ns   max_ns  mad_ns
       0  2200000  60214   60214        0       5572    264737758   0.004    60       22       3190    3044    3788    4820    6924    456489  1923610     156
       1  2200000  60269   60269        0       9959    203372392   0.003    60       22       2756    2322    3779    5261    6292     10767  3557162     485
       2  2200000  60252   60252        0       6423    186974446   0.003    60       22       2453    2248    3483    4911    6309     12167  2523608     234
       3  2200000  60259   60259        0       5960    200100643   0.003    60       22       2401    2160    3386    4820    6088     24665  4725210     252
       4  2200000  60225   60225        0       6667    220479831   0.004    60       22       2501    2291    3531    4988    9270     38184  4778341     252
       5  2200000      8       8        0          1        43055   0.000    60       22       3486    1967    8683   11649   11649     11649    12141    1519
       6  2200000      8       8        0          1        43463   0.000    60       22       3369    1748    9275   12473   12473     12473    12675    1620
       7  2200000      8       8        0          1        43149   0.000    60       22       3181    1703    9305   12520   12520     12520    12824    1477

=> Isolated CPUs: Improvements in median, max and MAD.

Switch from througput-performance based tuned profile to a latency-performance
based one (i.e. disable CPU frequency scaling, longer stat interval, writeback
cpubask etc.):

    # tuned-adm profile gs-latency

OSjitter:

    $ ./osjitter  -t 60
     CPU  TSC_khz  #intr  #delta  ovfl_ns  invol_ctx  sum_intr_ns  iratio  rt_s  loop_ns  median_ns  p20_ns  p80_ns  p90_ns  p99_ns  p99.9_ns   max_ns  mad_ns
       0  2200000  60210   60210        0      30056    454539363   0.008    60       22       6040    3810    8597    8674    9582    464898  8367503    2311
       1  2200000  60393   60393        0        302    221610551   0.004    60       22       2373    2275    2528    2597    3782    265434  9155245      95
       2  2200000  60427   60427        0        549    229374123   0.004    60       22       2374    2272    2517    2600    6093    282190  8115911      90
       3  2200000  60319   60319        0        337    252409914   0.004    60       22       2320    2144    2481    2568    4604    555541  5052078     140
       4  2200000  60457   60457        0        811    235356238   0.004    60       22       2376    2200    2547    2616   13364    296060 20524508     155
       5  2200000      5       5        0          0        15541   0.000    60       22       2182    1904    6778    6778    6778      6778     6778     356
       6  2200000      5       5        0          0        13930   0.000    60       22       1911    1690    6300    6300    6300      6300     6300     400
       7  2200000      5       5        0          0        14120   0.000    60       22       1946    1852    5869    5869    5869      5869     5869     369

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

[Cyclictest][cyctest] measures OS latency by [setting
timers][cyc2] and comparing the actual sleep time with the
configured one.


[sj]: https://www.openonload.org/download.html
[hwl]: https://www.kernel.org/doc/html/latest/trace/hwlat_detector.html
[smm]: https://en.wikipedia.org/wiki/System_Management_Mode
[1]: https://stackoverflow.com/a/57961772/427158
[tsc]: https://en.wikipedia.org/wiki/Time_Stamp_Counter
[cyc]: https://git.kernel.org/pub/scm/linux/kernel/git/clrkwllms/rt-tests.git
[cyc2]: http://people.redhat.com/williams/latency-howto/rt-latency-howto.txt
[2]: https://stackoverflow.com/a/57835630/427158
