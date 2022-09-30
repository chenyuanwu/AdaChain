#!/bin/bash 
num_keys="10000"
interval="50000"
early_execution="true"
reorder="false"
num_hot_keys="10"
execution_delay="10000"
#high contention, high execution delay, high trans arrival,
#if without early abort is lower than without early abort and number 
for block_size in $(seq 25 25 100);
do
    for write_ratio in $(seq 0.1 0.1 0.9);
    do
        for hot_key_ratio in $(seq 0.1 0.1 0.5);
        do 
            for trans_per_interval in $(seq 50 50 200);
            do
                for early_abort in false true;
                do
                        timeout -s SIGINT 120 ./run.sh $write_ratio $hot_key_ratio $num_keys $num_hot_keys $trans_per_interval $interval $execution_delay $block_size $early_execution $reorder $early_abort 1 -b
                        #timeout -s SIGINT 120 ./run.sh $write_ratio $hot_key_ratio $num_keys $num_hot_keys $trans_per_interval $interval $execution_delay $block_size $early_execution $reorder $early_abort 1 -b & sleep 120
                        #echo "write_ratio:$1 | hot_key_ratio:$2 | num_keys:$3 | num_hot_keys:$4	trans_per_interval:$5	interval:$6	execution_delay:$7	blocksize:$8	xov:${9}	reorder:${10}	early_abort:${11}	block_pipe_num:${12}";
                        #echo "write_ratio:$1 | hot_key_ratio:$2 | num_keys:$3 | num_hot_keys:$4	trans_per_interval:$5	interval:$6	execution_delay:$7	blocksize:$8	xov:${9}	reorder:${10}	early_abort:${11}	block_pipe_num:${12}";
                        echo "early_abort: $early_abort | trans_per_interval: $trans_per_interval | hot_key_ratio: $hot_key_ratio | write_ratio: $write_ratio | block_size: $block_size"
                done
                echo "---------------------------------------------------------------------------------------------------------"
            done
        done
    done
done
