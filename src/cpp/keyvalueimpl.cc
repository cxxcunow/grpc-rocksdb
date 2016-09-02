#include "keyvalue.grpc.pb.h"
#include "keyvalueimpl.h"
#include "rocksdb/db.h"
#include "rocksdb/utilities/transaction.h"
#include "rocksdb/utilities/transaction_db.h"
#include <algorithm>
#include <assert.h>
#include <dirent.h>
#include <grpc++/security/server_credentials.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc/grpc.h>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>

using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;
using keyvalue::KeyValue;
using rocksdb::TransactionDB;

const std::string KeyValueImpl::NO_ERROR = "";
const std::string KeyValueImpl::TABLE_ALREADY_EXISTS = "Table already exists";
const std::string KeyValueImpl::TABLE_DOES_NOT_EXIST = "Table does not exist";
const std::string KeyValueImpl::DISK_ERROR = "Disk error";
const std::string KeyValueImpl::INTERNAL_ERROR = "Internal error";
const std::string KeyValueImpl::KEY_NOT_FOUND = "Key not found";
const std::string KeyValueImpl::CONDITION_NOT_MET = "Condition not met";

ErrorTranslation::ErrorTranslation(grpc::Status s, keyvalue::ErrorCode c) {
    status = s;
    code = c;
}

ErrorTranslation KeyValueImpl::TranslateError(keyvalue::ErrorCode code, rocksdb::Status status) {
    if (code == keyvalue::ErrorCode::NONE && status.ok()) {
        return ErrorTranslation(Status::OK, keyvalue::ErrorCode::NONE); 
    }

    if (code != keyvalue::ErrorCode::NONE) {
        return ErrorTranslation(Status(ToStatusCode(code), ToErrorMsg(code)), code);
    }

    if (status.IsNotFound()) {
        code = keyvalue::ErrorCode::KEY_NOT_FOUND;
    } else {
        code = keyvalue::ErrorCode::INTERNAL_ERROR;   
    }
    
    return ErrorTranslation(Status(ToStatusCode(code), ToErrorMsg(code) + " " + status.ToString()), code);
}

grpc::StatusCode KeyValueImpl::ToStatusCode(keyvalue::ErrorCode err) {
    switch (err) {
        case keyvalue::ErrorCode::TABLE_DOES_NOT_EXIST:
            return grpc::StatusCode::NOT_FOUND;
        case keyvalue::ErrorCode::CONDITION_NOT_MET:
            return grpc::StatusCode::ABORTED;
        case keyvalue::ErrorCode::KEY_NOT_FOUND:
            return grpc::StatusCode::NOT_FOUND;
        case keyvalue::ErrorCode::INTERNAL_ERROR:
            return grpc::StatusCode::INTERNAL;
        default:
            return grpc::StatusCode::INTERNAL;
    }
}

std::string KeyValueImpl::ToErrorMsg(keyvalue::ErrorCode err) {
    switch (err) {
        case keyvalue::ErrorCode::TABLE_DOES_NOT_EXIST:
            return TABLE_DOES_NOT_EXIST;
        case keyvalue::ErrorCode::CONDITION_NOT_MET:
            return CONDITION_NOT_MET;
        case keyvalue::ErrorCode::KEY_NOT_FOUND:
            return KEY_NOT_FOUND;
        case keyvalue::ErrorCode::INTERNAL_ERROR:
            return NO_ERROR;
        default:
            return NO_ERROR; 
    }
}

KeyValueImpl::KeyValueImpl() {
    dbDir = "/tmp";         
    shuffleSource = "0123456789abcdefghijklmnopqrstuvwxyz";
    txnOptions.set_snapshot = true;
     
    // If the tables table doesn't exist, create it.
    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::TransactionDBOptions txnDbOptions;
    rocksdb::Status status = TransactionDB::Open(options, txnDbOptions, dbDir + "/tables", &tablesTable);
    assert(status.ok());

}

