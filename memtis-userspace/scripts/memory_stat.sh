#!/bin/bash

TARGET=$1

while :
do
    cat /sys/fs/cgroup/htmm/memory.stat | grep -e anon_thp -e anon >> ${TARGET}/memory_stat.txt
    cat /sys/fs/cgroup/htmm/memory.hotness_stat >> ${TARGET}/hotness_stat.txt
    cat /proc/vmstat | grep pgmigrate_su >> ${TARGET}/pgmig.txt
    # per-node memory allocation snapshot (same format as pact's harness;
    # parsed by plot_scripts/parse_apps.py:parse_numactl)
    {
        date +%s
        numactl -H
        echo
    } >> ${TARGET}/numactl.txt
    sleep 1
done
