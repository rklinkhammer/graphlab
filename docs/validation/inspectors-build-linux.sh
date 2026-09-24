#!/bin/sh
set -eu
export LD_LIBRARY_PATH=/var/tmp/graphlab-m6-tools/root/usr/lib/aarch64-linux-gnu
app_source=/tmp/graphlab-inspectors-20260923
app_sdk=/tmp/graphlab-source-sdk-20260923
app_cmake=/var/tmp/graphlab-m6-tools/root/usr/bin/cmake
"$app_cmake" -S "$app_source" -B "$app_source/build/dev" -G Ninja -DCMAKE_MAKE_PROGRAM=/var/tmp/graphlab-m6-tools/root/usr/bin/ninja -DBoost_DIR=/var/tmp/graphlab-m6-tools/boost/lib/cmake/Boost-portable -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/tmp/graphlab-m0-source/build/dev/_deps/nlohmann_json-src -DFETCHCONTENT_SOURCE_DIR_YAML-CPP=/tmp/graphlab-m0-source/build/dev/_deps/yaml-cpp-src -DOPENSSL_INCLUDE_DIR=/var/tmp/graphlab-m6-tools/root/usr/include -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/aarch64-linux-gnu/libcrypto.so.3 -DSQLite3_INCLUDE_DIR=/var/tmp/graphlab-m6-tools/root/usr/include -DSQLite3_LIBRARY=/usr/lib/aarch64-linux-gnu/libsqlite3.so.0 -DPCAP_INCLUDE_DIR=/var/tmp/graphlab-m6-tools/root/usr/include -DPCAP_LIBRARY=/usr/lib/aarch64-linux-gnu/libpcap.so.0.8 -DCMAKE_CXX_FLAGS=-I/var/tmp/graphlab-m6-tools/root/usr/include/aarch64-linux-gnu
"$app_cmake" --build "$app_source/build/dev" --target lab-agent lab-api lab-terminal lab-capture m2_tests m3_tests -j 4

sudo chown root:root "$app_source/build/dev/lab-terminal" "$app_source/build/dev/lab-capture"
sudo chmod 755 "$app_source/build/dev/lab-terminal" "$app_source/build/dev/lab-capture"
