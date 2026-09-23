#!/bin/sh
# Executes real privileged fixtures; use a dedicated host and explicit artifacts.
set -eu
umask 077
if [ "$#" -ne 6 ]; then
  echo 'Usage: run-linux.sh SOURCE NEW_OUTPUT IMAGE_A_ID IMAGE_B_ID PPC_ARTIFACTS ARM_ARTIFACTS' >&2
  exit 2
fi
source=$(cd "$1" && pwd)
output=$2
case "$output" in /*) ;; *) echo 'Output must be absolute' >&2; exit 2;; esac
[ ! -e "$output" ] || { echo 'Output already exists' >&2; exit 2; }
mkdir -m 700 "$output"
bin=$source/build/dev
failed=0
run() {
  name=$1; shift
  echo "RUN $name"
  if "$@" > "$output/$name.txt" 2>&1; then code=0; else code=$?; fi
  printf '%s %s\n' "$name" "$code" >> "$output/exits.txt"
  if [ "$code" -ne 0 ]; then failed=1; fi
  echo "RESULT $name $code"
}
# Never infer a gate pass from a skipped or failed executable.
run ctest ctest --test-dir "$bin" --output-on-failure
run transport sudo "$bin/m3_transport"
run m2 sudo "$bin/m2_linux" "$source" "$bin/m2_tests" "$3" "$4"
run m3 sudo "$bin/m3_linux" "$source" "$bin/m2_tests" "$3" "$4"
run m4-guests sudo "$bin/m4_linux" "$source" "$5" "$6"
run m4-console sudo "$bin/m4_console_linux" "$source" "$3"
run m5 sudo "$bin/m5_linux" "$source" "$3"
run network sudo "$bin/m6_network" "$source" "$3"
run direction sudo "$bin/m6_direction" "$source" "$3"
capacity_parent=$(sudo mktemp -d /tmp/gl6-XXXXXX)
printf '%s\n' "$capacity_parent" > "$output/capacity-location.txt"
run capacity sudo "$bin/m6_capacity" "$source" "$3" "$capacity_parent/direct" "${GRAPHLAB_SAMPLE_SECONDS:-10}"
if sudo test -f "$capacity_parent/direct/capacity.json"; then
 sudo cat "$capacity_parent/direct/capacity.json" > "$output/capacity.json"
fi
{
 date -u '+%Y-%m-%dT%H:%M:%SZ'
 uname -a
 cat /etc/os-release
 getconf _NPROCESSORS_ONLN
 cat /proc/meminfo
 c++ --version
 cmake --version
 ninja --version
 sudo docker version
 sudo docker info
 sudo ovs-vsctl --version
 qemu-system-ppc64 --version
 qemu-system-aarch64 --version
 systemd --version
 ip -Version
 tc -Version
 dpkg-query -W docker.io openvswitch-switch qemu-system-arm qemu-system-ppc libpcap0.8t64 libssl3t64 iproute2
 sudo docker image inspect "$3" "$4"
 sha256sum "$bin/lab-agent" "$bin/lab-api" "$bin/lab-capture" "$bin/lab-terminal" "$bin/m6_capacity"
} > "$output/runtime.txt" 2>&1
{
 sudo systemctl list-units --state=active --no-legend 'graphlab-vm-*' 'graphlab-terminal-*' 'graphlab-cap-*'
 sudo docker ps --filter label=graphlab.run --format '{{.ID}} {{.Names}}'
 sudo ovs-vsctl list-br
} > "$output/cleanup.txt" 2>&1
# The matrix remains an explicit evidence review: running an executable cannot
# silently promote a partial test of T01-T19 to a complete pass.
echo "Evidence saved: $output; review exits.txt and populate the qualification matrix."
exit "$failed"
