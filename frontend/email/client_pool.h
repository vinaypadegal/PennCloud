#ifndef CLIENT_POOL_H
#define CLIENT_POOL_H

#include <unordered_map>
#include <memory>
#include <mutex>
#include <grpcpp/grpcpp.h>
#include "../../backend/kvs_client.h"
#include "../../build/kvstore.grpc.pb.h"

class ClientPool {
public:
    static ClientPool& getInstance() {
        static ClientPool instance;
        return instance;
    }

    MasterNodeClient* getMaster() {
        return masterClient.get();
    }

    KeyValueStoreClient* getWorkerClient(const std::string& rowKey) {
        std::string workerAddr = masterClient->Lookup(rowKey);
        std::lock_guard<std::mutex> lock(mtx);
        if (kvClients.find(workerAddr) == kvClients.end()) {
            kvClients[workerAddr] = std::make_unique<KeyValueStoreClient>(
                grpc::CreateChannel(workerAddr, grpc::InsecureChannelCredentials()));
        }
        return kvClients[workerAddr].get();
    }

private:
    ClientPool() {
        masterClient = std::make_unique<MasterNodeClient>(
            grpc::CreateChannel("127.0.0.1:10000", grpc::InsecureChannelCredentials()));
    }

    std::unique_ptr<MasterNodeClient> masterClient;
    std::unordered_map<std::string, std::unique_ptr<KeyValueStoreClient>> kvClients;
    std::mutex mtx;
};

#endif
