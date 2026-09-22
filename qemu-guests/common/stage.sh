#!/bin/sh
# Copy immutable blobs and the separately protected SSH credential into an agent state directory.
set -eu
if [ "$#" -ne 2 ]; then echo 'usage: stage.sh EXAMPLE_DIRECTORY AGENT_STATE_DIRECTORY' >&2; exit 2; fi
example=$1
state=$2
(cd "$example" && sha256sum -c SHA256SUMS)
sudo install -d -m 700 "$state" "$state/artifacts" "$state/credentials"
for file in kernel firmware initrd.gz disk.raw known_hosts; do
 if [ -f "$example/artifacts/$file" ]; then
  hash=$(sha256sum "$example/artifacts/$file")
  hash=${hash%% *}
  sudo install -o root -g root -m 400 "$example/artifacts/$file" "$state/artifacts/$hash"
 fi
done
if [ -f "$example/artifacts/client.key" ]; then
 sudo install -o root -g root -m 600 "$example/artifacts/client.key" "$state/credentials/guest-linux.key"
fi
