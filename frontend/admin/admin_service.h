#ifndef ADMIN_CONSOLE_SERVICE_H
#define ADMIN_CONSOLE_SERVICE_H

#include <string>
#include <grpcpp/grpcpp.h>
#include "../../backend/kvs_client.h"
#include "../../build/master.grpc.pb.h"

// Service for admin console.
class AdminConsoleService {
public:
    // master_address e.g. "127.0.0.1:10000"
    explicit AdminConsoleService();
    ~AdminConsoleService();

    // Display home page
    string home();

    // Fetch the latest backend worker alive status
    string fetchWorkerStatuses();

    // Shutdown or restart worker
    bool handleToggle(string worker, string command);

    // Display worker page
    string workerPage(int workerId);

    // Display tablet page
    string tabletPage(int workerId, int tabletId);

    string checkHttpAlive(const string &hostport);

    bool handleFrontendToggle(const string &port, const string &action);

private:
    std::unique_ptr<MasterNodeClient> masterClient_;
    std::vector<std::unique_ptr<KeyValueStoreClient>> workerClients_;

    bool shutdownServer(int workerId);
    bool restartServer(int workerId);
};

#endif // ADMIN_CONSOLE_SERVICE_H
