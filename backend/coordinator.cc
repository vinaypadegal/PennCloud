#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <pthread.h>
#include <ctime>
#include <unordered_map>
#include <grpcpp/grpcpp.h>
#include "../build/master.grpc.pb.h"
#include "utils.h"

using namespace std;

#define DEFAULT_HOST "127.0.0.1"
#define MASTER_PORT "10000"
#define HOSTS_FILE "../backend/hosts.txt"
#define NUM_TABLETS 6

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;
using master::MasterService;
using master::LookupRequest;
using master::LookupResponse;
using master::HeartbeatRequest;
using master::HeartbeatResponse;
using master::WorkerLookupRequest;
using master::WorkerLookupResponse;
using master::AliveRequest;
using master::AliveResponse;

unordered_map<int, string> hostMap;
unordered_map<int, bool> isAlive;

pthread_mutex_t aliveMutex = PTHREAD_MUTEX_INITIALIZER;


class MasterNode final : public MasterService::Service {
public:
    MasterNode() {
        pthread_mutex_init(&heartbeatMutex_, nullptr);

        // Mark all workers as "unseen"
        auto distant_past = chrono::steady_clock::now() - std::chrono::hours(1);  // very old time
        for (int worker_id = 1; worker_id <= NUM_TABLETS; ++worker_id) {
            lastHeartbeat_[worker_id] = distant_past;
        }

        pthread_create(&heartbeatMonitorThread_, nullptr, &MasterNode::monitorThreadFunc, this);
    }

    ~MasterNode() {
        pthread_cancel(heartbeatMonitorThread_);
        pthread_join(heartbeatMonitorThread_, nullptr);
        pthread_mutex_destroy(&heartbeatMutex_);
    }

    Status Lookup(ServerContext* context, const LookupRequest* request, LookupResponse* response) override {
        int tablet = hashToBucket(request->key(), NUM_TABLETS);
        cout << request->key() << " mapped to tablet: " << tablet << endl;
        pthread_mutex_lock(&aliveMutex);
        if (isAlive[tablet]) {
            response->set_worker(hostMap.at(tablet));
        } else if (isAlive[(tablet % NUM_TABLETS) + 1]) {
            response->set_worker(hostMap.at((tablet % NUM_TABLETS) + 1));
        } else if(isAlive[((tablet + 1) % NUM_TABLETS) + 1]) {
            response->set_worker(hostMap.at(((tablet + 1) % NUM_TABLETS) + 1));
        } else {
        	pthread_mutex_unlock(&aliveMutex);
            return Status(StatusCode::UNAVAILABLE, "All workers are down");
        }
        pthread_mutex_unlock(&aliveMutex);
        return Status::OK;
    }

    Status Heartbeat(ServerContext* context, const HeartbeatRequest* request, HeartbeatResponse* response) override {
        pthread_mutex_lock(&heartbeatMutex_);
        lastHeartbeat_[request->worker_id()] = chrono::steady_clock::now();
        pthread_mutex_unlock(&heartbeatMutex_);

        response->set_ack(true);
        // cout << "Received heartbeat from worker " << request->worker_id() << endl;
        return Status::OK;
    }

    Status WorkerLookup(ServerContext* context, const WorkerLookupRequest* request, WorkerLookupResponse* response) override {
        int primary_id = request->worker_id();
        int secondary_id = ((primary_id + NUM_TABLETS - 2) % NUM_TABLETS) + 1;
        int tertiary_id = ((primary_id + NUM_TABLETS - 3) % NUM_TABLETS) + 1;
        // int secondary_id = (primary_id % NUM_TABLETS) + 1;                   
        // int tertiary_id  = ((primary_id + 1) % NUM_TABLETS) + 1;             

        string primary   = getCurrentPrimary(primary_id);
        string secondary = getCurrentPrimary(secondary_id);
        string tertiary  = getCurrentPrimary(tertiary_id);

        response->set_primary(primary);
        response->set_secondary(secondary);
        response->set_tertiary(tertiary);
        return Status::OK;
    }

    Status Alive(ServerContext* context, const AliveRequest* request, AliveResponse* response) override {
        int worker = request->worker_id();
        pthread_mutex_lock(&aliveMutex);
        if (isAlive.find(worker) == isAlive.end()) {
        	pthread_mutex_unlock(&aliveMutex);
            return Status(StatusCode::INVALID_ARGUMENT, "Only workers 1-6 exist.");
        }
        response->set_alive(isAlive[worker]);
        pthread_mutex_unlock(&aliveMutex);
        return Status::OK;
    }

private:
    unordered_map<int, chrono::steady_clock::time_point> lastHeartbeat_;
    pthread_mutex_t heartbeatMutex_;
    pthread_t heartbeatMonitorThread_;

    static void* monitorThreadFunc(void* arg) {
        MasterNode* self = static_cast<MasterNode*>(arg);

        while (true) {
            sleep(3);
            auto now = chrono::steady_clock::now();

            pthread_mutex_lock(&self->heartbeatMutex_);
            pthread_mutex_lock(&aliveMutex);
            for (const auto& [worker_id, last_seen] : self->lastHeartbeat_) {
                auto elapsed = chrono::duration_cast<chrono::seconds>(now - last_seen).count();
                if (elapsed > 3) {
                    cout << "Worker " << worker_id << " is DOWN (last seen " << elapsed << "s ago)" << std::endl;
                    isAlive[worker_id] = false;
                } else {
                    // cout << "✅ Worker " << worker_id << " is healthy" << std::endl;
                    isAlive[worker_id] = true;
                }
            }
            pthread_mutex_unlock(&aliveMutex);
            pthread_mutex_unlock(&self->heartbeatMutex_);
        }

        return nullptr;
    }

    string getCurrentPrimary(int tablet_id) {
        pthread_mutex_lock(&aliveMutex);

        const int candidateIds[] = {
            tablet_id,
            (tablet_id % NUM_TABLETS) + 1,
            ((tablet_id + 1) % NUM_TABLETS) + 1
        };

        string primary = "ALLDEAD";

        for (int candidate : candidateIds) {
            if (isAlive[candidate]) {
                primary = DEFAULT_HOST + string(":") + to_string(10000 + candidate);
                break;
            }
        }

        pthread_mutex_unlock(&aliveMutex);
        return primary;
    }
        
};


void loadHostMap(string filename) {
    ifstream infile(filename);

    if (!infile.is_open()) {
        cerr << "Error opening file: " << filename << endl;
        exit(EXIT_FAILURE);
    }

    int id;
    string address;
    while (infile >> id >> address) {
        hostMap[id] = address;
    }

    infile.close();
}


void initialiseMaster() {
    loadHostMap(HOSTS_FILE);
}


void RunServer() {
    string server_address = DEFAULT_HOST + string(":") + MASTER_PORT;
    MasterNode master;
  
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which communication with client takes place
    builder.RegisterService(&master);
  
    // Assembling the server
    unique_ptr<Server> server(builder.BuildAndStart());
    cout << "Server listening on port: " << server_address << endl;
  
    server->Wait();
}
  
  
int main(int argc, char** argv) {
    initialiseMaster();
    RunServer();
    return 0;
}
