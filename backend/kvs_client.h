#ifndef KVS_CLIENT_H
#define KVS_CLIENT_H

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <grpcpp/grpcpp.h>
#include "../build/kvstore.grpc.pb.h"
#include "../build/master.grpc.pb.h"

using namespace std;

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using grpc::ClientReader;
using kvstore::KeyValueStore;
using kvstore::PutRequest;
using kvstore::GetRequest;
using kvstore::CPutRequest;
using kvstore::DeleteRequest;
using kvstore::GetReply;
using kvstore::Reply;
using kvstore::CopyTabletRequest;
// using kvstore::CopyTabletResponse;
using kvstore::TabletChunk;
using kvstore::AdminRequest;
using kvstore::AdminResponse;

using master::MasterService;
using master::LookupRequest;
using master::LookupResponse;
using master::HeartbeatRequest;
using master::HeartbeatResponse;
using master::WorkerLookupRequest;
using master::WorkerLookupResponse;
using master::AliveRequest;
using master::AliveResponse;

class KeyValueStoreClient {
    public:
        KeyValueStoreClient(shared_ptr<Channel> channel) : stub_(KeyValueStore::NewStub(channel)) {}

        string Get(string row, string column) {
            GetRequest request;
            request.set_row(row);
            request.set_column(column);

            GetReply reply;
            // Context can be used to send meta data to server or modify RPC behaviour
            ClientContext context;

            Status status = stub_->Get(&context, request, &reply);
            if (status.ok()) {
                return reply.value();
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }

        string Put(string row, string column, string value) {
            PutRequest request;
            request.set_row(row);
            request.set_column(column);
            request.set_value(value);

            Reply reply;
            ClientContext context;

            Status status = stub_->Put(&context, request, &reply);
            if (status.ok()) {
                return "PUT Successful";
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }

        string CPut(string row, string column, string curvalue, string newvalue) {
            CPutRequest request;
            request.set_row(row);
            request.set_column(column);
            request.set_current_value(curvalue);
            request.set_new_value(newvalue);

            Reply reply;
            ClientContext context;

            Status status = stub_->CPut(&context, request, &reply);
            if (status.ok()) {
                return "CPUT Successful";
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }

        string Delete(string row, string column) {
            DeleteRequest request;
            request.set_row(row);
            request.set_column(column);

            Reply reply;
            ClientContext context;

            Status status = stub_->Delete(&context, request, &reply);
            if (status.ok()) {
                return "Delete Successful";
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }

        string ReplicatePut(string row, string column, string value) {
            PutRequest request;
            request.set_row(row);
            request.set_column(column);
            request.set_value(value);

            Reply reply;
            ClientContext context;

            Status status = stub_->ReplicatePut(&context, request, &reply);
            if (status.ok()) {
                return "PUT Successful";
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }

        string ReplicateCPut(string row, string column, string curvalue, string newvalue) {
            CPutRequest request;
            request.set_row(row);
            request.set_column(column);
            request.set_current_value(curvalue);
            request.set_new_value(newvalue);

            Reply reply;
            ClientContext context;

            Status status = stub_->ReplicateCPut(&context, request, &reply);
            if (status.ok()) {
                return "CPUT Successful";
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }

        string ReplicateDelete(string row, string column) {
            DeleteRequest request;
            request.set_row(row);
            request.set_column(column);

            Reply reply;
            ClientContext context;

            Status status = stub_->ReplicateDelete(&context, request, &reply);
            if (status.ok()) {
                return "Delete Successful";
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }

        // string CopyTablet(int tablet_id) {
        //     CopyTabletRequest request;
        //     request.set_tablet_id(tablet_id);

        //     CopyTabletResponse response;
        //     ClientContext context;

        //     Status status = stub_->CopyTablet(&context, request, &response);
        //     if (status.ok()) {
        //         return response.tablet_string();
        //     } else {
        //         cout << status.error_code() << ": " << status.error_message() << endl;
        //         return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
        //     }
        // }

        string CopyTablet(int tablet_id) {
            CopyTabletRequest request;
            request.set_tablet_id(tablet_id);

            ClientContext context;
            unique_ptr<ClientReader<TabletChunk>> reader(stub_->CopyTablet(&context, request));

            string tabletData;
            TabletChunk chunk;
            while (reader->Read(&chunk)) {
                tabletData.append(chunk.data());
            }

            Status status = reader->Finish();
            if (!status.ok()) {
                cout << "CopyTablet failed: " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }

            return tabletData;
        }

        string AdminConsole(string password, string command) {
            AdminRequest request;
            request.set_password(password);
            request.set_command(command);

            AdminResponse response;
            ClientContext context;

            Status status = stub_->AdminConsole(&context, request, &response);
            if (status.ok()) {
                return "AdminConsole request successful";
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }
    
    private:
        std::unique_ptr<KeyValueStore::Stub> stub_;
};

class MasterNodeClient {
    public:
        MasterNodeClient(shared_ptr<Channel> channel) : stub_(MasterService::NewStub(channel)) {}

        string Lookup(string key) {
            LookupRequest request;
            request.set_key(key);

            LookupResponse response;
            // Context can be used to send meta data to server or modify RPC behaviour
            ClientContext context;

            Status status = stub_->Lookup(&context, request, &response);
            if (status.ok()) {
                return response.worker();
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }

        string Heartbeat(int worker_id, long long timestamp) {
            HeartbeatRequest request;
            request.set_worker_id(worker_id);
            request.set_timestamp(timestamp);

            HeartbeatResponse response;
            ClientContext context;

            Status status = stub_->Heartbeat(&context, request, &response);
            if (status.ok()) {
                return "Heartbeat Successful";
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }

        string WorkerLookup(int worker_id) {
            WorkerLookupRequest request;
            request.set_worker_id(worker_id);

            WorkerLookupResponse response;
            ClientContext context;

            Status status = stub_->WorkerLookup(&context, request, &response);

            if (status.ok()) {
                return response.primary() + ";" + response.secondary() + ";" + response.tertiary();
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }

        string Alive(int worker_id) {
            AliveRequest request;
            request.set_worker_id(worker_id);

            AliveResponse response;
            ClientContext context;

            Status status = stub_->Alive(&context, request, &response);
            if (status.ok()) {
                return response.alive() ? "Alive" : "Dead";
            } else {
                cout << status.error_code() << ": " << status.error_message() << endl;
                return "ERR " + to_string(status.error_code()) + ": " + status.error_message() + "\n";
            }
        }
    
    private:
        std::unique_ptr<MasterService::Stub> stub_;
};

#endif