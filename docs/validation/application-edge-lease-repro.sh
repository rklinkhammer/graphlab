#!/bin/sh
set -eu
# Refuse execution in the host mount or network namespace.
test "$(readlink /proc/self/ns/mnt)" != "$(readlink /proc/1/ns/mnt)"
test "$(readlink /proc/self/ns/net)" != "$(readlink /proc/1/ns/net)"
mount --make-rprivate /
mount -t tmpfs -o mode=755 tmpfs /run
ip link add data0 type dummy
ip addr add 10.233.99.1/24 dev data0
ip link set data0 up
ip link set lo up
/tmp/graphlab-edge-20260923/build/app-streams/lab-node &
fixture_pid=$!
trap 'kill "$fixture_pid" 2>/dev/null || true; wait "$fixture_pid" 2>/dev/null || true' EXIT
for attempt in $(seq 1 50); do
 test ! -S /run/graphlab-node.sock || break
 sleep .02
done
python3 /tmp/verify-edge-lease.py
