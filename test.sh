#!/bin/bash 
for block_size in 25 50 100;
do
for write_ratio in $(seq 0.1 0.1 0.9);
do
    for hot_key_ratio in $(seq 0.1 0.1 0.9);
    do 
        for trans_per_interval in $(seq 10 50 1000);
        do
            for execution_delay in $(seq 0 1000 20000);
            do
                for early_abort in true false;
                do
                        timeout -s SIGINT 90 ./run.sh write_ratio hot_key_ratio 10000 num_hot_keys trans_per_interval 50000 execution_delay blocksize true true early_abort 1 -b 
                done
            done
        done
    done
done
done