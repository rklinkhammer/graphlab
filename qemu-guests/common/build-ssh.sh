#!/bin/sh
# Optional static Dropbear for the minimal guest templates. Custom lab support stays C++.
set -eu
if [ "$#" -ne 3 ]; then echo 'usage: build-ssh.sh PROFILE DROPBEAR_TARBALL OUTPUT_DIRECTORY' >&2; exit 2; fi
profile=$1
source=$2
out=$3
printf '%s  %s\n' '783f50ea27b17c16da89578fafdb6decfa44bb8f6590e5698a4e4d3672dc53d4' "$source" | sha256sum -c -
mkdir -p "$out/dropbear-build"
tar -xjf "$source" -C "$out/dropbear-build" --strip-components=1
case "$profile" in
 ppc64le-tcg) target=--host=powerpc64le-linux-gnu ;;
 arm64-kvm) [ "$(uname -m)" = aarch64 ] || { echo "arm64-kvm requires an ARM64 Linux build host" >&2; exit 2; }; target= ;;
 *) exit 2 ;;
esac
printf '%s\n' '#define DROPBEAR_SVR_PASSWORD_AUTH 0' '#define DROPBEAR_SVR_PAM_AUTH 0' > "$out/dropbear-build/localoptions.h"
(cd "$out/dropbear-build" && ./configure $target --enable-static --disable-zlib --disable-shadow --disable-syslog --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx && make -j4 PROGRAMS='dropbear dropbearkey')
mkdir -p "$out/root/usr/sbin" "$out/root/etc/dropbear" "$out/root/root/.ssh" "$out/root/bin" "$out/root/dev/pts" "$out/root/tmp"
cp "$out/dropbear-build/dropbear" "$out/root/usr/sbin/"
printf 'root:x:0:0:Graphlab fixture:/root:/bin/lab-console\n' > "$out/root/etc/passwd"
printf 'root:x:0:\n' > "$out/root/etc/group"
printf '/bin/lab-console\n' > "$out/root/etc/shells"
# Keys are generated separately and kept private; never commit them.
