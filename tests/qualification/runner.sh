#!/bin/sh
# PATH-isolated runner regression. Never contacts sudo, Docker or the host runtime.
set -eu
runner=$1
root=$2
mkdir -p "$root/bin" "$root/source/build/dev"
cat > "$root/bin/shim" <<'SH'
#!/bin/sh
case "${0##*/}" in
 ctest) exit "${FIXTURE_STATUS:-0}";;
 sudo)
  case "$1" in
   */m*) exit "${FIXTURE_STATUS:-0}";;
   mktemp) echo /tmp/unused-m6-runner-test;;
   test) exit 1;;
  esac;;
esac
exit 0
SH
chmod 755 "$root/bin/shim"
for name in ctest sudo cat getconf c++ cmake ninja qemu-system-ppc64 qemu-system-aarch64 systemd ip tc dpkg-query sha256sum; do
 ln -sf shim "$root/bin/$name"
done
for status in 0 42 77; do
 rm -rf "$root/output-$status"
 result=0
 PATH="$root/bin:$PATH" FIXTURE_STATUS=$status sh "$runner" "$root/source" "$root/output-$status" a b ppc arm > "$root/log-$status" 2>&1 || result=$?
 if [ "$status" = 0 ]; then expected=0; else expected=1; fi
 [ "$result" = "$expected" ] || { cat "$root/log-$status"; exit 1; }
 [ "$(wc -l < "$root/output-$status/exits.txt" | tr -d ' ')" = 10 ]
 grep -q "capacity $status" "$root/output-$status/exits.txt"
 [ -f "$root/output-$status/cleanup.txt" ]
done
