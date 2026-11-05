#!/bin/sh
# Verify the installed DANOS command names resolve from /usr/bin with a
# non-login PATH (the same PATH a non-root login shell starts with).
set -eu
prefix=${1:-}
bin="${prefix}/usr/bin"
for command in danos-mgrd danos-netconfd danos-netconf-subsystem; do
    test -x "$bin/$command" || { echo "FAIL missing executable: $bin/$command"; exit 1; }
    resolved=$(PATH="${bin}:/usr/bin:/bin" command -v "$command" || true)
    test "$resolved" = "$bin/$command" || {
        echo "FAIL $command resolves to '${resolved:-nothing}'"; exit 1;
    }
done
for command in danos-fib danos-cli; do
    if PATH="${bin}:/usr/bin:/bin" command -v "$command" >/dev/null 2>&1; then
        echo "FAIL unexpected executable: $command"
        exit 1
    fi
done
echo 'PASS installed DANOS commands resolve from /usr/bin'
