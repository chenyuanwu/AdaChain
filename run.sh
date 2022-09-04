#!/bin/bash 
#RUN THIS OUTSIDE YOUR LBC DIRECTORY
if [ $# -eq 0 ]; then
  echo "Usage: ./run.sh write_ratio hot_key_ratio num_keys num_hot_keys trans_per_interval interval execution_delay"
  exit 1
fi

cd ~/chen2/config
rm -rf client_config.json
#create a json file for inputs given by the user
echo "{" >> client_config.json
echo "\"write_ratio\": $1," >> client_config.json 
echo "\"hot_key_ratio\": $2," >> client_config.json 
echo "\"num_keys\": $3,"    >> client_config.json 
echo "\"num_hot_keys\": $4,">> client_config.json 
echo "\"trans_per_interval\": $5," >> client_config.json 
echo "\"interval\": $6," >> client_config.json
echo "\"execution_delay\": $7" >> client_config.json 
echo "}" >> client_config.json
cd ..
 
rm -rf peer.cc
if [[ "$8" != "" ]]; then
ln -s blocking.cc peer.cc
else
ln -s nonblocking.cc peer.cc
fi

make clean
make

./peer -l -a 10.10.1.2:50052
#./peer -a 10.10.1.4:50052
#./peer -a 10.10.1.4:50053
#./client