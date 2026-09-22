#!/bin/sh
# Linux only. No network access: kernel, firmware and matching modules are supplied explicitly.
set -eu
if [ "$#" -ne 5 ]; then echo 'usage: build.sh PROFILE KERNEL FIRMWARE MODULE_DIRECTORY OUTPUT_DIRECTORY' >&2; exit 2; fi
profile=$1
kernel=$2
firmware=$3
modules=$4
out=$5
case "$profile" in
 ppc64le-tcg) compiler=powerpc64le-linux-gnu-g++ ;;
 arm64-kvm) [ "$(uname -m)" = aarch64 ] || { echo "arm64-kvm requires an ARM64 Linux build host" >&2; exit 2; }; compiler=g++ ;;
 *) echo 'unsupported profile' >&2; exit 2 ;;
esac
mkdir -p "$out/root/modules" "$out/root/dev" "$out/root/proc" "$out/root/sys"
[ -c "$out/root/dev/console" ] || sudo mknod "$out/root/dev/console" c 5 1
[ -c "$out/root/dev/null" ] || sudo mknod "$out/root/dev/null" c 1 3
"$compiler" -std=c++23 -static -Os "$(dirname "$0")/init.cpp" -o "$out/root/init"
mkdir -p "$out/root/bin"
cp "$out/root/init" "$out/root/bin/lab-console"
# Include explicit, uncompressed kernel-matching module files only.
find "$modules" -type f -name '*.ko' -exec cp {} "$out/root/modules/" \;
cp "$kernel" "$out/kernel"
cp "$firmware" "$out/firmware"
(cd "$out/root" && find . -print0 | cpio --null -o --format=newc --owner=0:0) | gzip -n > "$out/initrd.gz"
truncate -s 16M "$out/disk.raw"
(cd "$out" && sha256sum kernel firmware initrd.gz disk.raw > SHA256SUMS)
