set -eu
cd /tmp/graphlab-m0-source
export PATH=/var/tmp/graphlab-m6-tools/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/var/tmp/graphlab-m6-tools/root/usr/lib/aarch64-linux-gnu
export CMAKE_PREFIX_PATH=/var/tmp/graphlab-m6-tools/root/usr
app_a=$(sudo docker image inspect graphlab-m6/app-a:qualification --format '{{.Id}}')
app_b=$(sudo docker image inspect graphlab-m6/app-b:qualification --format '{{.Id}}')
runner=$(sudo cat /var/tmp/gl7-runner-v2/image-id)
cmake -S . -B build/dev -DGRAPHLAB_TEST_CHECKPOINTS=ON > build/m7-crash-build.txt 2>&1
cmake --build build/dev -j4 >> build/m7-crash-build.txt 2>&1
sudo chown root:root build/dev/lab-capture build/dev/lab-terminal
sudo chmod 755 build/dev/lab-capture build/dev/lab-terminal
sudo build/dev/m7_linux "$PWD" /var/tmp/graphlab-m7-guests/ppc /var/tmp/graphlab-m7-guests/arm "$app_a" "$runner" --crash > build/m7-crash.txt 2>&1
cmake -S . -B build/dev -DGRAPHLAB_TEST_CHECKPOINTS=OFF > build/m7-final-build.txt 2>&1
cmake --build build/dev -j4 >> build/m7-final-build.txt 2>&1
sudo chown root:root build/dev/lab-capture build/dev/lab-terminal
sudo chmod 755 build/dev/lab-capture build/dev/lab-terminal
sudo build/dev/m7_linux "$PWD" /var/tmp/graphlab-m7-guests/ppc /var/tmp/graphlab-m7-guests/arm "$app_a" "$runner" > build/m7-final-backends.txt 2>&1
ctest --test-dir build/dev --output-on-failure > build/m7-final-ctest.txt 2>&1
sudo build/dev/m3_transport > build/m7-final-transport.txt 2>&1
sudo build/dev/m2_linux "$PWD" "$PWD/build/dev/m2_tests" "$app_a" "$app_b" > build/m7-regression-m2.txt 2>&1
sudo build/dev/m3_linux "$PWD" "$PWD/build/dev/m2_tests" "$app_a" "$app_b" > build/m7-regression-m3.txt 2>&1
sudo build/dev/m4_linux "$PWD" /var/tmp/graphlab-m6-guests/ppc /var/tmp/graphlab-m6-guests/arm > build/m7-regression-m4.txt 2>&1
sudo build/dev/m5_linux "$PWD" "$app_a" > build/m7-regression-m5.txt 2>&1
printf 'PASS M7 final Linux stages and shared-backend regressions\n'
