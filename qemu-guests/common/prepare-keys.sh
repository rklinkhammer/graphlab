#!/bin/sh
set -eu
if [ "$#" -ne 2 ]; then echo 'usage: prepare-keys.sh OUTPUT_DIRECTORY NATIVE_DROPBEARKEY' >&2; exit 2; fi
out=$1
keytool=$2
umask 077
[ -f "$out/client.key" ] || ssh-keygen -q -t ed25519 -N '' -f "$out/client.key"
[ -f "$out/root/etc/dropbear/dropbear_ed25519_host_key" ] || "$keytool" -t ed25519 -f "$out/root/etc/dropbear/dropbear_ed25519_host_key" >/dev/null
cp "$out/client.key.pub" "$out/root/root/.ssh/authorized_keys"
chmod 700 "$out/root/root" "$out/root/root/.ssh"
chmod 600 "$out/root/root/.ssh/authorized_keys" "$out/client.key"
"$keytool" -y -f "$out/root/etc/dropbear/dropbear_ed25519_host_key" | awk '/^ssh-ed25519 / {print "172.31.243.10 " $1 " " $2}' > "$out/known_hosts"
