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


/*
Patch-up code take a transaction’s read set and oracle set as input. 
The read set is used to get the current key values from the latest version of the world state. 
Based on this and the oracle set, the smart contract then performs the necessary computations to generate a new write set. 
?If the transaction is not allowed by the logic of the smart contract based on the updated values, it is discarded. 

Finally, in case of success, it generates an updated RW set, which is then compared to the old one. 
If all the keys are a subset of the old RW set, the result is valid and can be committed to the world state and blockchain.
*/

//Patch-up code take a transaction’s read set and oracle set as input. 
//RECORD_VERSION = WORLD STATE
//TRANSACTION IS OLD STATE

bool patch_up_code(Endorsement *transaction, struct RecordVersion record_version,  TransactionProposal proposal) {
    uint64_t block_id = 0;
    uint64_t last_block_id = 0; 
    TransactionProposal proposalread;
    TransactionProposal proposalwrite;

    //keys in read set
    //The read set is used to get the current key values from the latest version of the world state. 
    for (int i = 0; i < transaction->read_set_size(); i++) {
        struct RecordVersion r_record_version;
        string key = transaction->read_set(i).read_key();
        proposalread.add_keys(key);
        proposalread.add_values(kv_get(key, transaction, &r_record_version, block_id));
    }

    //keys in write set
    for (int i = 0; i < transaction->write_set_size(); i++) {
        proposalwrite.add_keys(transaction->write_set(i).write_key());
    }
    /*
    Patch-up code take a transaction’s read set and oracle set as input. 
    ?If the transaction is not allowed by the logic of the smart contract based on the updated values, it is discarded. 
    Finally, in case of success, it generates an updated RW set, which is then compared to the old one. 
    If all the keys are a subset of the old RW set, the result is valid and can be committed to the world state and blockchain.
    */
    // //Based on read set and the oracle set the smart contract then performs the necessary computations to generate a new write_set.
    // //*(endorsement->mutable_received_ts()) = proposal.received_ts();
    if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Get) {
        ycsb_get(proposalread.keys() , transaction);
    } else if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Put) {
        ycsb_put(proposalread.keys() , proposalread.values() , record_version, true, transaction);
    } else {
        smallbank(proposalread.keys(), proposal.type(), proposal.execution_delay(), true, record_version, transaction);
    }    

    //it generates an updated RW set - 
    //old RW keys = proposalread.keys(), write keys = proposalwrite.keys()
    //new RW keys = 
    //which is then compared to the old one

    //compare old readset keys and new readset keys
    for (int i = 0; i < proposalread.keys_size(); i++) {
        bool found = false;
        for (int j = 0; j < transaction->read_set_size(); j++) {
            if (transaction->read_set(j).read_key() == proposalread.keys(i)) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    //compare old writeset keys and new writeset keys
    for (int i = 0; i < proposalwrite.keys_size(); i++) {
        bool found = false;
        for (int j = 0; j < transaction->write_set_size(); j++) {
            if (transaction->write_set(j).write_key() == proposalwrite.keys(i)) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}


    //If all the keys are a subset of the old RW set, the result is valid and can be committed to the world state and blockchain
   
    // for (int read_id = 0; read_id<newreadkeys.size(); read_id++) {
    // bool readfound = false;
    // for (int new_read_id = 0; new_read_id < transaction->read_set_size(); new_read_id++) {
    //     if (newreadkeys[read_id] == transaction->read_set(new_read_id).read_key()) {
    //         readfound = true;
    //         break;
    //     }
    // }
    // if (!readfound) {
    //     return false;
    // }
    // }

    // for (int write_id = 0; write_id < newwritekeyssize; write_id++) {
    //     bool found = false;
    //     for (int new_write_id = 0;new_write_id < transaction->write_set_size(); new_write_id++) {
    //         if (newwritekeys[write_id]== transaction->write_set(new_write_id).write_key()) {
    //             found = true;
    //             break;
    //         }
    //     }
    //     if (!found) {
    //         return false;
    //     }
    // }

return true;
    //updated RW set is compared to the old one. if all the keys are a subset of the old RW set, the result is valid and can be committed to the world state and blockchain
}


bool smallbank(const RepeatedPtrField<string> &keys, TransactionProposal::Type type, int execution_delay, bool expose_write,
               struct RecordVersion record_version, Endorsement *endorsement, uint64_t last_block_id) {
    if (type == TransactionProposal::Type::TransactionProposal_Type_TransactSavings) {
        uint64_t block_id = 0;
        string key = keys[0];

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
        // PutOracle(key, to_string(balance), record_version, expose_write, endorsement);
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
        // PutOracle(key, to_string(balance), record_version, expose_write, endorsement);


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
            // PutOracle(sender_key, to_string(sender_balance), record_version, expose_write, endorsement);
            // PutOracle(receiver_key, to_string(receiver_balance), record_version, expose_write, endorsement);
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

            // OutOracle(key, to_string(balance), record_version, expose_write, endorsement);
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
        // PutOracle(checking_key, to_string(checking_balance), record_version, expose_write, endorsement);
        // PutOracle(saving_key, to_string(saving_balance), record_version, expose_write, endorsement);
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
