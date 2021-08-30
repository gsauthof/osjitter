#!/usr/bin/env python3

# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: © 2021 Georg Sauthoff <mail@gms.tf>

import argparse

def is_not_a_point(line):
    for k in ('_mean"', '_median"', '_stddev"'):
        if k in line:
            return True
    return False

def dump_csv(filename, host, o):
    with open(filename) as f:
        state = 0
        for line in f:
            if state == 0:
                if line.startswith('name,iterations,real_time,cpu_time,time_unit'):
                    state = 1
            elif state == 1:
                if is_not_a_point(line):
                    continue
                i = line.rindex(',ns,')
                o.write(f'{host},{line[:i]}\n')


def main(filenames, ofilename):
    with open(ofilename, 'w') as f:
        f.write('host,name,iterations,real_ns,cpu_ns\n')
        for fn in filenames:
            host = fn[fn.rindex('-')+1:-4]
            dump_csv(fn, host, f)

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument('filenames', metavar='CSV_FILENAME', nargs='+',
            help='hosts under test')
    p.add_argument('--out', '-o', default='all.csv',
            help='resulting CSV filename (default: %(default)s)')
    return p.parse_args()

if __name__ == '__main__':
    args = parse_args()
    main(args.filenames, args.out)

