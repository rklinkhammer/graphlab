#!/bin/sh
set -eu
export LD_LIBRARY_PATH=/var/tmp/graphlab-m6-tools/root/usr/lib/aarch64-linux-gnu
app_source=/tmp/graphlab-messages-20260924
app_sdk=/tmp/graphlab-messages-sdk-20260924
app_cmake=/var/tmp/graphlab-m6-tools/root/usr/bin/cmake
"$app_cmake" -S "$app_source" -B "$app_source/build/dev" -G Ninja -DCMAKE_MAKE_PROGRAM=/var/tmp/graphlab-m6-tools/root/usr/bin/ninja -DBoost_DIR=/var/tmp/graphlab-m6-tools/boost/lib/cmake/Boost-portable -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/tmp/graphlab-m0-source/build/dev/_deps/nlohmann_json-src -DFETCHCONTENT_SOURCE_DIR_YAML-CPP=/tmp/graphlab-m0-source/build/dev/_deps/yaml-cpp-src -DOPENSSL_INCLUDE_DIR=/var/tmp/graphlab-m6-tools/root/usr/include -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/aarch64-linux-gnu/libcrypto.so.3 -DSQLite3_INCLUDE_DIR=/var/tmp/graphlab-m6-tools/root/usr/include -DSQLite3_LIBRARY=/usr/lib/aarch64-linux-gnu/libsqlite3.so.0 -DPCAP_INCLUDE_DIR=/var/tmp/graphlab-m6-tools/root/usr/include -DPCAP_LIBRARY=/usr/lib/aarch64-linux-gnu/libpcap.so.0.8 -DCMAKE_CXX_FLAGS=-I/var/tmp/graphlab-m6-tools/root/usr/include/aarch64-linux-gnu
"$app_cmake" --build "$app_source/build/dev" --target lab-agent lab-api lab-terminal lab-capture m2_tests message_tests message_linux lab_lifecycle -j 4
"$app_source/build/dev/message_tests"
"$app_cmake" --install "$app_source/build/dev/packages/lab-support" --prefix "$app_sdk"
for node in app-messages app-message-target; do
"$app_cmake" -S "$app_source/docker-nodes/$node" -B "$app_source/build/$node" -G Ninja -DCMAKE_MAKE_PROGRAM=/var/tmp/graphlab-m6-tools/root/usr/bin/ninja -DCMAKE_PREFIX_PATH="$app_sdk" -DLAB_SUPPORT_VERSION=1.6.0 -DOPENSSL_INCLUDE_DIR=/var/tmp/graphlab-m6-tools/root/usr/include -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/aarch64-linux-gnu/libcrypto.so.3
"$app_cmake" --build "$app_source/build/$node"
mkdir -p "$app_source/build/image-$node"
cp "$app_source/build/$node/lab-node" "$app_source/build/image-$node/lab-node"
cp "$app_source/docker-nodes/app-a/Dockerfile" "$app_source/build/image-$node/Dockerfile"
sudo docker build --build-arg BASE_IMAGE=sha256:be5f4184e81e402c997478da98ee34e208b84922da122a94948f033b3f95a458 -t "graphlab-messages/$node:qualification" "$app_source/build/image-$node"
done
sudo chown root:root "$app_source/build/dev/lab-terminal" "$app_source/build/dev/lab-capture"
sudo chmod 755 "$app_source/build/dev/lab-terminal" "$app_source/build/dev/lab-capture"
