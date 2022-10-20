import argparse
import threading
import statistics
import time
import json
import csv
from concurrent import futures
import struct
import logging
from threading import Condition, Lock
import queue
import grpc
import blockchain_pb2
import blockchain_pb2_grpc
import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestRegressor, GradientBoostingRegressor

block_id_start = 0
block_id_end = 0
local_throughput = 0
episode_cv = Condition()
originator_to_message = {}
committed_messages = 0
agent_exchange_lock = Lock()


class AgentCommServicer(blockchain_pb2_grpc.AgentCommServicer):

    def __init__(self, peer_config, preprepare_queue):
        self.peer_config = peer_config
        self.num_agents = len(self.peer_config['sysconfig']['agents'])
        self.preprepare_queue = preprepare_queue

    def reached_watermark_low(self, request, context):
        global block_id_start, block_id_end, local_throughput

        with episode_cv:
            local_throughput = request.throughput
            block_id_start = request.block_id_start
            block_id_end = request.block_id_now
            episode_cv.notify()

        return blockchain_pb2.google_dot_protobuf_dot_empty__pb2.Empty()

    def send_preprepare(self, request, context):
        with agent_exchange_lock:
            update_originator_to_message(request)
        self.preprepare_queue.put(request)

        return blockchain_pb2.google_dot_protobuf_dot_empty__pb2.Empty()

    def send_prepare(self, request, context):
        global committed_messages
        # logging.info('received prepare message for originator=%s.', request.originator)
        with agent_exchange_lock:
            count = update_originator_to_message(request)
            if count == self.num_agents:
                committed_messages += 1
                # logging.info('committed message for originator=%s.', request.originator)

        return blockchain_pb2.google_dot_protobuf_dot_empty__pb2.Empty()


def update_originator_to_message(request):
    global originator_to_message
    originator = request.originator
    message, count = originator_to_message.get(originator, (None, 0))
    if message is None:
        originator_to_message[originator] = (request, 1)
        count = 1
    else:
        if request == message:
            count += 1
            originator_to_message[originator] = (request, count)
            # logging.info('detected matching exchange message for originator=%s, count updated to %d.', originator, count)
        else:
            logging.info('detected mismatching exchange message for originator=%s.', originator)

    return count


def preprepare_handler(peer_config, preprepare_queue, agent_channels):
    global committed_messages

    def send_prepare(stub, request):
        response = stub.send_prepare(request)

    t = futures.ThreadPoolExecutor(max_workers=len(agent_channels))
    agent_stubs = [blockchain_pb2_grpc.AgentCommStub(channel) for channel in agent_channels]
    logging.info('ready for accepting preprepare messages.')

    while True:
        preprepare = preprepare_queue.get()
        all_tasks = [t.submit(send_prepare, stub, preprepare) for stub in agent_stubs]
        with agent_exchange_lock:
            count = update_originator_to_message(preprepare)
            if count == len(peer_config['sysconfig']['agents']):
                committed_messages += 1
                # logging.info('committed message for originator=%s.', preprepare.originator)
        futures.wait(all_tasks, return_when=futures.ALL_COMPLETED)
        # logging.info('finish sending prepare messages for originator=%s.', preprepare.originator)


def seed_model_and_experience(seed_file, model, experiences_window, experiences_X, experiences_y):
    df = pd.read_csv(seed_file)
    df.rename(columns=str.strip, inplace=True)
    df_X = df.loc[:, 'write_ratio':'reorder'][-experiences_window:]
    df_y = df['throughput'][-experiences_window:]

    bootstrapped_idx = np.random.choice(len(df_X), len(df_X), replace=True)
    training_X = df_X.values[bootstrapped_idx, :]
    training_y = df_y.values[bootstrapped_idx]
    model.fit(training_X, training_y)

    for i in range(len(training_X)):
        experiences_X.append(df_X.values[i])
        experiences_y.append(df_y.values[i])


