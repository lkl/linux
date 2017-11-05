#!/usr/bin/env python
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License
#
# Author: Octavian Purdila <tavi@cs.pub.ro>
#

from __future__ import print_function

import argparse
import os
import subprocess
import sys
import tap13

from junit_xml import TestSuite, TestCase


class Reporter(tap13.Reporter):
    def start(self, obj):
        if type(obj) is tap13.Test:
            if obj.result == "*":
                end='\r'
            else:
                end='\n'
            print("  TEST       %-8s %.50s" %
                  (obj.result, obj.description + " " + obj.comment), end=end)

        elif type(obj) is tap13.Suite:
            if obj.tests_planned == 0:
                status = "skip"
            else:
                status = ""
            print("  SUITE      %-8s %s" % (status, obj.name))

    def end(self, obj):
        if type(obj) is tap13.Test:
            if obj.result != "ok":
                try:
                    print(obj.yaml["log"], end='')
                except:
                    None


mydir=os.path.dirname(os.path.realpath(__file__))

runs = [
    {'command': 'boot.sh' },
    {'command': 'disk.sh -t %s', 'set': ['ext4', 'btrfs', 'vfat', 'xfs']},
    {'command': 'net.sh -b %s', 'set': ['loopback', 'tap', 'pipe', 'raw', 'macvtap']},
    {'command': 'lklfuse.sh -t %s', 'set': ['ext4', 'btrfs', 'vfat', 'xfs']},
    {'command': 'hijack-test.sh'},
]

parser = argparse.ArgumentParser(description='LKL test runner')
parser.add_argument('--junit-dir',
                    help='directory where to store the juni suites')

args = parser.parse_args()

commands = []
for r in runs:
    if 'set' in r:
        for s in r['set']:
            commands.append(r['command'] % (s))
    else:
        commands.append(r['command'])

tap = tap13.Parser(Reporter())

os.environ['PATH'] += ":" + mydir

for c in commands:
    p = subprocess.Popen(c, shell=True, stdout=subprocess.PIPE)
    tap.parse(p.stdout)

suites_count = 0
tests_total = 0
tests_not_ok = 0
tests_ok = 0
tests_skip = 0

for s in tap.run.suites:

    junit_tests = []
    suites_count += 1

    for t in s.tests:
        try:
            secs = t.yaml["time_us"]/1000000.0
        except:
            secs = 0
        try:
            log = t.yaml['log']
        except:
            log = ""

        jt = TestCase(t.description, elapsed_sec=secs, stdout=log)
        if t.result == 'skip':
            jt.add_skipped_info(output=log)
        elif t.result == 'not ok':
            jt.add_error_info(output=log)

        junit_tests.append(jt)

        tests_total += 1
        if t.result == "ok":
            tests_ok += 1
        elif t.result == "not ok":
            tests_not_ok += 1
        elif t.result == "skip":
            tests_skip += 1

    if args.junit_dir:
        js = TestSuite(s.name, junit_tests)
        with open(os.path.join(args.junit_dir, os.path.basename(s.name) + '.xml'), 'w') as f:
            js.to_file(f, [js])

print("Summary: %d suites run, %d tests, %d ok, %d not ok, %d skipped" %
      (suites_count, tests_total, tests_ok, tests_not_ok, tests_skip))

if tests_not_ok > 0:
    sys.exit(1)
