#!/bin/bash
# Script to get throughput from supervised learning

rm ~/LBC/config/peer_config.json
rm ~/LBC/config/client_config.json

write_ratio="0.1"
hot_key_ratio="0.1"
trans_per_interval="300"
execution_delay="0"
num_hot_keys="10"
N="25"
blocksizerange= [1,10,20,30,40,50,60,70,80,90,100,200,250, 300, 350, 400, 450, 500, 550, 600, 650, 700, 750, 800, 850, 900, 950, 1000]

echo "\n{\n\"write_ratio\": $write_ratio,\n\"hot_key_ratio\": $hot_key_ratio,\n\"num_keys\": 10000,\n\"num_hot_keys\": $num_hot_keys,\n\"trans_per_interval\": $trans_per_interval,\n\"interval\": 50000,\n\"execution_delay\": $execution_delay}" > ~/LBC/config/peer_config.json

make clean
make

for block_size in $(shuf --input-range=0-$(( ${#blocksizerange[*]} - 1 )) -n ${N})
do
    for early_execution in false true;
    do
        for reorder in false true;
        do
            echo "{\n\"arch\": {\n\"blocksize\": $block_size,\n\"early_execution\": $early_execution, \n\"reorder\": $reoder\n},\n\"sysconfig\": {\n\"leveldb_dir\": \"/mydata/testdb\",\n\"log_dir\": \"/mydata/log\",\n\"trans_water_mark\": 10000,\n\"num_execution_threads\": 16,\n\"leader\": \"10.10.1.2:50052\",\n\"followers\": [\n\"10.10.1.3:50052,\"\n\"10.10.1.4:50052\"\n], \n\"agent\": \"10.10.1.2:50053\"\n}\n}" > ~/LBC/config/peer_config.json
            timeout -s SIGINT 25 ./peer -l -a 10.10.1.2:50052 
            sleep 2s 
            timeout -s SIGINT 25 ./peer -a 10.10.1.3:50052
            sleep 3s
            imeout -s SIGINT  25 ./peer -a 10.10.1.4:50052
            sleep 5s
            ./client & sleep 25

        done
    done
done