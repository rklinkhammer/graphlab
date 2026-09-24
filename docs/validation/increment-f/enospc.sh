#!/bin/sh
set -eu
source=/tmp/graphlab-f-20260924/build/dev
for mode in messages logs application; do
  directory=$(mktemp -d /var/tmp/graphlab-f-full-XXXXXX)
  trap 'umount "$directory"; rmdir "$directory"' EXIT
  mount -t tmpfs -o size=8m,mode=700 tmpfs "$directory"
  case "$mode" in
    messages) "$source/message_linux" "$directory";;
    logs) "$source/process_log_linux" --full "$directory";;
    application) "$source/application_full_linux" "$directory";;
  esac
  umount "$directory"
  rmdir "$directory"
  trap - EXIT
done
