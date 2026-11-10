#!/bin/bash
# Stress closing a connection while an event poll is in flight.
#
#   closerace.sh [runs]     (default: 10)
#
# Starts its own slow mock server (0.3 s per request) on port 18004, so
# that a poll is nearly always running, then
#   1. runs "virsh event --timeout" repeatedly, and
#   2. runs closerace.c, which keeps libvirt's event loop on another thread
#      and closes connections at random moments.
# Both should end without errors. To check for use-after-free, build libvirt
# with -Db_sanitize=address in a separate directory and point
# REDFISH_BUILD_DIR at it (run this under LD_PRELOAD=libasan or compile
# closerace.c with -fsanitize=address as well).

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=${REDFISH_BUILD_DIR:-$root/build}
runs=${1:-10}
port=18004
uri="redfish://admin@127.0.0.1:$port/?transport=http&poll=1"
export LIBVIRT_AUTH_FILE=${LIBVIRT_AUTH_FILE:-$here/auth.conf}

(cd "$root" && exec python3 tests/redfish-mock-server.py --port $port --latency 0.3 --quiet \
    > /dev/null 2>&1 < /dev/null) &
mock=$!
trap 'kill $mock 2>/dev/null' EXIT
sleep 1

fail=0
echo "1. virsh event --timeout, $runs runs"
for i in $(seq 1 "$runs"); do
    out=$("$build/run" "$build/tools/virsh" -c "$uri" event --all --timeout $((2 + i % 3)) 2>&1 </dev/null)
    rc=$?
    # virsh exits with 1 and says "timed out" when the timeout runs out
    if ! { [ $rc -eq 1 ] && grep -q "timed out" <<<"$out"; }; then
        fail=$((fail + 1)); echo "  run $i: exit $rc: $(tail -1 <<<"$out")"
    fi
done
echo "   failures: $fail"

echo "2. closerace.c on its own event loop thread, $runs runs"
bin=$(mktemp)
gcc -O1 -g -I"$root/include" -I"$build/include" -o "$bin" "$here/closerace.c" \
    -L"$build/src" -lvirt -lpthread || exit 1
"$build/run" "$bin" "$uri" "$runs" || fail=$((fail + 1))
rm -f "$bin"
exit $((fail != 0))
