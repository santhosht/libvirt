#!/bin/bash
# Walk through the driver's features against a local test server.
#
#   demo.sh [mock|emulator]     (default: mock)
#
# Start the server first:  servers.sh start mock   (or emulator)
# Attach and detach are only run against the mock; the emulator refuses them.

here=$(cd "$(dirname "$0")" && pwd)
server=${1:-mock}
rf() { "$here/rf" -s "$server" "$@"; }
step() { printf '\n=== %s\n' "$*"; }

case "$server" in
    mock)     domxml=$here/domain-mock.xml ;;
    emulator) domxml=$here/domain-emulator.xml ;;
    *) echo "usage: $0 [mock|emulator]" >&2; exit 2 ;;
esac

names() { rf list --all --name 2>/dev/null | grep -v '^$' | sort; }

if ! rf version >/dev/null 2>&1; then
    echo "cannot connect to the $server server; try: $here/servers.sh start $server" >&2
    rf version 2>&1 | tail -2 >&2
    exit 1
fi

step "connection"; rf version | tail -2

step "resource blocks (node devices)"
rf nodedev-list | grep -v '^$' | tr '\n' ' '; echo
first=$(rf nodedev-list | grep -v '^$' | head -1)
rf nodedev-dumpxml "$first"
if [ "$server" = mock ]; then
    for cap in pci storage system; do
        printf '%s: ' "$cap"; rf nodedev-list --cap "$cap" | grep -v '^$' | tr '\n' ' '; echo
    done
fi

step "compose a machine from $(basename "$domxml")"
before=$(names)
rf define "$domxml" || exit 1
name=$(comm -13 <(echo "$before") <(names) | head -1)
[ -n "$name" ] || { echo "could not tell which machine was created" >&2; exit 1; }
echo "machine: '$name'"
rf list --all

step "details"
rf dominfo "$name"
rf dumpxml "$name" | sed -n '/<domain/,/<\/os>/p;/redfish:composition/,/\/redfish:composition/p'

step "power (flip the state twice)"
toggle() {
    if [ "$(rf domstate "$name" | tr -d '\n')" = running ]; then
        rf destroy "$name"
    else
        rf start "$name"
    fi
    rf domstate "$name"
}
rf domstate "$name"; toggle; toggle

if [ "$server" = mock ]; then
    step "attach GPU2, detach GPU1 (the mock needs the machine powered off)"
    rf destroy "$name"
    rf attach-device "$name" "$here/attach-gpu2.xml"
    rf detach-device "$name" "$here/detach-gpu1.xml"
    rf dumpxml "$name" | grep 'redfish:block name'
    rf dominfo "$name" | grep -E 'CPU|memory'
fi

step "decompose"
rf destroy "$name" >/dev/null 2>&1
rf undefine "$name"
rf list --all
