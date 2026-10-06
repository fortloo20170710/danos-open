#!/usr/bin/env python3
"""Bind topology acceptance to one clean boot and the actual ISO digest."""
import hashlib
import re
import shlex
import sys
from pathlib import Path


def validate(topology: Path) -> None:
    values = {}
    for line in (topology / "run-manifest.env").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, raw = line.partition("=")
        tokens = shlex.split(raw)
        if not separator or key in values or len(tokens) != 1:
            raise ValueError("malformed or duplicate manifest entry")
        values[key] = tokens[0]
    commit = values.get("git_commit", "")
    digest = values.get("danos_iso_sha256", "")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("invalid manifest commit")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("invalid manifest ISO digest")
    if 'runner_commit' in values:
        if not re.fullmatch(r'[0-9a-f]{40}', values['runner_commit']):
            raise ValueError('invalid runner commit')
        source_topology = Path(values.get('topology_dir', ''))
        if not source_topology.is_absolute():
            raise ValueError('seed topology path must be absolute')
        for node in (1, 2):
            expected = values.get(f'frr_seed{node}_sha256', '')
            seed = source_topology / f'r{node}-seed.iso'
            if not re.fullmatch(r'[0-9a-f]{64}', expected) or not seed.is_file():
                raise ValueError('FRR seed identity is missing')
            if hashlib.sha256(seed.read_bytes()).hexdigest() != expected:
                raise ValueError('FRR seed content differs from manifest')
    iso = Path(values.get("danos_iso", ""))
    if not iso.is_absolute() or not iso.is_file():
        raise ValueError("manifest ISO must be an existing absolute path")
    with iso.open("rb") as stream:
        actual = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual != digest:
        raise ValueError("ISO content does not match manifest digest")
    serial = (topology / "danos.serial.log").read_text(errors="replace")
    boots = re.findall(
        r"DANOS-BUILD commit=(\S+) source_dirty=([01]) iso=(\S+)", serial
    )
    if len(boots) != 1:
        raise ValueError("acceptance requires exactly one identifiable DANOS boot")
    boot_commit, dirty, name = boots[0]
    if dirty != "0" or boot_commit != commit or name != iso.name:
        raise ValueError("boot identity is dirty or differs from manifest")


if __name__ == "__main__":
    try:
        validate(Path(sys.argv[1]))
    except (ValueError, OSError, IndexError) as error:
        print(f"[FAIL] topology identity: {error}", file=sys.stderr)
        sys.exit(1)
    print("[PASS] topology ISO content and clean single-boot identity")
