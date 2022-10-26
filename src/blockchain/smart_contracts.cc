#include "common.h"
#include "smart_contracts.h"

#include "leveldb/db.h"

bool ycsb_get(const RepeatedPtrField<string> &keys, Endorsement *endorsement, uint64_t last_block_id) {
    uint64_t block_id = 0;
    set_timestamp(endorsement->mutable_execution_start_ts());
    kv_get(keys[0], endorsement, nullptr, block_id); 
    if((last_block_id!=0) && (block_id > last_block_id)){
        //endorsement->set_aborted(true);
        //LOG(INFO) << "aborted in simulation handler";
        return false;
    }
    else {
        //endorsement->set_aborted(false);
        return true;
    }

    set_timestamp(endorsement->mutable_execution_end_ts());

}

void ycsb_put(const RepeatedPtrField<string> &keys, const RepeatedPtrField<string> &values, struct RecordVersion record_version,
              bool expose_write, Endorsement *endorsement) {
    set_timestamp(endorsement->mutable_execution_start_ts());
    kv_put(keys[0], values[0], record_version, expose_write, endorsement);
    set_timestamp(endorsement->mutable_execution_end_ts());
}

/* interface of versioned key value store over leveldb */
string kv_get(const string &key, Endorsement *endorsement, struct RecordVersion *record_version, uint64_t &block_id) {
    string value;
    leveldb::Status s = db->Get(leveldb::ReadOptions(), key, &value);

    uint64_t read_version_blockid = 0;
    uint64_t read_version_transid = 0;
    memcpy(&read_version_blockid, value.c_str(), sizeof(uint64_t));
    memcpy(&read_version_transid, value.c_str() + sizeof(uint64_t), sizeof(uint64_t));

    block_id = read_version_blockid;    

    if (endorsement != nullptr) {
        ReadItem *read_item = endorsement->add_read_set();
        read_item->set_read_key(key);
        read_item->set_block_seq_num(read_version_blockid);
        read_item->set_trans_seq_num(read_version_transid);
    }
    if (record_version != nullptr) {
        record_version->version_blockid = read_version_blockid;
        record_version->version_transid = read_version_transid;
    }

    /* the value returned to smart contracts should not contain version numbers */
    value.erase(0, 16);
    return value;
}

/* interface of versioned key value store over leveldb */
int kv_put(const string &key, const string &value, struct RecordVersion record_version, bool expose_write,
           Endorsement *endorsement) {
    if (endorsement != nullptr) {
        WriteItem *write_item = endorsement->add_write_set();
        write_item->set_write_key(key);
        write_item->set_write_value(value);
    }
    if (expose_write) {
        uint64_t my_version_blockid = record_version.version_blockid;
        uint64_t my_version_transid = record_version.version_transid;
        char *ver = (char *)malloc(2 * sizeof(uint64_t));

        bzero(ver, 2 * sizeof(uint64_t));
        memcpy(ver, &my_version_blockid, sizeof(uint64_t));
        memcpy(ver + sizeof(uint64_t), &my_version_transid, sizeof(uint64_t));
        string internal_value(ver, 2 * sizeof(uint64_t));
        free(ver);

        internal_value += value;
        leveldb::Status s = db->Put(leveldb::WriteOptions(), key, internal_value);
    }

    return 0;
}

//Analogous to existing calls to kv_get and kv_put that record the read and write set key-value pairs, respectively, we add a new call PutOracle to the chaincode API.
int PutOracle(const string &key, const string &value, struct RecordVersion record_version, bool expose_write,
           Endorsement *endorsement) {
    if (endorsement != nullptr) {
        WriteItem *write_item = endorsement->add_write_set();
        write_item->set_write_key(key);
        write_item->set_write_value(value);
    }
    if (expose_write) {
        uint64_t my_version_blockid = record_version.version_blockid;
        uint64_t my_version_transid = record_version.version_transid;
        char *ver = (char *)malloc(2 * sizeof(uint64_t));

        bzero(ver, 2 * sizeof(uint64_t));
        memcpy(ver, &my_version_blockid, sizeof(uint64_t));
        memcpy(ver + sizeof(uint64_t), &my_version_transid, sizeof(uint64_t));
        string internal_value(ver, 2 * sizeof(uint64_t));
        free(ver);

        internal_value += value;
        leveldb::Status s = db->Put(leveldb::WriteOptions(), key, internal_value);
    }

    return 0;
}


