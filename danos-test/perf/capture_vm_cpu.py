#!/usr/bin/env python3
"""Bracket a local traffic command with QEMU/VMware process CPU snapshots."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time
from collect_vpp_cpu import parse_stat, cpu_percent


def check_executable(executable, lane):
    name = executable.name
    if (lane == 'qemu-e1000' and not name.startswith('qemu-system-')) or (
            lane == 'vmware-vmxnet3' and name != 'vmware-vmx'):
        raise ValueError('PID executable does not match selected VM lane')


def run_command(command, timeout, log):
    # Kill the command group on timeout, including traffic subprocesses.
    with subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                          start_new_session=True) as child:
        try:
            return child.wait(timeout=timeout)
        except BaseException:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--lane', choices=['qemu-e1000', 'vmware-vmxnet3'], required=True)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if args.pid <= 0 or not 0 < args.timeout <= 3600 or not command:
        parser.error('positive PID, timeout in (0,3600], and traffic command required')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    log_path = args.out.with_suffix('.command.log')
    result = dict(schema_version=1, status='FAIL', scope='host-vm-command-window',
                  lane=args.lane, pid=args.pid, cpu_basis='one-logical-cpu',
                  performance_status='ENVIRONMENT-OPEN', command=command,
                  command_log=str(log_path))
    try:
        proc = Path('/proc') / str(args.pid)
        executable = (proc / 'exe').resolve(strict=True)
        check_executable(executable, args.lane)
        first = parse_stat((proc / 'stat').read_text())
        hz = os.sysconf('SC_CLK_TCK')
        before = time.monotonic()
        with log_path.open('wb') as log:
            code = run_command(command, args.timeout, log)
        after = time.monotonic()
        last = parse_stat((proc / 'stat').read_text())
        if (proc / 'exe').resolve(strict=True) != executable:
            raise ValueError('VM executable changed during capture')
        result.update(status='PASS' if code == 0 else 'FAIL', command_exit_code=code,
                      executable=str(executable), process_start_ticks=first[1],
                      ticks_start=first[2], ticks_end=last[2], clock_ticks_per_second=hz,
                      monotonic_start=before, monotonic_end=after,
                      duration_ms=(after-before)*1000,
                      cpu_pct=cpu_percent(first, last, after-before, hz))
    except (OSError, ValueError, IndexError, subprocess.SubprocessError) as exc:
        result['failure_reason'] = str(exc)
    args.out.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
    print(json.dumps(result, allow_nan=False))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
