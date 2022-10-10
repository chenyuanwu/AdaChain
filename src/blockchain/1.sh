#!/bin/bash
# Script to get throughput from supervised learning
echo > script.csv

write_ratio="0.1"
hot_key_ratio="0.1"
trans_per_interval="300"
execution_delay="0"
num_hot_keys="10"
count="25"
arr=("1" "10" "20" "30" "40" "50" "60" "70" "80" "90" "100" "200" "250" "300" "350" "400" "450" "500" "550" "600" "650" "700" "750" "800" "850" "900" "950" "1000")
rand=$[ $RANDOM % 29 ]

printf "write_ratio\t,hot_key_ratio\t,trans_arrival_rate\t,execution_delay (us)\t,blocksize\t,early_execution\t,reorder\t,throughput\n" >> script.csv


echo "{
	    \"write_ratio\": $write_ratio,
            \"hot_key_ratio\": $hot_key_ratio,
            \"num_keys\": 10000,
            \"num_hot_keys\": $num_hot_keys,
            \"trans_per_interval\": $trans_per_interval,
            \"interval\": 50000,
            \"execution_delay\": $execution_delay
    }" > ~/LBC/config/client_config.json



for block_size in $(seq $count)

#for block_size in $(shuf --input-range=0-$(( ${#blocksizerange[*]} - 1 )) -n ${N})
do
blocksizef=${arr[$rand]}
    for early_execution in false true
    do
        for reorder in false true
        do
            echo "{
                        \"arch\": {
                            \"blocksize\": $blocksizef,
                            \"early_execution\": $early_execution, 
                            \"reorder\": $reorder
                            },
                        \"sysconfig\": {
                            \"leveldb_dir\": \"/mydata/testdb\",
                            \"log_dir\": \"/mydata/log\",
                            \"trans_water_mark\": 10000,
                            \"num_execution_threads\": 16,
                            \"leader\": \"10.10.1.2:50052\",
                            \"followers\": 
                                [
                                    \"10.10.1.3:50052\",
                                    \"10.10.1.4:50052\"
                                ], 
                            \"agent\": \"10.10.1.2:50053\"
                        }
                    }" > ~/LBC/config/peer_config.json
        make clean >> /dev/null
        make >> /dev/null
            
	timeout -s SIGINT 35 sh -c '{ ./peer -l -a 10.10.1.2:50052; } 2>&1 > throughput.log'
	 throughput=$(grep throughput throughput.log | awk '{ print $NF }')

 
	printf "$write_ratio\t,$hot_key_ratio\t,$trans_per_interval\t,$execution_delay\t,$blocksizef\t,$early_execution\t,$reorder\t,$throughput\n" >> script.csv

	done
    done
done
