#!/bin/sh
# Dedicated-host fixture services. Invoke as the unprivileged lab operator.
set -eu
mode=$1
source=$2
root=$3
bin=$source/build/dev
case "$root" in /var/tmp/gl6-*) ;; *) echo 'Use a new /var/tmp/gl6-* fixture root' >&2; exit 2;; esac
case "$mode" in
 prepare)
  [ ! -e "$root" ]; mkdir -m 700 "$root"
  "$bin/m2_tests" --fixtures "$source" "$root" "$(sudo docker image inspect graphlab-m6/app-a:qualification --format '{{.Id}}')"
  # The fixture generator's development topology becomes capture-required.
  sed -i 's/"required": false/"required": true/' "$root/m2.yaml"
  sudo mkdir -m 750 "$root/rpc"
  sudo chgrp "$(id -g)" "$root/rpc"
  sudo mkdir -m 700 "$root/state"
  chmod 755 "$root"
  "$bin/lab-api" init-auth "$root/auth.json" > "$root/password"
  chmod 600 "$root/password"
  ;;
 agent)
  sudo systemd-run --unit=graphlab-m6-agent --collect --property=KillMode=process "$bin/lab-agent" --socket "$root/rpc/agent.sock" --topologies "$root" --lock "$root/artifacts.lock.json" --state "$root/state" --allow-uid "$(id -u)"
  ;;
 api)
  sudo systemd-run --unit=graphlab-m6-api --collect --uid="$(id -u)" "$bin/lab-api" --socket "$root/rpc/agent.sock" --auth "$root/auth.json" --assets "$source/console/web/dist" --port 18089 --agent-uid 0
  ;;
 stop-api) sudo systemctl stop graphlab-m6-api;;
 stop-agent) sudo systemctl stop graphlab-m6-agent;;
 *) exit 2;;
esac