def run_agent(my_address, peer_config, agent_channels, peer_channel, num_episodes=1000, experiences_window=40):
    global block_id_start, block_id_end, local_throughput, originator_to_message, committed_messages

    def new_episode_info(stub, action):
        response = stub.new_episode_info(action)

    def send_preprepare(stub, request):
        response = stub.send_preprepare(request)

    """ Init """
    experiences_X = []
    experiences_y = []
    t = futures.ThreadPoolExecutor(max_workers=len(agent_channels))
    agent_stubs = [blockchain_pb2_grpc.AgentCommStub(channel) for channel in agent_channels]
    peer_stub = blockchain_pb2_grpc.PeerCommStub(peer_channel)
    initial_blocksize = peer_config['arch']['blocksize']
    initial_early_execution = peer_config['arch']['early_execution']
    initial_reorder = peer_config['arch']['reorder']

    data_store = open('data.csv', 'w')
    csv_writer = csv.writer(data_store)
    csv_writer.writerow(
        ['write_ratio', 'hot_key_ratio', 'trans_arrival_rate', 'execution_delay (us)', 'blocksize', 'early_execution',
         'reorder', 'blocksize * early_execution', 'throughput', 'feature_extraction_overhead (s)', 'training_overhead (s)', 'inference_overhead (s)',
         'communication_overhead (s)', 'episode_duration (s)'])
    block_store = open(peer_config['sysconfig']['log_dir'] + '/blockchain.log', 'rb')

    # set the enumeration matrix as input to the predictor
    #rf = RandomForestRegressor(max_depth=5)
    # rf = GradientBoostingRegressor()
    # seed_model_and_experience('ts_episode_100_6.csv', rf, experiences_window, experiences_X, experiences_y)
    # optimal_action_predicted = []
    actions = []
    actions.append(np.array([10, True, True, 10]))
    actions_matrix = np.vstack(actions)
    enumeration_matrix = np.hstack((np.zeros((actions_matrix.shape[0], 4)), actions_matrix))
    logging.info('learning agent has been initialized.')

    time_records = []
    episode_start_time = None
    feature_extraction_overhead = 0
    training_overhead = 0
    inference_overhead = 0
    block_id = 0

    for episode in range(num_episodes):
        # wait for the peer to reach lower watermark in the current episode
        with episode_cv:
            episode_cv.wait()
        episode_end_time = time.time()
        if episode_start_time is not None:
            time_records[-1].append(round(episode_end_time - episode_start_time, 6))

        """ Extract feature from blocks """
        feature_extraction_start = time.time()
        blocks = []
        while True:
            data = block_store.read(4)
            if not data:
                break
            size, = struct.unpack('I', data)
            data = block_store.read(size)
            block_id += 1
            if block_id > block_id_start:
                block = blockchain_pb2.Block()
                block.ParseFromString(data)
                blocks.append(block)
            if block_id == block_id_end:
                break

        if len(blocks):
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
            local_write_ratio = num_write_trans / num_total_trans
            sorted_keys = sorted(key_access_count, key=key_access_count.get, reverse=True)
            local_hot_key_ratio = key_access_count[sorted_keys[0]] / sum(key_access_count.values())

            # measure the transaction arrival rate
            seconds = blocks[-1].transactions[-1].received_ts.seconds - blocks[0].transactions[0].received_ts.seconds
            nanos = blocks[-1].transactions[-1].received_ts.nanos - blocks[0].transactions[0].received_ts.nanos
            seconds = seconds + nanos * 1e-9
            local_trans_arrival_rate = num_total_trans / seconds

            # measure the execution delay in us
            local_execution_delay = (execution_delay_total_ms / num_total_trans) * 1000
            feature_extraction_overhead = round(time.time() - feature_extraction_start, 6)

        """ Exchange <state, throughput> with other agents """
        communication_start = time.time()
        agent_exchange = blockchain_pb2.AgentExchange(
            originator=my_address, write_ratio=local_write_ratio, hot_key_ratio=local_hot_key_ratio,
            trans_arrival_rate=local_trans_arrival_rate, execution_delay=local_execution_delay,
            throughput=local_throughput)
        with agent_exchange_lock:
            update_originator_to_message(agent_exchange)
        all_tasks = [t.submit(send_preprepare, stub, agent_exchange) for stub in agent_stubs]
        futures.wait(all_tasks, return_when=futures.ALL_COMPLETED)
        # logging.info('finish sending preprepare messages, originator=%s.', my_address)

        # wait for two rounds of communication to complete
        while committed_messages != len(peer_config['sysconfig']['agents']):
            pass
        # logging.info('all exchange messages are committed.')

        # take the median of each peer's state and throughput
        write_ratio = statistics.median([value[0].write_ratio for value in originator_to_message.values()])
        hot_key_ratio = statistics.median([value[0].hot_key_ratio for value in originator_to_message.values()])
        trans_arrival_rate = statistics.median([value[0].trans_arrival_rate for value in originator_to_message.values()])
        execution_delay = statistics.median([value[0].execution_delay for value in originator_to_message.values()])
        throughput = statistics.median([value[0].throughput for value in originator_to_message.values()])
        if episode:
            experiences_y.append(throughput)

        originator_to_message = {}
        committed_messages = 0
        communication_overhead = round(time.time() - communication_start, 6)
        logging.info('local info of episode %d: num_blocks_read = %d, trans_arrival_rate = %f, num_total_trans = %d, seconds = %f',
                     episode + 1, len(blocks), local_trans_arrival_rate, num_total_trans, seconds)   

        # """ Retrain """
        # assert (len(experiences_X) == len(experiences_y))
        # if len(experiences_X) > experiences_window:
        #     experiences_X.pop(0)
        #     experiences_y.pop(0)
        # if episode:
        #     training_start = time.time()
        #     bootstrapped_idx = np.random.choice(len(experiences_X), len(experiences_X), replace=True)
        #     feature_idx = np.array([True, True, True, True, False, False, True, True])
        #     training_X = np.vstack(experiences_X)[bootstrapped_idx, :][:, feature_idx]
        #     training_y = np.array(experiences_y)[bootstrapped_idx]
        #     rf.fit(training_X, training_y)
        #     training_overhead = round(time.time() - training_start, 6)

        #     # save the latest experience to csv file
    
        
        # else:
        #     training_overhead = 0

        # """ Select the best action according to the predictor (M_theta) """
        if len(experiences_X) > 0:
        #     inference_start = time.time()
            enumeration_matrix[:, 0:4] = np.array([write_ratio, hot_key_ratio, trans_arrival_rate, execution_delay])

        #     feature_idx = np.array([True, True, True, True, False, False, True, True])
        #     prediction = rf.predict(enumeration_matrix[:, feature_idx])
        #     # best_index = np.argmax(prediction)
        #     while True:
        #         best_index = np.random.choice(np.flatnonzero(np.isclose(prediction, prediction.max())), replace=True)
        #         best_blocksize = enumeration_matrix[best_index, 4]
        #         best_early_execution = enumeration_matrix[best_index, 5]
        #         best_reorder = enumeration_matrix[best_index, 6]
        #         # if not (best_early_execution == 1 and best_reorder == 1):
        #         #     break
        #         break
        #     experiences_X.append(enumeration_matrix[best_index, :])
        #     inference_overhead = round(time.time() - inference_start, 6)
        #     # optimal_action_predicted.append(prediction[2])
        # else:
            best_blocksize = 10
            best_early_execution = True
            best_reorder = True
            if best_early_execution:
                product = best_blocksize
            else:
                product = -1 * best_blocksize
        experiences_X.append(np.array([write_ratio, hot_key_ratio, trans_arrival_rate, execution_delay,
                                            best_blocksize, best_early_execution, best_reorder, product]))
        inference_overhead = 0
            # optimal_action_predicted.append(0)
        csv_writer.writerow(experiences_X[0].tolist() + [experiences_y[0]] + time_records[0])
        data_store.flush()

        # notify the peer about the action
        action = blockchain_pb2.Action(blocksize=int(best_blocksize),
                                       early_execution=best_early_execution, reorder=best_reorder)
        new_episode_info(peer_stub, action)
        episode_start_time = time.time()
        time_records.append([feature_extraction_overhead, training_overhead, inference_overhead, communication_overhead])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('-a', required=True, dest='my_address')
    args = parser.parse_args()

    LOG_FORMAT = '%(asctime)s - %(levelname)s - %(funcName)s:%(lineno)d - %(message)s'
    logging.basicConfig(level=logging.DEBUG, format=LOG_FORMAT)

    """ Set the same random seed for each peer """
    np.random.seed(0)

    """ Read default configuration """
    with open('../../config/peer_config.json', 'r') as peer_config_f:
        peer_config = json.load(peer_config_f)

    """ Start the grpc server and clients"""
    preprepare_queue = queue.Queue()
    server_address = '[::]:50053'
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    blockchain_pb2_grpc.add_AgentCommServicer_to_server(AgentCommServicer(peer_config, preprepare_queue), server)
    server.add_insecure_port(server_address)
    server.start()
    logging.info('grpc server running at %s.', server_address)

    try:
        peer_channel = grpc.insecure_channel('localhost:50052')
        agent_channels = []  # channels for other agents
        for agent in peer_config['sysconfig']['agents']:
            if agent != args.my_address:
                channel = grpc.insecure_channel(agent)
                agent_channels.append(channel)

        threading.Thread(target=preprepare_handler, args=(peer_config, preprepare_queue, agent_channels)).start()
        run_agent(args.my_address, peer_config, agent_channels, peer_channel, 100, 100)
    finally:
        peer_channel.close()
        for channel in agent_channels:
            channel.close()
