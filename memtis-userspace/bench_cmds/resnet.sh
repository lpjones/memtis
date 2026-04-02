#!/bin/bash

BIN=/proj/TppPlus/tpp/scripts/venv/bin/python
BENCH_RUN="/proj/TppPlus/tpp/scripts/venv/bin/python /proj/TppPlus/tpp/workloads/resnet/resnet_train.py"
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

