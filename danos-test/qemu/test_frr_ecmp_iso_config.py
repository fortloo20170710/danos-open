#!/usr/bin/env python3
"""Guard slow-but-real FRR boot readiness without weakening packet gates."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
builder = (root / "danos-test/qemu/build_frr_ecmp_iso.sh").read_text()
assert ': "${VPP_TRAFFIC_READY_TIMEOUT:=600}"' in builder
assert 'VPP_TRAFFIC_READY_TIMEOUT' in builder
assert ': "${VPP_TRAFFIC_READY_ENDPOINT:=tcp://10.0.3.2:2603}"' in builder
assert ': "${FRR_SHARED_PEER_L2:=0}"' in builder
assert 'if test "$FRR_SHARED_PEER_L2" = 1; then' in builder
assert ': "${VPP_IF2_EXTRA_ADDRS:=172.31.0.1/24}"' in builder
assert 'export VPP_IF2_EXTRA_ADDRS' in builder
print("FRR/VPP ISO readiness timeout configuration PASS")
