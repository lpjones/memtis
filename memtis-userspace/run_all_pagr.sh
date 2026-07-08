#!/bin/bash
# Run all main + memtis workloads on the PAGR-tuned kernel (CXL mode,
# BENCH_DRAM=max after offlining part of node0).
#
# Usage: sudo ./run_all_pagr.sh [version-label]
# Results land in results/<bench>/<version-label>/

set -u

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root"
    exit 1
fi

DIR=/proj/TppPlus/tpp/memtis/memtis-userspace
SCRIPTS=/proj/TppPlus/tpp/scripts
VER=${1:-pagr-tuned}

BENCHES=(cgups stream xsbench gapbs-bfs gapbs-pr resnet btree graph500 silo liblinear)

##### machine prep #####
# local scratch disk (liblinear dataset lives there); not in fstab
if ! mountpoint -q /mnt/pact_storage; then
    mkdir -p /mnt/pact_storage
    mount /dev/sdd /mnt/pact_storage || echo "WARNING: could not mount /mnt/pact_storage"
fi

# shrink fast tier (node0) and disable node1 CPUs (idempotent: only
# offline the difference so re-runs don't shrink node0 further)
cd ${SCRIPTS}
TARGET_OFFLINE=8
offline_now=$(cat /sys/devices/system/node/node0/memory*/online | grep -c '^0$')
need=$(( TARGET_OFFLINE - offline_now ))
if (( need > 0 )); then
    ./mem_dis.sh offline ${need}
else
    echo "node0 already has ${offline_now} blocks offline; skipping"
fi
./disable_cpus.sh
numactl -H

##### run #####
cd ${DIR}
mkdir -p ${DIR}/results
SUMMARY=${DIR}/results/run_all_${VER}.log
echo "==== run_all_pagr $(date) kernel=$(uname -r) ver=${VER} ====" | tee -a ${SUMMARY}

for BENCH in "${BENCHES[@]}"; do
    echo "==== [$(date +%H:%M:%S)] starting ${BENCH} ====" | tee -a ${SUMMARY}
    ${DIR}/scripts/run_bench.sh --cxl -B ${BENCH} -V ${VER} \
        > ${DIR}/results/${BENCH}_${VER}_console.log 2>&1
    rc=$?
    echo "==== [$(date +%H:%M:%S)] finished ${BENCH} rc=${rc} ====" | tee -a ${SUMMARY}
done

echo "==== run_all_pagr done $(date) ====" | tee -a ${SUMMARY}
