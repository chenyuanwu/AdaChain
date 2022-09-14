
import json
import csv
from concurrent import futures
import struct
import logging
from threading import Condition
from urllib import response
import grpc
import blockchain_pb2
import blockchain_pb2_grpc
import numpy as np
from sklearn.ensemble import RandomForestRegressor

experiences_y = []
num_finished_peers = 0
episode_cv = Condition()


class AgentCommServicer(blockchain_pb2_grpc.AgentCommServicer):

    def __init__(self, peer_config):
        self.peer_config = peer_config
        self.num_peers = len(self.peer_config['sysconfig']['followers']) + 1

    def end_current_episode(self, request, context):
        global experiences_y
        global num_finished_peers

        if request.is_leader:
            experiences_y.append(request.throughput)

        with episode_cv:
            num_finished_peers += 1
            if num_finished_peers == self.num_peers:
                episode_cv.notify()

        return blockchain_pb2.google_dot_protobuf_dot_empty__pb2.Empty()

def start_new_episode(stub, action):
    response = stub.start_new_episode(action)

def run_agent(peer_config, peer_comm_stubs, num_episodes=1000):
    global experiences_y
    global num_finished_peers
    """ Init """
    num_peers = len(peer_config['sysconfig']['followers']) + 1
    t = futures.ThreadPoolExecutor(max_workers=num_peers)
    initial_blocksize = peer_config['arch']['blocksize']
    initial_early_execution = peer_config['arch']['early_execution']
    initial_reorder = peer_config['arch']['reorder']

    data_store = open('data.csv', 'w')
    csv_writer = csv.writer(data_store)
    csv_writer.writerow(['write_ratio', 'hot_key_ratio', 'trans_arrival_rate',
                        'execution_delay', 'blocksize', 'early_execution', 'execution_delay'])
    block_store = open('../../log/blockchain.log', 'rb')
    experiences_X = []

    # set the enumeration matrix as input to the predictor
    rf = RandomForestRegressor()
    blocksizes = [1] + list(range(10, 200, 10)) + list(range(200, 1000, 50))
    early_execution = [False, True]
    reorder = [False, True]
    actions = []
    for dim_1 in blocksizes:
        for dim_2 in early_execution:
            for dim_3 in reorder:
                actions.append(np.array([dim_1, dim_2, dim_3]))
    actions_matrix = np.vstack(actions)
    enumeration_matrix = np.hstack((np.zeros((actions_matrix.shape[0], 4)), actions_matrix))
    logging.info('learning agent has been initialized.')

    for episode in range(num_episodes):
        # check that the episode ends on all peers
        with episode_cv:
            while num_finished_peers < num_peers:
                episode_cv.wait()
            num_finished_peers = 0

        """ Retrain """
        assert (len(experiences_X) == len(experiences_y))
        if len(experiences_X) > 0:
            bootstrapped_idx = np.random.choice(len(experiences_X), len(experiences_X), replace=True)
            training_X = np.vstack(experiences_X)[bootstrapped_idx, :]
            training_y = np.array(experiences_y)[bootstrapped_idx]
            rf.fit(training_X, training_y)

            # save the latest experience to csv file
            csv_writer.writerow(experiences_X[-1].tolist() + [experiences_y[-1]])
            data_store.flush()

        """ Extract feature from blocks """
        blocks = []
        while True:
            data = block_store.read(4)
            if not data:
                break
            size, = struct.unpack('I', data)
            data = block_store.read(size)
            block = blockchain_pb2.Block()
            block.ParseFromString(data)
            blocks.append(block)

        # measure the write ratio and hot key ratio
        num_write_trans = 0
        num_total_trans = 0
        key_access_count = {}
        execution_delay_total_ms = 0
        for block in blocks:
            for trans in block.transactions:
                if len(trans.read_set) or len(trans.write_set):
                    num_total_trans += 1
                    delay_ms = (trans.execution_end_ts.seconds - trans.execution_start_ts.seconds) * 1e3 + \
                        (trans.execution_end_ts.nanos - trans.execution_start_ts.nanos) * 1e-6
                    execution_delay_total_ms += delay_ms
                if len(trans.write_set):
                    num_write_trans += 1
                for read_item in trans.read_set:
                    key_access_count[read_item.read_key] = key_access_count.get(read_item.read_key, 0) + 1
                for write_item in trans.write_set:
                    key_access_count[write_item.write_key] = key_access_count.get(write_item.write_key, 0) + 1
        write_ratio = num_write_trans / num_total_trans
        sorted_keys = sorted(key_access_count, key=key_access_count.get, reverse=True)
        hot_key_ratio = sorted_keys[0] / sum(key_access_count.values())

        # measure the transaction arrival rate
        seconds = blocks[-1].transactions[-1].received_ts.seconds - blocks[0].transactions[0].received_ts.seconds
        nanos = blocks[-1].transactions[-1].received_ts.nanos - blocks[0].transactions[0].received_ts.nanos
        seconds = seconds + nanos * 1e-9
        trans_arrival_rate = num_total_trans / seconds

        # measure the execution delay in us
        execution_delay = (execution_delay_total_ms / num_total_trans) * 1000

        """ Select the best action according to the predictor (M_theta) """
        if len(experiences_X) > 0:
            enumeration_matrix[:, 0:4] = np.array([write_ratio, hot_key_ratio, trans_arrival_rate, execution_delay])
            prediction = rf.predict(enumeration_matrix)
            best_index = np.argmax(prediction)
            best_blocksize = enumeration_matrix[best_index, 4]
            best_early_execution = enumeration_matrix[best_index, 5]
            best_reorder = enumeration_matrix[best_index, 6]
            experiences_X.append(enumeration_matrix[best_index, :])
        else:
            best_blocksize = initial_blocksize
            best_early_execution = initial_early_execution
            best_reorder = initial_reorder
            experiences_X.append(np.array([write_ratio, hot_key_ratio, trans_arrival_rate, execution_delay,
                                           best_blocksize, best_early_execution, best_reorder]))

        # notify all peers about the action
        action = blockchain_pb2.Action(blocksize=int(best_blocksize),
                                       early_execution=best_early_execution, reorder=best_reorder)
        all_tasks = [t.submit(start_new_episode, stub, action) for stub in stubs]
        futures.wait(all_tasks, return_when=futures.ALL_COMPLETED)


if __name__ == '__main__':
    LOG_FORMAT = '%(asctime)s - %(levelname)s - %(funcName)s:%(lineno)d - %(message)s'
    logging.basicConfig(level=logging.DEBUG)
    """ Read default configuration """
    with open('../../config/peer_config.json', 'r') as peer_config_f:
        peer_config = json.load(peer_config_f)

    """ Start the grpc server and clients"""
    server_address = '[::]:50053'
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    blockchain_pb2_grpc.add_AgentCommServicer_to_server(AgentCommServicer(peer_config), server)
    server.add_insecure_port(server_address)
    server.start()
    logging.info('grpc server running at %s.', server_address)

    peers = []
    channels = []
    stubs = []
    peers.append(peer_config['sysconfig']['leader'])
    for follower in peer_config['sysconfig']['followers']:
        peers.append(follower)
    try:
        for peer in peers:
            channel = grpc.insecure_channel(peer)
            stub = blockchain_pb2_grpc.PeerCommStub(channel)
            channels.append(channel)
            stubs.append(stub)

        run_agent(peer_config, stubs, 10)
    finally:
        for channel in channels:
            channel.close()
