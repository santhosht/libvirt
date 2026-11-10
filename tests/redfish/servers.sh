#!/bin/bash
# Start, stop and check the two local Redfish test servers.
#
#   servers.sh start|stop|status|restart [mock|emulator|all]   (default: all)
#
#   mock      tests/redfish-mock-server.py            port 18001
#   emulator  DMTF Redfish-Interface-Emulator,        port 5000
#             with its composition switched on by
#             tests/dmtf-emulator-composition.patch
#
# The emulator is fetched from GitHub the first time (git clone, a python
# venv, pip install), so that needs internet access and python3-venv.
#
# Environment:
#   REDFISH_STATE   where logs, pids and the emulator live
#                   (default: /tmp/redfish-test)
#   MOCK_PORT, EMULATOR_PORT, MOCK_ARGS (extra options for the mock server,
#   e.g. "--task-delay 0 --hot-plug")

set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
state=${REDFISH_STATE:-/tmp/redfish-test}
mock_port=${MOCK_PORT:-18001}
emu_port=${EMULATOR_PORT:-5000}
emu_rev=5f51d8a
mkdir -p "$state"

listening_pid() {   # port -> pid of the process listening on it, if any
    ss -ltnp 2>/dev/null | grep -E "[:.]$1[[:space:]]" | grep -oP 'pid=\K[0-9]+' | head -1
}

wait_for() {        # url
    for _ in $(seq 1 30); do
        curl -sL -m 2 -o /dev/null "$1" && return 0
        sleep 1
    done
    return 1
}

start_mock() {
    if [ -n "$(listening_pid "$mock_port")" ]; then
        echo "mock: already listening on port $mock_port"; return 0
    fi
    # shellcheck disable=SC2086
    (cd "$root" && exec setsid nohup python3 tests/redfish-mock-server.py \
        --port "$mock_port" --task-delay 2 ${MOCK_ARGS:-} \
        > "$state/mock.log" 2>&1 < /dev/null) &
    if wait_for "http://127.0.0.1:$mock_port/redfish/v1"; then
        echo "mock: started on port $mock_port (log: $state/mock.log)"
    else
        echo "mock: did not start, see $state/mock.log" >&2; return 1
    fi
}

setup_emulator() {
    local dir=$state/emulator
    if [ ! -d "$dir/.git" ]; then
        echo "emulator: cloning DMTF/Redfish-Interface-Emulator"
        git clone -q https://github.com/DMTF/Redfish-Interface-Emulator.git "$dir" || return 1
    fi
    git -C "$dir" checkout -q "$emu_rev" 2>/dev/null || {
        # a patched tree: put the original back before checking out
        git -C "$dir" checkout -q -- . && git -C "$dir" checkout -q "$emu_rev" || return 1
    }
    if git -C "$dir" apply --check "$root/tests/dmtf-emulator-composition.patch" 2>/dev/null; then
        git -C "$dir" apply "$root/tests/dmtf-emulator-composition.patch" || return 1
        echo "emulator: composition patch applied"
    fi
    if [ ! -x "$state/venv/bin/python" ]; then
        echo "emulator: creating python environment"
        python3 -m venv "$state/venv" || return 1
        "$state/venv/bin/pip" install -q Flask flask_restful flask_httpauth \
            requests StringGenerator Werkzeug || return 1
    fi
}

start_emulator() {
    if [ -n "$(listening_pid "$emu_port")" ]; then
        echo "emulator: already listening on port $emu_port"; return 0
    fi
    setup_emulator || { echo "emulator: setup failed" >&2; return 1; }
    (cd "$state/emulator" && PORT=$emu_port exec setsid nohup "$state/venv/bin/python" \
        emulator.py -port "$emu_port" > "$state/emulator.log" 2>&1 < /dev/null) &
    if wait_for "http://127.0.0.1:$emu_port/redfish/v1/"; then
        echo "emulator: started on port $emu_port (log: $state/emulator.log)"
    else
        echo "emulator: did not start, see $state/emulator.log" >&2; return 1
    fi
}

stop_port() {       # name port
    local pid
    pid=$(listening_pid "$2")
    if [ -n "$pid" ]; then
        kill "$pid" && echo "$1: stopped (pid $pid)"
    else
        echo "$1: not running"
    fi
}

status_port() {     # name port url
    local pid
    pid=$(listening_pid "$2")
    if [ -n "$pid" ]; then
        echo "$1: running on port $2 (pid $pid)"
    else
        echo "$1: not running"
    fi
}

action=${1:-status}
which=${2:-all}

for s in mock emulator; do
    [ "$which" = all ] || [ "$which" = "$s" ] || continue
    case "$action" in
        start)   "start_$s" ;;
        stop)    if [ $s = mock ]; then stop_port mock "$mock_port"; else stop_port emulator "$emu_port"; fi ;;
        restart) if [ $s = mock ]; then stop_port mock "$mock_port"; else stop_port emulator "$emu_port"; fi
                 sleep 1; "start_$s" ;;
        status)  if [ $s = mock ]; then status_port mock "$mock_port"; else status_port emulator "$emu_port"; fi ;;
        *) echo "usage: $0 start|stop|status|restart [mock|emulator|all]" >&2; exit 2 ;;
    esac
done
