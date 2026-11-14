#!/usr/bin/env bash
# Disposable dependency container, never host networking or a production VPP.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${VPP_TEST_IMAGE:?set VPP_TEST_IMAGE to a local VPP 26.10 dependency image with vppctl/ip/stdbuf}"
OUT="${READBACK_LIVE_OUT:-$(mktemp -d "$ROOT/build/backend-readback.XXXXXX")}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
NAME="danos-open-readback-$$"
started=0
cleanup() {
    local rc=$?
    if test "$started" = 1; then
        timeout 5 docker logs "$NAME" > "$OUT/container.log" 2>&1 || true
        docker rm -f "$NAME" > /dev/null 2>&1 || true
    fi
    printf 'status=%s\nexit_code=%s\n' "$([ "$rc" = 0 ] && echo PASS || echo FAIL)" "$rc" > "$OUT/result.env"
    echo "readback evidence: $OUT"
}
trap cleanup EXIT
for binary in backend_readback_live_test netlink_readback_live_test vpp_live_test; do
    test -x "$ROOT/build/danos-test/$binary"
done
# Do not silently borrow another checkout's build products.
git -C "$ROOT" rev-parse HEAD > "$OUT/source-commit"
git -C "$ROOT" diff HEAD --binary > "$OUT/changes.patch"
git -C "$ROOT" status --porcelain --untracked-files=all > "$OUT/source-status"
while IFS= read -r -d '' source; do
    sha256sum "$ROOT/$source"
done < <(git -C "$ROOT" ls-files --others --exclude-standard -z) > "$OUT/untracked.sha256"
sha256sum "$ROOT/build/danos-test/"{backend_readback_live_test,netlink_readback_live_test} > "$OUT/binaries.sha256"
docker image inspect --format '{{.Id}}' "$VPP_TEST_IMAGE" > "$OUT/dependency-image-id"
cat > "$OUT/startup.conf" <<'CONF'
unix {
 nodaemon
 log /fixture/vpp.log
 cli-listen /fixture/cli.sock
 runtime-dir /fixture/run
}
socksvr { socket-name /fixture/api.sock }
statseg { socket-name /fixture/stats.sock size 32M }
plugins { plugin dpdk_plugin.so { disable } }
CONF
docker run -d --name "$NAME" --network none --cap-add NET_ADMIN \
    -v "$ROOT:/work:ro" -v "$OUT:/fixture" --entrypoint /usr/bin/vpp \
    "$VPP_TEST_IMAGE" -c /fixture/startup.conf > "$OUT/container-id"
started=1
ready=0
for _ in $(seq 1 30); do
    if timeout 2 docker exec "$NAME" vppctl -s /fixture/cli.sock show version > "$OUT/vpp-version" 2>&1; then
        ready=1; break
    fi
    sleep 1
done
test "$ready" = 1
docker exec "$NAME" sh -ec '
    vppctl -s /fixture/cli.sock create loopback interface
    vppctl -s /fixture/cli.sock create loopback interface
    ip link add drift0 type dummy
    ip link add drift1 type dummy
    ip addr add 10.231.1.1/24 dev drift0
    ip addr add 10.231.2.1/24 dev drift1
    ip link set drift0 up
    ip link set drift1 up
' > "$OUT/setup.log" 2>&1
timeout 30 docker exec -e DANOS_VPP_IFINDEX_MAP=101:1,102:2 \
    -e DANOS_READBACK_TEST_VPP_SOCK=/fixture/api.sock "$NAME" \
    stdbuf -oL /work/build/danos-test/backend_readback_live_test 2>&1 | tee "$OUT/vpp-live.log"
timeout 30 docker exec -e DANOS_READBACK_TEST_ISOLATED=1 "$NAME" \
    stdbuf -oL /work/build/danos-test/netlink_readback_live_test 2>&1 | tee "$OUT/netlink-live.log"
timeout 30 docker exec -e VPP_API_SOCK=/fixture/api.sock -e VPP_STAT_SOCK=/fixture/stats.sock \
    -e DANOS_VPP_IFINDEX_MAP=1:1,2:2 "$NAME" \
    stdbuf -oL /work/build/danos-test/vpp_live_test 2>&1 | tee "$OUT/upstream-vpp-live.log"
docker exec "$NAME" vppctl -s /fixture/cli.sock show ip fib > "$OUT/vpp-fib.log"
docker exec "$NAME" vppctl -s /fixture/cli.sock show interface > "$OUT/vpp-interface.log"
docker exec "$NAME" ip route show table 257 > "$OUT/linux-table257.log"
docker exec "$NAME" ip route show table main > "$OUT/linux-main.log"
if grep -q '198.51.100.0/24' "$OUT/linux-main.log"; then
    echo 'FAIL: custom table route leaked into main' >&2; exit 1
fi
