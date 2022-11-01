# AdaChain

This is the source code for the AdaChain paper, a learning-based blockchain framework that adaptively chooses the best permissioned blockchain architecture in order to optimize effective throughput for dynamic transaction workloads.


## Testbed Setup:
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


***Note:*** We use the cloudlab profile for convenience, however, you can also deploy the testbed on your own infrastructure.

#### Dependencies: 
The following dependencies are required to run the experiments on the node machines:
- g++
- python 3.x(http://www.python.org)
- Python modules: [numpy](http://www.numpy.org/), [pandas](http://pandas.pydata.org/), [statsmodels](http://statsmodels.sourceforge.net/), [jupyter](http://jupyter.org/), grpc (grpcio), [grpcio-tools](https://pypi.python.org/pypi/grpcio-tools/1.0.0), [sklearn](http://scikit-learn.org/stable/)
- pip3


## Compiling the code:
In order to compile the code run the following commands in the root directory of the project:
```
cd src/blockchain
make
```

## Running the peer:
In order to run the peer, the command of the following format will be used to execute the following command on 3 node machines(node 0, node 1, node 2) - one for leader and two for followers:
```
./peer -l -a <node_address>
```
where,  &nbsp;-l flag indicates that the node is a leader node(used only for the leader node), 

&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;-a flag indicates the address of the node,


&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; node_address is the address of the node in the format <ip_address>:<port_number>
<br />

1. Go into the src/blockchain directory in the main project directory.
    ```
    cd src/blockchain
    ```

2. On Node 0(leader node) run the following command:
    ```
    ./peer -l -a 10.10.1.2:50052
    ```

3. On Node 1(follower node) run the following command:
    ```
    ./peer -a 10.10.1.3:50052
    ```

4. On Node 2(follower node) run the following command:
    ```
    ./peer -a 10.10.1.4:50052
    ```
This will start the peer on the node machines.


The peer_config.json file specifies the configuration parameters for the peer. The user can specify the following parameters in the peer_config.json file as per their requirements:
- "trans_watermark_high": number of committed blocks that triggers the learning phase 
- "trans_watermark_low": number of committed blocks that marks the end of an episode
- "timeout": the timeout for slow path
- "leveldb_dir": The path to the database.


## Running the learning agent:
In order to run the learning agent, execute the following command on the node machines running the peer:
Run this from the main project directory on all peer nodes(leader and followers):
```
cd src/learning
python ../learning/learning_agent.py -a <node_address>:50053
```
where, -a flag indicates the address of the node,
        node_address is the address of the node in the format <ip_address>:<port_number>


## Running the client:
In order to run the client, execute the following command on the client machine:

Run this from the main project directory on the client machine:
```
cd src/blockchain
./client
```
The workload can be specified in the client_config.json file as per the requirements of the user. The following parameters can be modified in the client_config.json file:

- "write_ratio": the ratio of write transactions to the number of all transactions  
- "hot_key_ratio": the frequency corresponding to the hottest key accessed
- "num_keys": number of keys in the database 
- "num_hot_keys": contention level
- "trans_per_interval": total number of transactions per interval
- "interval": interval between two consecutive transactions
- "execution_delay": compute intensity of transactions. average delay taken for the to finish all transaction execution.

    
    