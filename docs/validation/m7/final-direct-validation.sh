set -eu
cd /tmp/graphlab-m0-source
export PATH=/var/tmp/graphlab-m6-tools/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/var/tmp/graphlab-m6-tools/root/usr/lib/aarch64-linux-gnu
export CMAKE_PREFIX_PATH=/var/tmp/graphlab-m6-tools/root/usr
app=$(sudo docker image inspect graphlab-m6/app-a:qualification --format '{{.Id}}')
runner=$(sudo cat /var/tmp/gl7-runner-v2/image-id)
cmake -S . -B build/dev -DGRAPHLAB_TEST_CHECKPOINTS=ON > build/m7-neutral-crash-build.txt 2>&1
cmake --build build/dev -j4 >> build/m7-neutral-crash-build.txt 2>&1
sudo chown root:root build/dev/lab-capture build/dev/lab-terminal
sudo chmod 755 build/dev/lab-capture build/dev/lab-terminal
sudo build/dev/m7_linux "$PWD" /var/tmp/graphlab-m7-guests/ppc /var/tmp/graphlab-m7-guests/arm "$app" "$runner" --crash > build/m7-neutral-crash.txt 2>&1
cmake -S . -B build/dev -DGRAPHLAB_TEST_CHECKPOINTS=OFF > build/m7-neutral-final-build.txt 2>&1
cmake --build build/dev -j4 >> build/m7-neutral-final-build.txt 2>&1
sudo chown root:root build/dev/lab-capture build/dev/lab-terminal
sudo chmod 755 build/dev/lab-capture build/dev/lab-terminal
sudo build/dev/m7_linux "$PWD" /var/tmp/graphlab-m7-guests/ppc /var/tmp/graphlab-m7-guests/arm "$app" "$runner" > build/m7-neutral-backends.txt 2>&1
ctest --test-dir build/dev --output-on-failure > build/m7-neutral-ctest.txt 2>&1
sudo build/dev/m3_transport > build/m7-neutral-transport.txt 2>&1
sudo build/dev/m5_linux "$PWD" "$app" > build/m7-neutral-shared-faults.txt 2>&1
sudo build/dev/m7_isolation > build/m7-isolation-final.txt 2>&1
printf 'PASS final direct queues, crash recovery, portable Linux tests and shared fault regressions\n'
