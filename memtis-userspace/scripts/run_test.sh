#!/bin/bash
# py_bin="/proj/TppPlus/tpp/scripts/venv/bin/python"

trials=3
node=7

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

# bench_run "og" "cgups" $trials $node
# bench_run "og" "resnet" $trials $node
# bench_run "og" "stream" $trials $node
# bench_run "og" "xsbench" $trials $node
# bench_run "og" "graph500" $trials $node
# bench_run "og" "pagerank" $trials $node
bench_run "og" "silo" $trials $node
bench_run "og" "liblinear" $trials $node
bench_run "og" "btree" $trials $node
bench_run "og" "dlrm" $trials $node
bench_run "og" "gapbs-bfs" $trials $node
bench_run "og" "nas-cg" $trials $node
bench_run "og" "llama-matmul" $trials $node
bench_run "og" "diskann" $trials $node

