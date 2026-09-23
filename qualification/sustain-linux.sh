#!/bin/sh
# Declared M6 T18 envelope: one hour of serialized measured traffic.
set -eu
[ "$#" -eq 3 ] || { echo 'Usage: sudo sustain-linux.sh SOURCE IMAGE_ID NEW_SHORT_OUTPUT' >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || exit 2
source=$(cd "$1" && pwd)
image=$2
output=$3
case "$output" in /tmp/gl6-*) ;; *) echo 'Use a fresh short /tmp/gl6-* output path' >&2; exit 2;; esac
[ ! -e "$output" ] || exit 2
mkdir -m 700 "$output"
bin=$source/build/dev
cp "$source/qualification/remaining-gates.md" "$output/declared-criteria.md"
{
 date -u '+%Y-%m-%dT%H:%M:%SZ'
 uname -a
 getconf _NPROCESSORS_ONLN
 cat /proc/meminfo
 sha256sum "$bin/m6_capacity" "$bin/lab-capture" "$bin/lab-terminal" "$source/tests/qualification/capacity.cpp" "$output/declared-criteria.md"
 docker image inspect "$image"
 systemctl list-units --state=active --no-legend 'graphlab-*'
 docker ps --filter label=graphlab.run --format '{{.ID}} {{.Names}}'
} > "$output/runtime-before.txt"
failed=0
for shape in direct triangle; do
 echo "START $shape $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
 if [ "$shape" = triangle ]; then export GRAPHLAB_CAPACITY_TRIANGLE=1; else unset GRAPHLAB_CAPACITY_TRIANGLE; fi
 if GRAPHLAB_CAPACITY_SUSTAINED=1 "$bin/m6_capacity" "$source" "$image" "$output/$shape" 300 > "$output/$shape.txt" 2>&1; then code=0; else code=$?; failed=1; fi
 printf '%s %s\n' "$shape" "$code" >> "$output/exits.txt"
 if [ -f "$output/$shape/capacity.json" ]; then
  if ! "$bin/lab-qualify" summarize "$output/$shape/capacity.json" > "$output/$shape.md"; then failed=1; fi
 fi
 echo "END $shape $code $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
done
{
 date -u '+%Y-%m-%dT%H:%M:%SZ'
 systemctl list-units --state=active --no-legend 'graphlab-*'
 docker ps --filter label=graphlab.run --format '{{.ID}} {{.Names}}'
 ovs-vsctl list-br
 ip -j link show
} > "$output/cleanup.txt"
exit "$failed"
