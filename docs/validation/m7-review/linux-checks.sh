set -eu
cd /tmp/graphlab-m0-source
export PATH=/var/tmp/graphlab-m6-tools/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/var/tmp/graphlab-m6-tools/root/usr/lib/aarch64-linux-gnu
export CMAKE_PREFIX_PATH=/var/tmp/graphlab-m6-tools/root/usr
app=sha256:be5f4184e81e402c997478da98ee34e208b84922da122a94948f033b3f95a458
runner=sha256:802b24b4b25003369e0369e6a9c08ace37bd0d3c098f63b5972f13f7acff7a5c
restore() {
  cmake -S . -B build/dev -DGRAPHLAB_TEST_CHECKPOINTS=OFF > build/m7-review-restore.txt 2>&1
  cmake --build build/dev -j4 >> build/m7-review-restore.txt 2>&1
  sudo chown root:root build/dev/lab-capture build/dev/lab-terminal
  sudo chmod 755 build/dev/lab-capture build/dev/lab-terminal
}
trap restore EXIT
cmake -S . -B build/dev -DGRAPHLAB_TEST_CHECKPOINTS=ON > build/m7-review-crash-build.txt 2>&1
cmake --build build/dev -j4 >> build/m7-review-crash-build.txt 2>&1
sudo chown root:root build/dev/lab-capture build/dev/lab-terminal
sudo chmod 755 build/dev/lab-capture build/dev/lab-terminal
sudo build/dev/m7_linux "$PWD" /var/tmp/graphlab-m7-guests/ppc /var/tmp/graphlab-m7-guests/arm "$app" "$runner" --crash > build/m7-review-crash.txt 2>&1
restore
trap - EXIT
sudo build/dev/m3_transport > build/m7-review-transport.txt 2>&1
sudo build/dev/m5_linux "$PWD" "$app" > build/m7-review-shared-faults.txt 2>&1
sudo build/dev/m7_isolation > build/m7-review-isolation.txt 2>&1
printf 'PASS review crash recovery, privileged transport, shared faults and isolation rejection\n'
