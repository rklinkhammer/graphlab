set -eu
review_dir=$(mktemp -d /tmp/graphlab-m7-review-source.XXXXXX)
tar -xzf qualification/artifacts/linux-arm64-m7/source.tar.gz -C "$review_dir"
for path in CMakeLists.txt cpp include packages docker-nodes qemu-runners qemu-guests tests; do
  diff -qr -x artifacts "$path" "$review_dir/$path"
done
printf 'PASS current implementation and tests match sealed source archive: %s\n' "$review_dir"
