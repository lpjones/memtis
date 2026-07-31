#!/bin/bash
# py_bin="/proj/TppPlus/tpp/scripts/venv/bin/python"

trials=3
node=1
prefix="pagr"

function bench_run() {
    local prefix=$1; shift
    local bench=$1; shift
    local trials=$1; shift
    local node=$1; shift
    for ((i=1; i<=trials; i++)); do
        echo "Running $prefix $bench trial $i"
        # Add actual benchmark execution here
        ./run_bench.sh --cxl -B $bench -V "$prefix-n$node-$i"
        mv ../results/$bench/$prefix-n$node-$i /mnt/pact_storage/results/$bench/
    done
}

bench_run "$prefix" "cgups" $trials $node
bench_run "$prefix" "resnet" $trials $node
bench_run "$prefix" "stream" $trials $node
bench_run "$prefix" "xsbench" $trials $node
bench_run "$prefix" "graph500" $trials $node
bench_run "$prefix" "pagerank" $trials $node
bench_run "$prefix" "silo" $trials $node
bench_run "$prefix" "liblinear" $trials $node
bench_run "$prefix" "btree" $trials $node
bench_run "$prefix" "dlrm" $trials $node
bench_run "$prefix" "gapbs-bfs" $trials $node
bench_run "$prefix" "nas-cg" $trials $node
bench_run "$prefix" "llama-matmul" $trials $node
bench_run "$prefix" "diskann" $trials $node

