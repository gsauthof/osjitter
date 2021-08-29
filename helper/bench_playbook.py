#!/usr/bin/env python3

# Distribute and run a benchmark on a bunch of hosts.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: © 2021 Georg Sauthoff <mail@gms.tf>

import mitogen
import mitogen.select
import mitogen.utils

import argparse
import logging
import os
import platform
import subprocess
import tempfile


log = logging.getLogger(__name__)

def bench(exe, bcmd):
    with tempfile.TemporaryDirectory() as d:
        exe_path = f'{d}/bench'
        with open(exe_path, 'wb') as f:
            f.write(exe)
        os.chmod(exe_path, 0o755)
        core = min(int(os.cpu_count()/2*1.5), os.cpu_count()-1)
        ts = [ 'taskset', '-c', str(core) ]
        subprocess.check_output(ts + bcmd, cwd=d)
        hostname = platform.node().split('.', 1)[0]
        with open(f'{d}/out.csv') as f:
            csv = f.read()
        with open('/proc/cmdline') as f:
            cmdline = f.read().strip()
        try:
            tuned = subprocess.check_output(['tuned-adm', 'active'], universal_newliens=True)
            tuned = tuned.split()[-1]
        except:
            tuned = ''
        with open('/proc/cpuinfo') as f:
            cpuinfo = f.read().splitlines()
            cpuinfo = [ l.split(': ')[-1] for l in cpuinfo if l.startswith('model name') ][0]
        return hostname, cpuinfo, cmdline, csv


def main(router, hosts, exe_path, bcmd, out_dir):
    with open(exe_path, 'rb') as f:
        exe = f.read()

    cns = [ (router.ssh(hostname=h, python_path='/usr/bin/python3'), h) for h in hosts ]

    fs = []
    for c, host in cns:
        log.info(f'Starting bench on {host} ...')
        fs.append(c.call_async(bench, exe, bcmd))

    with open(f'{out_dir}/hosts.csv', 'w') as g:
        g.write('hostname,cpuinfo,cmdline\n')
        for i, res in enumerate(mitogen.select.Select(fs)):
            log.info(f'Receiving from {res.router._stream_by_id[res.src_id].conn.options.hostname} ...')
            r = res.unpickle()
            g.write(f'{r[0]},{r[1]},"{r[2]}"\n')
            with open(f'{out_dir}/bench-{r[0]}.csv', 'w') as f:
                f.write(r[3])

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument('hosts', metavar='HOST', nargs='+',
            help='hosts under test')
    p.add_argument('--out', '-o', default='out',
            help='local directory for storing collected benchmark results (default: %(default)s)')
    p.add_argument('--exe', '-e', default='bench_syscalls',
            help='executable to transfer and execute remotely (default: %(default)s)')
    p.add_argument('-n', type=int, default=3,
            help='benchmark repetitions (default: %(default)s)')
    p.add_argument('--log', default='pb.log',
            help='logfile (is more verbose than the console log) (default: %(default)s)')
    args = p.parse_args()
    return args

if __name__ == '__main__':
    args = parse_args()
    bcmd = [ './bench', '--benchmark_out_format=csv', '--benchmark_out=out.csv',
             f'--benchmark_repetitions={args.n}' ]
    os.makedirs(args.out, exist_ok=True)
    mitogen.utils.log_to_file(args.log)
    h = logging.StreamHandler()
    h.setFormatter(logging.Formatter(
        '%(asctime)s - %(levelname)-8s - %(message)s [%(name)s]',
        '%Y-%m-%d %H:%M:%S'))
    log.addHandler(h)
    mitogen.utils.run_with_router(main, args.hosts, args.exe, bcmd, args.out)

