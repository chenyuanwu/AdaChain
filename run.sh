#!/bin/bash 
#RUN THIS OUTSIDE YOUR LBC DIRECTORY
if [ $# -eq 0 ]; then
  echo "Usage: ./run.sh write_ratio hot_key_ratio num_keys num_hot_keys trans_per_interval interval execution_delay"
  exit 1
fi

cd ~/LBC/config
#create a json file for inputs given by the user
echo "{" > client_config.json
echo "\"write_ratio\": $1," >> client_config.json 
echo "\"hot_key_ratio\": $2," >> client_config.json 
echo "\"num_keys\": $3,"    >> client_config.json 
echo "\"num_hot_keys\": $4,">> client_config.json 
echo "\"trans_per_interval\": $5," >> client_config.json 
echo "\"interval\": $6," >> client_config.json
echo "\"execution_delay\": $7" >> client_config.json 
echo "}" >> client_config.json
echo "{\"arch\": {\"blocksize\": $8,\"early_execution\": $9,\"reorder\": ${10},\"early_abort\": ${11},\"block_pipe_num\": ${12}},\"sysconfig\": {\"num_execution_threads\": 16,\"leader\": \"10.10.1.2:50052\",\"followers\": [\"10.10.1.3:50052\",\"10.10.1.4:50052\"]}}" > peer_config.json
cd ..

#echo "write_ratio:$1 | hot_key_ratio:$2 | num_keys:$3 | num_hot_keys:$4	trans_per_interval:$5	interval:$6	execution_delay:$7	blocksize:$8	xov:${9}	reorder:${10}	early_abort:${11}	block_pipe_num:${12}";

rm -rf peer.cc >> /dev/null
if [[ "${13}" != "" ]]; then
ln -s blocking.cc peer.cc
else
ln -s nonblocking.cc peer.cc
fi

make clean >> /dev/null
make >> /dev/null

#sleep 2s
#sleep 3s
#sleep 5s

./peer -l -a 10.10.1.2:50052
#./peer -a 10.10.1.3:50052
#./peer -a 10.10.1.4:50052
#./client