Status KeyValueImpl::CreateTable(ServerContext* context,
                                 const keyvalue::CreateTableReq* req,
                                 keyvalue::CreateTableRes* res) {
    mtx.lock();
    
    auto lookup = tableLookup.find(req->tablename());
    
    if (lookup != tableLookup.end()) {
        mtx.unlock();
        res->set_errorcode(keyvalue::ErrorCode::TABLE_ALREADY_EXISTS);
        return Status(grpc::StatusCode::ALREADY_EXISTS, TABLE_ALREADY_EXISTS);
    }
    
    //Table doesn't exist

    //Generate the private name of the table
    std::string tablePrivateName = shuffleSource;
    std::random_shuffle(tablePrivateName.begin(), tablePrivateName.end()); 
    std::string topLevelName = dbDir + "/" + req->tablename();
    std::string tableName = topLevelName + "/" + tablePrivateName;
    
    //Check if top level exists, create if necessary
    DIR* dir = opendir(topLevelName.c_str());
    if (dir) {
        closedir(dir);
    } else if (ENOENT == errno){
        const int dir_err = mkdir(topLevelName.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        if (dir_err == -1) {
            mtx.unlock();
            res->set_errorcode(keyvalue::ErrorCode::DISK_ERROR); 
            return Status(grpc::StatusCode::RESOURCE_EXHAUSTED, DISK_ERROR + " failed to create top level directory"); 
        }
    } else {
        mtx.unlock();
        res->set_errorcode(keyvalue::ErrorCode::DISK_ERROR); 
        return Status(grpc::StatusCode::RESOURCE_EXHAUSTED, DISK_ERROR + " failed to check for top level directory"); 
    }

    //Create the table on disk
    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::TransactionDBOptions txnOptions;
    TransactionDB* newTable;
    rocksdb::Status status = TransactionDB::Open(options, txnOptions, tableName, &newTable);
    
    if (!status.ok()) {
        //Failed to create table on disk
        mtx.unlock();
        res->set_errorcode(keyvalue::ErrorCode::DISK_ERROR); 
        return Status(grpc::StatusCode::RESOURCE_EXHAUSTED, DISK_ERROR + " " + status.ToString()); 
    }
    
    //Insert the table into the tables table
    rocksdb::DB* db = tablesTable->GetBaseDB();
    db->Put(writeOptions, req->tablename(), tableName);   
    
    // Install into the tableLookup
    tableLookup[req->tablename()] = newTable;
             
    mtx.unlock();
    res->set_errorcode(keyvalue::ErrorCode::NONE);
    return Status::OK;
}

Status KeyValueImpl::DeleteTable(ServerContext* context,
                                 const keyvalue::DeleteTableReq* req,
                                 keyvalue::DeleteTableRes* res) {
    mtx.lock();

    auto lookup = tableLookup.find(req->tablename());
    
    if (lookup == tableLookup.end()) {
        //Table doesn't exist
        mtx.unlock();
        res->set_errorcode(keyvalue::ErrorCode::TABLE_DOES_NOT_EXIST);
        return Status(grpc::StatusCode::NOT_FOUND, TABLE_DOES_NOT_EXIST);
    }
    
    //Table exists
    rocksdb::TransactionDB* txnDb = lookup->second;
    
    tableLookup.erase(req->tablename()); 
    
    delete txnDb;
    
    //TODO - clean up the on disk files
     
    mtx.unlock(); 
    res->set_errorcode(keyvalue::ErrorCode::NONE);
    return Status::OK; 
}

Status KeyValueImpl::Get(ServerContext* context,
                         const keyvalue::GetReq* req,
                         keyvalue::GetRes* res) {
    
    rocksdb::Status s = rocksdb::Status::OK();
    keyvalue::ErrorCode err = keyvalue::ErrorCode::NONE;
    rocksdb::TransactionDB* txnDb;
    keyvalue::Item item;
    std::string value; 

    mtx.lock_shared();
    
    auto lookup = tableLookup.find(req->tablename());
    
    if (lookup == tableLookup.end()) {
        err = keyvalue::ErrorCode::TABLE_DOES_NOT_EXIST;
        goto HANDLE_ERROR;
    }
    
    txnDb = lookup->second;
    
    s = txnDb->Get(readOptions, req->key(), &value);
    if (!s.ok()) { goto HANDLE_ERROR; }
    
    item.set_key(req->key());
    item.set_value(value);
        
    res->mutable_item()->CopyFrom(item);
HANDLE_ERROR:
    mtx.unlock_shared();

    ErrorTranslation trans = TranslateError(err, s);
    res->set_errorcode(trans.code);
    return trans.status;
}

Status KeyValueImpl::Put(ServerContext* context,
                         const keyvalue::PutReq* req,
                         keyvalue::PutRes* res) {

    rocksdb::Status s = rocksdb::Status::OK();
    keyvalue::ErrorCode err = keyvalue::ErrorCode::NONE;
    rocksdb::TransactionDB* txnDb;
    rocksdb::Transaction* txn;
    rocksdb::ReadOptions myReadOptions;
    std::string currentValue; 
    
    mtx.lock_shared();
    
    auto lookup = tableLookup.find(req->tablename());
    
    if (lookup == tableLookup.end()) {
        err = keyvalue::ErrorCode::TABLE_DOES_NOT_EXIST;
        goto HANDLE_ERROR;
    }
    
    txnDb = lookup->second;
    
    if (req->condition().size() == 0) {
        s = txnDb->Put(writeOptions, req->item().key(), req->item().value());
        if (!s.ok()) { goto HANDLE_ERROR; }
    } else {
        txn = txnDb->BeginTransaction(writeOptions);
        myReadOptions.snapshot = txn->GetSnapshot();
        s = txn->GetForUpdate(myReadOptions, req->item().key(), &currentValue);
        
        if (!s.ok()) {
            delete txn;
            goto HANDLE_ERROR;
        }

        if (currentValue.compare(req->condition()) != 0) {
            delete txn;
            err = keyvalue::ErrorCode::CONDITION_NOT_MET;
            goto HANDLE_ERROR;
        }
        
        txn->Put(req->item().key(), req->item().value());  
        s = txn->Commit(); 
        delete txn;
        
        if (!s.ok()) {
            err = keyvalue::ErrorCode::CONDITION_NOT_MET;
            goto HANDLE_ERROR;
        }
    }

HANDLE_ERROR:
    mtx.unlock_shared();

    ErrorTranslation trans = TranslateError(err, s);
    res->set_errorcode(trans.code);
    return trans.status;
}

Status KeyValueImpl::Delete(ServerContext* context,
                            const keyvalue::DeleteReq* req,
                            keyvalue::DeleteRes* res) {
    return Status::OK;
}

Status KeyValueImpl::Range(ServerContext* context,
                           const keyvalue::RangeReq* req,
                           ServerWriter<keyvalue::RangeRes>* writer) {
    return Status::OK;
}
