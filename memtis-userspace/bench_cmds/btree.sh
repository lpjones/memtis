#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/vmitosis-workloads/bin

# NELEMENTS = 200M (btree.c) => ~21GB resident on this testbed
BENCH_RUN="${BIN}/bench_btree_mt"
BENCH_DEPS=("${BIN}/bench_btree_mt")
BENCH_CATEGORY="data-structure"
BENCH_DESCRIPTION="Multithreaded B-tree with 350 million elements"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"

if [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="1300MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="2400MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="4300MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="7100MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="10600MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="25000MB"
fi

export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB
