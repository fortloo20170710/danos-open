#!/usr/bin/env bash
# Shared by local packaging and CI; never use the ignored build/ as context.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
CONTEXT="${1:?usage: stage_mgrd_oci.sh NEW_CONTEXT_DIRECTORY}"
# Require a fresh context so stale files cannot enter a release image.
mkdir "$CONTEXT"
mkdir -p "$CONTEXT/build/danos-mgrd" "$CONTEXT/danos-mgrd"
install -m 755 "$BUILD_DIR/danos-mgrd/danos-mgrd" "$CONTEXT/build/danos-mgrd/"
install -m 644 "$ROOT/danos-mgrd/danos-mgrd.service" "$CONTEXT/danos-mgrd/"
