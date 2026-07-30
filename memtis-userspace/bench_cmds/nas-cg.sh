#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/nas/NPB3.4-OMP/bin
PAIR_RUNNER=/proj/TppPlus/tpp/scripts/run_nas_cg_pair.sh
BENCH_RUN=("${PAIR_RUNNER}" "${BIN}/cg.D.x")
BENCH_DEPS=("${PAIR_RUNNER}" "${BIN}/cg.D.x")
BENCH_CATEGORY="scientific"
BENCH_DESCRIPTION="Two concurrent 8-thread NAS-CG class D solvers"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB
