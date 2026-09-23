set -eu
date -u
units=$(systemctl list-units --all --no-legend 'graphlab-*')
printf 'Graphlab units: %s\n' "$units"
[ -z "$units" ]
containers=$(docker ps -aq --filter label=graphlab.run)
printf 'Owned containers: %s\n' "$containers"
[ -z "$containers" ]
ovs-vsctl show
[ -z "$(ovs-vsctl list-br)" ]
qos=$(ovs-vsctl --data=bare --no-heading --columns=_uuid list QoS)
printf 'QoS rows: %s\n' "$qos"
[ -z "$qos" ]
ip -j -d link
ip -o link | awk -F ': ' '$2 ~ /^gl/ {print; found=1} END {exit found}'
ip netns list
[ -z "$(ip netns list)" ]
docker network ls
[ "$(docker network ls -q | wc -l)" -eq 3 ]
ps -eo pid,comm,args | awk '$2 ~ /^(lab-capture|lab-terminal|lab-agent|lab-api|qemu-system)/ {print; found=1} END {exit found}'
printf 'PASS final dedicated-runtime audit: no owned process, unit, namespace, link, network, container or OVS/QoS resources\n'