bool smallbank(const RepeatedPtrField<string> &keys, TransactionProposal::Type type, int execution_delay, bool expose_write,
               struct RecordVersion record_version, Endorsement *endorsement, uint64_t last_block_id) {
    if (type == TransactionProposal::Type::TransactionProposal_Type_TransactSavings) {
        string key = keys[0];
        uint64_t block_id = 0;
        string value = kv_get(key, endorsement, nullptr, block_id);
        if((last_block_id!=0) && (block_id > last_block_id))  {
            //endorsement->set_aborted(true);
            //LOG(INFO) << "aborted in simulation handler";
            return false;
        }
        
        int balance = stoi(value);
        balance += 1000;

        if (execution_delay > 0) {
            usleep(execution_delay);
        }

        kv_put(key, to_string(balance), record_version, expose_write, endorsement);
    } else if (type == TransactionProposal::Type::TransactionProposal_Type_DepositChecking) {
        uint64_t block_id = 0;
        string key = keys[0];
        
        string value = kv_get(key, endorsement, nullptr, block_id);

        if((last_block_id!=0) && (block_id > last_block_id)){
            //endorsement->set_aborted(true);
            //LOG(INFO) << "aborted in simulation handler";
            return false;
        }
       
        uint64_t balance = stoi(value);
        balance += 1000;

        if (execution_delay > 0) {
            usleep(execution_delay);
        }

        kv_put(key, to_string(balance), record_version, expose_write, endorsement);
    } else if (type == TransactionProposal::Type::TransactionProposal_Type_SendPayment) {
        string sender_key = keys[0];
        string receiver_key = keys[1];

        uint64_t block_id = 0;


        string sender_value = kv_get(sender_key, endorsement,  nullptr, block_id);
        if((last_block_id!=0) && (block_id > last_block_id)){
            //endorsement->set_aborted(true);
            //LOG(INFO) << "aborted in simulation handler";
            return false;
        }
        
        string receiver_value = kv_get(receiver_key, endorsement, nullptr, block_id);
        if((last_block_id!=0) && (block_id > last_block_id)){
            //endorsement->set_aborted(true);
            //LOG(INFO) << "aborted in simulation handler";
            return false;
        }
        
        uint64_t sender_balance = stoi(sender_value);
        uint64_t receiver_balance = stoi(receiver_value);

        if (execution_delay > 0) {
            usleep(execution_delay);
        }

        if (sender_balance >= 5) {
            sender_balance -= 5;
            receiver_balance += 5;

            kv_put(sender_key, to_string(sender_balance), record_version, expose_write, endorsement);
            kv_put(receiver_key, to_string(receiver_balance), record_version, expose_write, endorsement);
        }
    } else if (type == TransactionProposal::Type::TransactionProposal_Type_WriteCheck) {
        string key = keys[0];

        uint64_t block_id = 0;

        string value = kv_get(key, endorsement, nullptr, block_id);
        if((last_block_id!=0) && (block_id > last_block_id))  {
            //endorsement->set_aborted(true);
            //LOG(INFO) << "aborted in simulation handler";
            return false;
        }
        
        uint64_t balance = stoi(value);

        if (execution_delay > 0) {
            usleep(execution_delay);
        }

        if (balance >= 100) {
            balance -= 100;

            kv_put(key, to_string(balance), record_version, expose_write, endorsement);
        }
    } else if (type == TransactionProposal::Type::TransactionProposal_Type_Amalgamate) {
        string checking_key = keys[0];
        string saving_key = keys[1];
        uint64_t block_id = 0;

        string checking_value = kv_get(checking_key, endorsement, nullptr, block_id);
        if((last_block_id!=0) && (block_id > last_block_id)) {
            return false;
        }
        string saving_value = kv_get(saving_key, endorsement, nullptr, block_id);
        if((last_block_id!=0) && (block_id > last_block_id))  {
            return false;
        }
        
        uint64_t checking_balance = stoi(checking_value);
        uint64_t saving_balance = stoi(saving_value);
        checking_balance = checking_balance + saving_balance;
        saving_balance = 0;

        if (execution_delay > 0) {
            usleep(execution_delay);
        }

        kv_put(checking_key, to_string(checking_balance), record_version, expose_write, endorsement);
        kv_put(saving_key, to_string(saving_balance), record_version, expose_write, endorsement);
    } else if (type == TransactionProposal::Type::TransactionProposal_Type_Query) {
        string checking_key = keys[0];
        string saving_key = keys[1];
        uint64_t block_id = 0;

        string checking_value = kv_get(checking_key, endorsement, nullptr, block_id);
        if((last_block_id!=0) && (block_id > last_block_id))  {
            return false;
        }
        string saving_value = kv_get(saving_key, endorsement, nullptr, block_id);
        if((last_block_id!=0) && (block_id > last_block_id)) {
            return false;
        }

        if (execution_delay > 0) {
            usleep(execution_delay);
        }
    }
    return true;
    set_timestamp(endorsement->mutable_execution_end_ts());
}