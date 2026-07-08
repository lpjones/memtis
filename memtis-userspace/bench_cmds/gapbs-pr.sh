#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/gapbs
# fixed copy: original twitter-2010.sg has a corrupted inverse-CSR section
# (negative neighbor ids -> pr segfault); rebuilt from the valid out-CSR
GRAPH_DIR=/mnt/pact_storage/workloads/gapbs

BENCH_RUN="${BIN}/pr -f ${GRAPH_DIR}/twitter-2010-fixed.sg -i1000 -t1e-4 -n20"
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"

# twitter-2010.sg: ~12600MB

if [[ "x${NVM_RATIO}" == "x1:32" ]]; then
    BENCH_DRAM="382MB"
elif [[ "x${NVM_RATIO}" == "x1:16" ]]; then
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
