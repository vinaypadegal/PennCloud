#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <grpcpp/grpcpp.h>
#include "../build/kvstore.grpc.pb.h"

using grpc::ServerContext;
using grpc::Status;
using kvstore::KeyValueStore;
using kvstore::PutRequest;
using kvstore::GetRequest;
using kvstore::CPutRequest;
using kvstore::DeleteRequest;
using kvstore::GetReply;
using kvstore::Reply;

using namespace std;

class WorkerNodeKVS final : public KeyValueStore::Service {
    Status Get(ServerContext* context, const GetRequest* request, GetReply* reply) override {
        if (kvs.find(request->row()) == kvs.end()) {
            reply->set_success(false);
            return Status::CANCELLED;
        }
        if (kvs.at(request->row()).find(request->column()) == kvs.at(request->row()).end()) {
            reply->set_success(false);
            return Status::CANCELLED;
        }
        string value = kvs.at(request->row()).at(request->column());
        reply->set_success(true);
        reply->set_value(value);
        return Status::OK;
    }

    Status Put(ServerContext* context, const PutRequest* request, Reply* reply) override {
        if (kvs.find(request->row()) == kvs.end()) {
            unordered_map<string, string> entry;
            kvs[request->row()] = entry;
	    }
        unordered_map<string, string> kvstore_row = kvs.at(request->row());
        kvs.at(request->row())[request->column()] = request->value();
        reply->set_success(true);
        return Status::OK;
    }

    Status CPut(ServerContext* context, const CPutRequest* request, Reply* reply) override {
        if (kvs.find(request->row()) == kvs.end()) {
            reply->set_success(false);
            return Status::CANCELLED;
        }
        if (kvs.at(request->row()).find(request->column()) == kvs.at(request->row()).end()) {
            reply->set_success(false);
            return Status::CANCELLED;
        } else if (kvs.at(request->row()).at(request->column()).compare(request->current_value())) {
            reply->set_success(false);
            return Status::CANCELLED;
        }
        kvs.at(request->row())[request->column()] = request->new_value();
        reply->set_success(true);
        return Status::OK;
    }

    Status Delete(ServerContext* context, const DeleteRequest* request, Reply* reply) override {
        if (kvs.find(request->row()) != kvs.end()) {
            if (kvs.at(request->row()).find(request->column()) != kvs.at(request->row()).end()) {
                kvs.at(request->row()).erase(request->column());
                if (kvs.at(request->row()).empty()) {
                    kvs.erase(request->row());
                }
            }
        }
        reply->set_success(true);
        return Status::OK;
    }

    private:
        unordered_map<string, unordered_map<string, string>> kvs;
};