#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/cgups
BENCH_RUN="${BIN}/gups64-rw 16 move 60 kill 120"
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB
