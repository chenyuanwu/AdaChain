# AdaChain

This is the source code for the AdaChain paper, a learning-based blockchain framework that adaptively chooses the best permissioned blockchain architecture in order to optimize effective throughput for dynamic transaction workloads.


## Testbed Setup
We use cloudlab to deploy our testbed, our cloudlab profile used is available [here](https://www.cloudlab.us/p/97d8fe450a0cf392e00b4b8e6d91039234121e35). 

To setup the testbed in cloudlab, please follow the steps below:
1. Login to your [cloudlab](https://www.cloudlab.us/) account and click "Create Experiment" button.
2. Click "Show Profile" [here](https://www.cloudlab.us/p/97d8fe450a0cf392e00b4b8e6d91039234121e35). 
3. Click "Instantiate" button to instantiate the profile.
4. Enter the experiment name and click "Next" button.
5. Click "Finish" button.
6. Navigate to "List View" after the State of the experiment is "Ready" and ssh into the nodes.
7. Copy the Profile Instructions to the nodes and follow the instructions to setup the testbed.

Your testbed is ready to run the experiments!


***Note:*** We use the cloudlab profile for convenience, however, you can also deploy the testbed on your own infrastructure after installing the dependencies below.

#### Dependencies: 
The following dependencies are required to run AdaChain on your own infrastructure:
- g++-9
- LevelDB
- gRPC C++
- Python 3.8
- Python modules: sklearn, numpy, pandas, grpc (grpcio), grpcio-tools, jupyter


## Compiling the code
In order to compile the code, run the following commands in the root directory of the project:
```
cd src/blockchain
make
```

## Running the peer
In order to run the peer process on a server, run the following command inside the `src/blockchain` directory:
```
./peer -l -a <ip_address>:50052
```


The peer_config.json file inside the `config/` directory specifies the configuration parameters for the peer. The user can specify the following parameters in the peer_config.json file as per their requirements:
- "trans_watermark_high": number of processed transactions that marks the end of an episode
- "trans_watermark_low": number of processed transactions that triggers the learning phase 
- "timeout": the timeout for slow path
- "leveldb_dir": The path to the database


## Running the learning agent
In order to run the learning agent on a server, run the following command inside the `src/learning` directory:
```
python learning_agent.py -a <node_address>:50053
```


## Running the client
In order to run the client, execute the following command on the client machine inside the `src/blockchain` directory:

```
./client
```
The workload parameters can be specified in the client_config.json file. Please refer to the paper for the meaning of each parameter.

    
    