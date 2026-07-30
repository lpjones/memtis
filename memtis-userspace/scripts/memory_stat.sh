#!/bin/bash

set -u

TARGET=$1
CGROUP=${MEMTIS_CGROUP:-/sys/fs/cgroup/htmm}

while :
do
    timestamp=$(date +%s)
    printf '%s ' "${timestamp}" >> "${TARGET}/memory_current.txt"
    cat "${CGROUP}/memory.current" >> "${TARGET}/memory_current.txt"
    cat "${CGROUP}/memory.stat" | grep -e '^anon ' -e '^anon_thp ' \
        >> "${TARGET}/memory_stat.txt"
    cat "${CGROUP}/memory.hotness_stat" >> "${TARGET}/hotness_stat.txt"
    {
        printf '%s\n' "${timestamp}"
        cat "${CGROUP}/memory.numa_stat"
        echo
    } >> "${TARGET}/memory_numa_stat.txt"
    cat /proc/vmstat | grep pgmigrate_su >> "${TARGET}/pgmig.txt"
    # per-node memory allocation snapshot (same format as pact's harness;
    # parsed by plot_scripts/parse_apps.py:parse_numactl)
    {
        date +%s
        numactl -H
        echo
    } >> "${TARGET}/numactl.txt"
    sleep 1
done
