#!/usr/bin/env python3
"""Speed benchmark: the CPU time of a fixed set of scripted tests.

Deterministic runs always do the same work, so their CPU time measures the
port's speed (interpreter and Toolbox); the best of N rounds is reported.
Compare before and after a change that could affect speed.

Usage: tools/bench.py [-n ROUNDS] [SCRIPT...]
       (default: 3 rounds of fixture + tutorial, travel, combat, win)
"""
import os, resource, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = ['tests/scripts/tutorial.txt', 'tests/scripts/travel.txt',
           'tests/scripts/combat.txt', 'tests/scripts/win.txt']

def main():
    args = sys.argv[1:]
    rounds = 3
    if args[:1] == ['-n']:
        rounds = int(args[1]); args = args[2:]
    scripts = args or DEFAULT
    env = dict(os.environ, GOLDEN='off')
    times = []
    for r in range(rounds):
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        p = subprocess.run(['sh', 'tests/run.sh'] + scripts, cwd=ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        if p.returncode:
            sys.exit('tests failed:\n' + p.stdout)
        cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
        times.append(cpu)
        print(f'round {r + 1}: {cpu:.2f} s CPU', flush=True)
    print(f'best: {min(times):.2f} s CPU ({len(scripts)} scripts + fixture)')

if __name__ == '__main__':
    main()
