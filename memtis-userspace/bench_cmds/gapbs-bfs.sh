#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/gapbs
# fixed copy: original twitter-2010.sg has a corrupted inverse-CSR section
# (negative neighbor ids -> pr segfault); rebuilt from the valid out-CSR
GRAPH_DIR=/mnt/pact_storage/workloads/gapbs

# twitter-2010: ~9.7GB resident (both CSR directions touched)
BENCH_RUN="${BIN}/bfs -f ${GRAPH_DIR}/twitter-2010-fixed.sg -n 64 -r 0"
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"

if [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="740MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="1400MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="2520MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="4200MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="6300MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="70000MB"
fi

export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB
