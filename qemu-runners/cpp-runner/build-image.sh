#!/bin/sh
# Assemble an offline qualification image from explicitly installed native binaries.
# Archive the resulting rootfs/hashes and image ID; this is not a registry release.
set -eu
[ "$#" -eq 3 ] || { echo 'usage: build-image.sh RUNNER_BINARY NEW_DIRECTORY LOCAL_TAG' >&2; exit 2; }
[ "$(uname -m)" = aarch64 ] || { echo "This image assembly is qualified for native ARM64 only" >&2; exit 2; }
runner=$(realpath "$1")
out=$2
tag=$3
[ ! -e "$out" ]
mkdir -p "$out/root/usr/local/bin" "$out/root/tmp"
cp "$runner" "$out/root/usr/local/bin/lab-qemu-runner"
for binary in "$runner" /usr/bin/qemu-system-aarch64 /usr/bin/qemu-system-ppc64; do
  [ -x "$binary" ]
  if [ "$binary" != "$runner" ]; then mkdir -p "$out/root$(dirname "$binary")"; cp "$binary" "$out/root$binary"; fi
  ldd "$binary" | awk '/=> \/|^[[:space:]]*\// {for (i=1;i<=NF;i++) if ($i ~ /^\//) print $i}' >> "$out/libraries"
done
sort -u "$out/libraries" | while IFS= read -r lib; do
  mkdir -p "$out/root$(dirname "$lib")"
  cp -L "$lib" "$out/root$lib"
done
# QEMU's optional dlopen modules must match the exact installed executable.
if [ -d /usr/lib/aarch64-linux-gnu/qemu ]; then
  mkdir -p "$out/root/usr/lib/aarch64-linux-gnu"
  cp -a /usr/lib/aarch64-linux-gnu/qemu "$out/root/usr/lib/aarch64-linux-gnu/"
fi
(cd "$out/root" && find . -type f -exec sha256sum {} \;) > "$out/files.sha256"
printf '%s\n' 'FROM scratch' 'COPY root/ /' 'LABEL graphlab.qemu-runner="1"' 'ENTRYPOINT ["/usr/local/bin/lab-qemu-runner"]' > "$out/Dockerfile"
docker build --network=none -t "$tag" "$out"
docker image inspect "$tag" > "$out/image.json"
docker image inspect "$tag" --format '{{.Id}}' > "$out/image-id"
