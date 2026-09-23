#!/bin/sh
# Run after building the current project. Copies only node sources into consumers.
set -eu
[ "$#" -eq 4 ] || { echo 'Usage: build-protocol-releases.sh SOURCE FROZEN_SOURCE_1_0 NEW_OUTPUT IMMUTABLE_BASE_ID' >&2; exit 2; }
source=$(cd "$1" && pwd)
frozen=$2
output=$3
base=$4
dependency_prefix=${GRAPHLAB_DEPENDENCY_PREFIX:-/var/tmp/graphlab-m6-tools/root/usr}
target_triplet=$(c++ -dumpmachine)
if [ -d "$dependency_prefix/include/$target_triplet" ]; then
 export CPLUS_INCLUDE_PATH="$dependency_prefix/include/$target_triplet${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}"
fi
case "$base" in sha256:*) ;; *) exit 2;; esac
[ ! -e "$output" ] || exit 2
mkdir -p "$output/sdk-1.0" "$output/sdk-1.1" "$output/frozen-source"
tar -xf "$frozen" -C "$output/frozen-source"
cmake -S "$output/frozen-source/packages/lab-support" -B "$output/sdk-1.0-build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=${GRAPHLAB_DEPENDENCY_PREFIX:-/var/tmp/graphlab-m6-tools/root/usr}"
cmake --build "$output/sdk-1.0-build" -j4
cmake --install "$output/sdk-1.0-build" --prefix "$output/sdk-1.0"
# The full install includes pinned transitive dependencies, but consumers receive
# only installed files, never the project source tree.
cmake --install "$source/build/dev" --prefix "$output/sdk-1.1"
tar -czf "$output/sdk-1.1.tar.gz" -C "$output/sdk-1.1" include lib share
tar -czf "$output/sdk-1.0.tar.gz" -C "$output/sdk-1.0" include lib share
printf '{\n' > "$output/releases.json"
for minor in 1.0 1.1; do
 version=$minor.0
 sdk=$output/sdk-$minor
 sha=sha256:$(sha256sum "$output/sdk-$minor.tar.gz" | cut -d ' ' -f 1)
 printf '"%s":{"version":"%s","sdkSha256":"%s"' "$minor" "$version" "$sha" >> "$output/releases.json"
 for app in app-a app-b; do
  folder=$output/$minor-$app
  mkdir -p "$folder/source" "$folder/context"
  cp "$source/docker-nodes/$app/CMakeLists.txt" "$folder/source/"
  cp -R "$source/docker-nodes/$app/cpp" "$folder/source/"
  cmake -S "$folder/source" -B "$folder/build" -G Ninja -DLAB_SUPPORT_VERSION="$version" "-DCMAKE_PREFIX_PATH=$sdk;${GRAPHLAB_DEPENDENCY_PREFIX:-/var/tmp/graphlab-m6-tools/root/usr}"
  cmake --build "$folder/build"
  cp "$folder/build/lab-node" "$folder/context/"
  cp "$source/docker-nodes/$app/Dockerfile" "$folder/context/"
  sudo docker build --network=none --build-arg BASE_IMAGE="$base" --label "graphlab.lab-support.version=$version" --label "graphlab.lab-support.sha256=$sha" -t "graphlab-m6/$app:protocol-$minor" "$folder/context"
  image=$(sudo docker image inspect "graphlab-m6/$app:protocol-$minor" --format '{{.Id}}')
  printf ',"%s":"%s"' "$app" "$image" >> "$output/releases.json"
  ldd "$folder/context/lab-node" > "$folder/linked-libraries.txt"
  sudo docker run --rm --network=none --entrypoint /bin/sh "$image" -c '! command -v python && ! command -v python3'
 done
 printf '},\n' >> "$output/releases.json"
done
mkdir "$output/incompatible"
c++ -std=c++23 -static-libstdc++ -static-libgcc "$source/tests/qualification/incompatible_node.cpp" -o "$output/incompatible/lab-node"
cp "$source/docker-nodes/app-b/Dockerfile" "$output/incompatible/"
sudo docker build --network=none --build-arg BASE_IMAGE="$base" -t graphlab-m6/incompatible:protocol-2 "$output/incompatible"
bad=$(sudo docker image inspect graphlab-m6/incompatible:protocol-2 --format '{{.Id}}')
sha=sha256:$(sha256sum "$source/tests/qualification/incompatible_node.cpp" | cut -d ' ' -f 1)
printf '"unsupported":{"version":"2.0.0","sdkSha256":"%s","app-b":"%s"},\n"base":"%s"\n}\n' "$sha" "$bad" "$base" >> "$output/releases.json"
sudo docker save graphlab-m6/app-a:protocol-1.0 graphlab-m6/app-b:protocol-1.0 graphlab-m6/app-a:protocol-1.1 graphlab-m6/app-b:protocol-1.1 graphlab-m6/incompatible:protocol-2 | gzip -1 > "$output/images.tar.gz"
sha256sum "$output"/*.tar.gz "$output/releases.json" > "$output/hashes.txt"
