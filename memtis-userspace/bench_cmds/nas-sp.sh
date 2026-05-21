#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/nas/NPB3.4-OMP/bin
BENCH_RUN="${BIN}/sp.D.x"
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB
