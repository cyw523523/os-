#!/usr/bin/env python3
"""Build first, then run Lab5 and old regressions with strict log checks."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cpus', type=int, choices=(1, 2), default=1)
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--log-dir', default='support/lab5-logs')
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    logs = Path(args.log_dir).resolve()
    logs.mkdir(parents=True, exist_ok=True)
    subprocess.run(['make', '-s', '-j4'], cwd=root, check=True)
    subprocess.run([sys.executable, 'check_expect.py', '2024302111375'],
                   cwd=root, check=True)
    scripts = ['lab5-uart.script', 'lab3-uart.script',
               'lab2-uart-regression.script',
               'lab4-smp-uart.script' if args.cpus == 2 else 'lab4-uart.script']
    for iteration in range(args.repeat):
        for script in scripts:
            log = logs / f'cpu{args.cpus}-run{iteration + 1}-{script}.log'
            subprocess.run([sys.executable, 'support/inject_uart.py', '--tree', '.',
                            '--cpus', str(args.cpus), '--script', f'support/tests/{script}',
                            '--log', str(log)], cwd=root, check=True)
            data = log.read_text(encoding='utf-8')
            if re.search(r'FAIL|WARN|panic|kernel trap', data):
                raise RuntimeError(f'Unexpected failure/diagnostic in {log}')
            print(f'LOG CHECK PASS: {log}', flush=True)
    print(f'LAB5 VALIDATION PASS cpus={args.cpus} repeat={args.repeat}', flush=True)
    # Keep reviewable evidence on the Windows-mounted workspace, not transient /tmp.
    sections = ['Lab5 UART validation evidence\n'
                'Each section is a complete QEMU UART log from a successful check.\n']
    for log in sorted(logs.glob('cpu*-run*-*.log')):
        raw = log.read_bytes()
        sections.append(f'\n===== {log.name} sha256={hashlib.sha256(raw).hexdigest()} =====\n')
        sections.append(raw.decode('utf-8'))
    (root / 'support/lab5-validation.txt').write_text(''.join(sections), encoding='utf-8')


if __name__ == '__main__':
    main()
