#!/bin/bash
# Show lifecycle events: one process listens while others change a machine.
#
#   events-test.sh [mock|emulator]     (default: mock)
#
# Polling is set to 1 second so the events show up quickly.

here=$(cd "$(dirname "$0")" && pwd)
server=${1:-mock}
case "$server" in
    mock)     port=${MOCK_PORT:-18001};     domxml=$here/domain-mock.xml ;;
    emulator) port=${EMULATOR_PORT:-5000};  domxml=$here/domain-emulator.xml ;;
    *) echo "usage: $0 [mock|emulator]" >&2; exit 2 ;;
esac
rf() { "$here/rf" -s "$server" "$@"; }

rf version >/dev/null 2>&1 || { echo "cannot connect; try: $here/servers.sh start $server" >&2; exit 1; }

out=$(mktemp)
trap 'rm -f "$out"; kill $listener 2>/dev/null' EXIT
"$here/rf" -s "redfish://admin@127.0.0.1:$port/?transport=http&poll=1" event --all --loop > "$out" 2>&1 &
listener=$!
sleep 3

names() { rf list --all --name 2>/dev/null | grep -v '^$' | sort; }
before=$(names)
echo "define";  rf define "$domxml" >/dev/null; sleep 2.5
name=$(comm -13 <(echo "$before") <(names) | head -1)
echo "start";   rf start "$name" >/dev/null 2>&1; sleep 2.5
echo "destroy"; rf destroy "$name" >/dev/null 2>&1; sleep 2.5
echo "undefine"; rf undefine "$name" >/dev/null 2>&1; sleep 2.5

echo; echo "events seen by the listener:"; cat "$out"
