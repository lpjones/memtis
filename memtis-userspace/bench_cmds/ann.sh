#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/ann-benchmarks
BENCH_RUN="/proj/TppPlus/tpp/scripts/venv/bin/python /proj/TppPlus/tpp/workloads/ann-benchmarks/run.py --algorithm hnswlib --dataset deep-image-96-angular --local --force"
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB
