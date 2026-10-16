#!/usr/bin/env python3
"""Summarize archived functional measurements without promoting them to PCI acceptance."""
import argparse
import hashlib
import json
import math
import shlex
from pathlib import Path


def baseline(path):
    raw = path.read_bytes()
    values = {}
    for line in raw.decode().splitlines():
        if not line or line.startswith('#'):
            continue
        key, sep, value = line.partition('=')
        if not sep or key in values:
            raise ValueError('malformed or duplicate field')
        parsed = shlex.split(value)
        if len(parsed) > 1:
            raise ValueError('multiple field values')
        values[key] = parsed[0] if parsed else ''
    if values.get('status') != 'PASS':
        raise ValueError('functional lane must have a PASS result')
    if values.get('lane') not in ('qemu-e1000', 'vmware-vmxnet3-polling'):
        raise ValueError('only archived virtual functional lanes are supported')

    def number(key, optional=False):
        if not values.get(key) and optional:
            return None
        n = float(values[key])
        if not math.isfinite(n) or n < 0:
            raise ValueError('invalid metric: ' + key)
        return n

    tx, rx, duration = (number(k) for k in ('packets_tx', 'packets_rx', 'duration_ms'))
    if tx <= 0 or duration <= 0 or rx > tx:
        raise ValueError('invalid packet window')
    pps = tx * 1000 / duration
    if abs(number('pps') - pps) > max(0.02, pps * 0.01):
        raise ValueError('PPS disagrees with counters/window')
    buckets = [number('ecmp_bucket_0'), number('ecmp_bucket_1')]
    if any(n != int(n) for n in [tx, rx, *buckets]) or sum(buckets) > tx or sum(buckets) <= 0:
        raise ValueError('invalid bucket counters')
    p50, p99 = number('rtt_p50_us'), number('rtt_p99_us')
    if p99 < p50:
        raise ValueError('invalid RTT percentiles')
    size = number('packet_size_bytes', True)
    if size is not None and (size < 64 or size != int(size)):
        raise ValueError('invalid MAC frame size')
    mbps = pps * size * 8 / 1e6 if size else None
    recorded_mbps = number('mbps', True)
    if recorded_mbps is not None and (mbps is None or abs(recorded_mbps - mbps) > max(0.001, mbps * 0.02)):
        raise ValueError('throughput disagrees with frame size/window')
    cpu = number('cpu_pct', True)
    missing = []
    if mbps is None:
        missing.append('throughput: MAC frame size not recorded')
    # Legacy records have no clock/process provenance; even a supplied number
    # cannot establish a phase-aligned CPU baseline.
    missing.append('CPU: phase-aligned process identity and tick snapshots required')
    return dict(lane=values['lane'], status='PARTIAL-BASELINE', physical_performance_accepted=False,
                source=str(path), source_sha256=hashlib.sha256(raw).hexdigest(),
                source_commit=values.get('commit'), iso_sha256=values.get('iso_sha256'),
                recorded_utc=values.get('recorded_utc'), scope=values.get('performance_scope'),
                packets_tx=int(tx), packets_rx=int(rx), duration_ms=duration,
                tx_pps=pps, rx_pps=rx * 1000 / duration, mac_frame_mbps=mbps,
                packet_size_bytes=size, rtt_p50_us=p50, rtt_p99_us=p99,
                cpu_pct=None, legacy_unqualified_cpu_pct=cpu,
                ecmp_bucket_packets=buckets,
                ecmp_bucket_share_pct=[n * 100 / sum(buckets) for n in buckets],
                ecmp_counter_coverage_pct=sum(buckets) * 100 / tx,
                missing=missing)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--result', type=Path, action='append', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        lanes = [baseline(path) for path in args.result]
        if len({lane['lane'] for lane in lanes}) != len(lanes):
            raise ValueError('duplicate lane')
    except (ValueError, KeyError, OSError) as exc:
        parser.error(str(exc))
    result = dict(schema_version=1, status='PARTIAL-BASELINE', lanes=lanes,
                  pci_performance_status='ENVIRONMENT-OPEN')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
