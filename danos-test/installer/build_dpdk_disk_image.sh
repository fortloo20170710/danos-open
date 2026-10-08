#!/usr/bin/env bash
# Generic DPDK-capable installed disk image builder entry point.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
exec bash "$ROOT/danos-test/installer/build_i211_disk_image.sh" "$@"
