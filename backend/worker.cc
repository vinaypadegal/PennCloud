#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <pthread.h>
#include <ctime> 
#include <atomic>
#include <vector>
#include <string>
#include <fstream>
#include <stdexcept>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <grpcpp/grpcpp.h>
#include "../build/kvstore.grpc.pb.h"
#include "kvs_client.h"
#include "utils.h"
#include "logger.h"
#include "serializer.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;
using grpc::ServerWriter;
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


using namespace std;

#define DEFAULT_HOST "127.0.0.1"
#define MASTER_PORT "10000"
#define NUM_TABLETS 6
#define MAX_UPDATE_COUNT 10
#define TABLET_NOT_FOUND -1
#define ADMIN_PASS "admin"


int workerId;
vector<unordered_map<string, unordered_map<string, string>>> tablets(3);
vector<unique_ptr<Logger>> loggers;
unordered_map<string, unique_ptr<KeyValueStoreClient>> workerClientPtrs;

MasterNodeClient* masterClientPtr;
unordered_map<int, int> updateRequestCount = {{1, 0}, {2, 0}, {3, 0}};

atomic<bool> admin_workerUp(true);


void recoverAllTablets();


string getCurrentTime() {
    auto now = chrono::system_clock::now();
    auto now_sec = chrono::time_point_cast<chrono::seconds>(now);
    auto now_ms = chrono::duration_cast<chrono::milliseconds>(now - now_sec);

    time_t now_time = chrono::system_clock::to_time_t(now_sec);
    tm* timeinfo = localtime(&now_time);

    ostringstream oss;
    oss << put_time(timeinfo, "%Y-%m-%d %H:%M:%S")
        << '.' << setw(3) << setfill('0') << now_ms.count();
    return oss.str();
}


void log(string logString) {
    cout << "(" << getCurrentTime() << ") " << logString << endl;
}


unordered_map<string, unordered_map<string, string>>& getTablet(string key) {
    int t = hashToBucket(key, NUM_TABLETS);
    
    int prev1 = ((workerId + NUM_TABLETS - 2) % NUM_TABLETS) + 1;  // workerId - 1 mod 6
    int prev2 = ((workerId + NUM_TABLETS - 3) % NUM_TABLETS) + 1;  // workerId - 2 mod 6

    if (t == workerId) return tablets.at(0);
    if (t == prev1)     return tablets.at(1);
    if (t == prev2)     return tablets.at(2);

    throw runtime_error("No tablet found for key: " + key);
}


unordered_map<string, unordered_map<string, string>>& getTablet(int t) {    
    int prev1 = ((workerId + NUM_TABLETS - 2) % NUM_TABLETS) + 1;  // workerId - 1 mod 6
    int prev2 = ((workerId + NUM_TABLETS - 3) % NUM_TABLETS) + 1;  // workerId - 2 mod 6

    if (t == workerId) return tablets.at(0);
    if (t == prev1)     return tablets.at(1);
    if (t == prev2)     return tablets.at(2);

    throw runtime_error("No tablet found for number " + t);
}


int getTabletNumberOnThisWorker(string key) {
    int t = hashToBucket(key, NUM_TABLETS);
    
    int prev1 = ((workerId + NUM_TABLETS - 2) % NUM_TABLETS) + 1;  // workerId - 1 mod 6
    int prev2 = ((workerId + NUM_TABLETS - 3) % NUM_TABLETS) + 1;  // workerId - 2 mod 6

    if (t == workerId) return 1;
    if (t == prev1)     return 2;
    if (t == prev2)     return 3;

    return TABLET_NOT_FOUND;
}


int getTabletNumberOnThisWorker(int t) {    
    int prev1 = ((workerId + NUM_TABLETS - 2) % NUM_TABLETS) + 1;  // workerId - 1 mod 6
    int prev2 = ((workerId + NUM_TABLETS - 3) % NUM_TABLETS) + 1;  // workerId - 2 mod 6

    if (t == workerId) return 1;
    if (t == prev1)     return 2;
    if (t == prev2)     return 3;

    return TABLET_NOT_FOUND;
}


Logger& getLogger(string key) {
    int t = hashToBucket(key, NUM_TABLETS);
    
    int prev1 = ((workerId + NUM_TABLETS - 2) % NUM_TABLETS) + 1;  // workerId - 1 mod 6
    int prev2 = ((workerId + NUM_TABLETS - 3) % NUM_TABLETS) + 1;  // workerId - 2 mod 6

    if (t == workerId) return *loggers.at(0);
    if (t == prev1)     return *loggers.at(1);
    if (t == prev2)     return *loggers.at(2);

    throw runtime_error("No tablet found for key: " + key);
}


Logger& getLogger(int t) {    
    int prev1 = ((workerId + NUM_TABLETS - 2) % NUM_TABLETS) + 1;  // workerId - 1 mod 6
    int prev2 = ((workerId + NUM_TABLETS - 3) % NUM_TABLETS) + 1;  // workerId - 2 mod 6

    if (t == workerId) return *loggers.at(0);
    if (t == prev1)     return *loggers.at(1);
    if (t == prev2)     return *loggers.at(2);

    throw runtime_error("No tablet found for number " + t);
}


void commitCheckpoint(string& filename, string& binaryData) {
    ofstream out(filename, ios::binary | ios::trunc);
    if (!out) throw runtime_error("Failed to open file for writing");

    out.write(binaryData.data(), binaryData.size());
    out.close();
}


bool retrieveCheckpoint(string& filename, string& binaryData) {
    ifstream in(filename, ios::binary);
    if (!in) return false;

    // Seek to end to get size
    in.seekg(0, ios::end);
    streamsize size = in.tellg();
    in.seekg(0, ios::beg);

    if (size <= 0) return false;

    binaryData.resize(size);
    if (!in.read(&binaryData[0], size)) return false;

    in.close();
    return true;
}


vector<pair<string, KeyValueStoreClient*>> getReplicasForReplication(const string& key) {
    int t = hashToBucket(key, NUM_TABLETS);
    vector<pair<string, KeyValueStoreClient*>> replicas;

    if (t == workerId) {
        int replica1_id = (workerId % NUM_TABLETS) + 1;
        int replica2_id = ((workerId + 1) % NUM_TABLETS) + 1;
        string replica1_addr = DEFAULT_HOST + string(":") + to_string(10000 + replica1_id);
        string replica2_addr = DEFAULT_HOST + string(":") + to_string(10000 + replica2_id);
        replicas.emplace_back(replica1_addr, workerClientPtrs[replica1_addr].get());
        replicas.emplace_back(replica2_addr, workerClientPtrs[replica2_addr].get());
    } else if (t == ((workerId + NUM_TABLETS - 2) % NUM_TABLETS) + 1) {
        int replica_id = (workerId % NUM_TABLETS) + 1;
        string replica_addr = DEFAULT_HOST + string(":") + to_string(10000 + replica_id);
        if (workerClientPtrs.count(replica_addr)) {
            replicas.emplace_back(replica_addr, workerClientPtrs[replica_addr].get());
        }
    }

    return replicas;
}


class WorkerNodeKVS final : public KeyValueStore::Service {
    public:
        Status Get(ServerContext* context, const GetRequest* request, GetReply* reply) override {
            if (!admin_workerUp.load()) {
                log("Worker is shut down; rejecting request.");
                return Status(StatusCode::UNAVAILABLE, "Worker is shut down");
            }

            log("Received GET request with row [" + request->row() + "] column [" + request->column() + "]");

            int tabletNum = getTabletNumberOnThisWorker(request->row());
            if (tabletNum == TABLET_NOT_FOUND) {
                log("GET Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Key does not belong in this worker");
            }

            auto& kvs = getTablet(request->row());

            if (kvs.find(request->row()) == kvs.end()) {
                reply->set_success(false);
                log("GET Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Row key not found");
            }
            if (kvs.at(request->row()).find(request->column()) == kvs.at(request->row()).end()) {
                reply->set_success(false);
                log("GET Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Column key not found");
            }
            string value = kvs.at(request->row()).at(request->column());
            reply->set_success(true);
            reply->set_value(value);
            log("Successfully completed GET request on row [" + request->row() + "] column [" + request->column() + "]!\n");
            return Status::OK;
        }

        Status Put(ServerContext* context, const PutRequest* request, Reply* reply) override {
            if (!admin_workerUp.load()) {
                log("Worker is shut down; rejecting request.");
                return Status(StatusCode::UNAVAILABLE, "Worker is shut down");
            }
            
            log("Received PUT request with row [" + request->row() + "] column [" + request->column() + "]");        

            int tabletNum = getTabletNumberOnThisWorker(request->row());
            if (tabletNum == TABLET_NOT_FOUND) {
                log("PUT Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Key does not belong in this worker");
            }

            auto& kvs = getTablet(request->row());
            auto& logger = getLogger(request->row());

            if (kvs.find(request->row()) == kvs.end()) {
                unordered_map<string, string> entry;
                kvs[request->row()] = entry;
            }
            kvs.at(request->row())[request->column()] = request->value();
            reply->set_success(true);
            
            // log command
            logger.logCommand({"PUT", request->row(), request->column(), request->value(), "NULL"});

            // update counter
            updateRequestCount[tabletNum]++;

            // checkpoint tablet if required
            if (updateRequestCount[tabletNum] >= MAX_UPDATE_COUNT) {
                string checkpointFile = "../backend/checkpoints/worker" + to_string(workerId) + "_tablet" + to_string(tabletNum) + ".bin";
                string binaryData = serializeTablet(kvs);
                commitCheckpoint(checkpointFile, binaryData);
                // clear command logs
                logger.clearLog();
                updateRequestCount[tabletNum] = 0;
                log("Checkpoint committed for tablet " + to_string(tabletNum));
            }

            // replicate operation
            auto replicas = getReplicasForReplication(request->row());
            for (const auto& [addr, replica] : replicas) {
                log("Replicating PUT command on [" + addr + "]");
                string res = replica->ReplicatePut(request->row(), request->column(), request->value());
                if (res.substr(0, 3) == "ERR") {
                    log("Replication of Put on replica [" + addr + "] failed.");
                } else {
                    log("Replication of Put on replica [" + addr + "] successful!");
                }
            }

            log("Successfully completed PUT request on row [" + request->row() + "] column [" + request->column() + "]!\n");
            return Status::OK;
        }

        Status CPut(ServerContext* context, const CPutRequest* request, Reply* reply) override {
            if (!admin_workerUp.load()) {
                log("Worker is shut down; rejecting request.");
                return Status(StatusCode::UNAVAILABLE, "Worker is shut down");
            }
            
            log("Received CPUT request with row [" + request->row() + "] column [" + request->column() + "]");

            int tabletNum = getTabletNumberOnThisWorker(request->row());
            if (tabletNum == TABLET_NOT_FOUND) {
                log("CPUT Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Key does not belong in this worker");
            }

            auto& kvs = getTablet(request->row());
            auto& logger = getLogger(request->row());

            if (kvs.find(request->row()) == kvs.end()) {
                reply->set_success(false);
                log("CPUT Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Row key not found");
            }
            if (kvs.at(request->row()).find(request->column()) == kvs.at(request->row()).end()) {
                reply->set_success(false);
                log("CPUT Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Column key not found");
            }
            if (kvs.at(request->row()).at(request->column()).compare(request->current_value())) {
                reply->set_success(false);
                log("CPUT Request Failed.");
                return Status(StatusCode::FAILED_PRECONDITION, "Current value does not match specified value");
            }
            kvs.at(request->row())[request->column()] = request->new_value();
            reply->set_success(true);

            // log command
            logger.logCommand({"CPUT", request->row(), request->column(), request->current_value(), request->new_value()});

            // update counter
            updateRequestCount[tabletNum]++;;

            // checkpoint tablet if required
            if (updateRequestCount[tabletNum] >= MAX_UPDATE_COUNT) {
                string checkpointFile = "../backend/checkpoints/worker" + to_string(workerId) + "_tablet" + to_string(tabletNum) + ".bin";
                string binaryData = serializeTablet(kvs);
                commitCheckpoint(checkpointFile, binaryData);
                // clear command logs
                logger.clearLog();
                updateRequestCount[tabletNum] = 0;
                log("Checkpoint committed for tablet " + to_string(tabletNum));
            }

            // replicate operation
            auto replicas = getReplicasForReplication(request->row());
            for (const auto& [addr, replica] : replicas) {
                log("Replicating CPUT command on [" + addr + "]");
                string res = replica->ReplicateCPut(request->row(), request->column(), request->current_value(), request->new_value());
                if (res.substr(0, 3) == "ERR") {
                    log("Replication of CPut on replica [" + addr + "] failed.");
                } else {
                    log("Replication of CPut on replica [" + addr + "] successful!");
                }
            }

            log("Successfully completed CPUT request on row [" + request->row() + "] column [" + request->column() + "]!\n");
            return Status::OK;
        }

        Status Delete(ServerContext* context, const DeleteRequest* request, Reply* reply) override {
            if (!admin_workerUp.load()) {
                log("Worker is shut down; rejecting request.");
                return Status(StatusCode::UNAVAILABLE, "Worker is shut down");
            }
            
            log("Received DELETE request with row [" + request->row() + "] column [" + request->column() + "]");

            int tabletNum = getTabletNumberOnThisWorker(request->row());
            if (tabletNum == TABLET_NOT_FOUND) {
                log("DELETE Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Key does not belong in this worker");
            }

            auto& kvs = getTablet(request->row());
            auto& logger = getLogger(request->row());

            if (kvs.find(request->row()) != kvs.end()) {
                if (kvs.at(request->row()).find(request->column()) != kvs.at(request->row()).end()) {
                    kvs.at(request->row()).erase(request->column());
                    if (kvs.at(request->row()).empty()) {
                        kvs.erase(request->row());
                    }
                }
            }
            reply->set_success(true);

            // log command
            logger.logCommand({"DELETE", request->row(), request->column(), "NULL", "NULL"});

            // update counter
            updateRequestCount[tabletNum]++;

            // checkpoint tablet if required
            if (updateRequestCount[tabletNum] >= MAX_UPDATE_COUNT) {
                string checkpointFile = "../backend/checkpoints/worker" + to_string(workerId) + "_tablet" + to_string(tabletNum) + ".bin";
                string binaryData = serializeTablet(kvs);
                commitCheckpoint(checkpointFile, binaryData);
                // clear command logs
                logger.clearLog();
                updateRequestCount[tabletNum] = 0;
                log("Checkpoint committed for tablet " + to_string(tabletNum));
            }
            
            // replicate operation
            auto replicas = getReplicasForReplication(request->row());
            for (const auto& [addr, replica] : replicas) {
                log("Replicating DELETE command on [" + addr + "]");
                string res = replica->ReplicateDelete(request->row(), request->column());
                if (res.substr(0, 3) == "ERR") {
                    log("Replication of Delete to replica [" + addr + "] failed.");
                } else {
                    log("Replication of Delete on replica [" + addr + "] successful!");
                }
            }

            log("Successfully completed DELETE request on row [" + request->row() + "] column [" + request->column() + "]!\n");
            return Status::OK;
        }

        Status ReplicatePut(ServerContext* context, const PutRequest* request, Reply* reply) override {
            if (!admin_workerUp.load()) {
                log("Worker is shut down; rejecting request.");
                return Status(StatusCode::UNAVAILABLE, "Worker is shut down");
            }
            
            log("Received ReplicatePUT request with row [" + request->row() + "] column [" + request->column() + "]");

            int tabletNum = getTabletNumberOnThisWorker(request->row());
            if (tabletNum == TABLET_NOT_FOUND) {
                log("ReplicatePUT Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Key does not belong in this worker");
            }

            auto& kvs = getTablet(request->row());
            auto& logger = getLogger(request->row());

            if (kvs.find(request->row()) == kvs.end()) {
                unordered_map<string, string> entry;
                kvs[request->row()] = entry;
            }
            kvs.at(request->row())[request->column()] = request->value();
            reply->set_success(true);
            
            // log command
            logger.logCommand({"PUT", request->row(), request->column(), request->value(), "NULL"});

            // update counter
            updateRequestCount[tabletNum]++;

            // checkpoint tablet if required
            if (updateRequestCount[tabletNum] >= MAX_UPDATE_COUNT) {
                string checkpointFile = "../backend/checkpoints/worker" + to_string(workerId) + "_tablet" + to_string(tabletNum) + ".bin";
                string binaryData = serializeTablet(kvs);
                commitCheckpoint(checkpointFile, binaryData);
                // clear command logs
                logger.clearLog();
                updateRequestCount[tabletNum] = 0;
                log("Checkpoint committed for tablet " + to_string(tabletNum));
            }
            
            log("Successfully completed ReplicatePUT request on row [" + request->row() + "] column [" + request->column() + "]!\n");
            return Status::OK;
        }

        Status ReplicateCPut(ServerContext* context, const CPutRequest* request, Reply* reply) override {
            if (!admin_workerUp.load()) {
                log("Worker is shut down; rejecting request.");
                return Status(StatusCode::UNAVAILABLE, "Worker is shut down");
            }
            
            log("Received ReplicateCPUT request with row [" + request->row() + "] column [" + request->column() + "]");

            int tabletNum = getTabletNumberOnThisWorker(request->row());
            if (tabletNum == TABLET_NOT_FOUND) {
                log("ReplicateCPUT Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Key does not belong in this worker");
            }

            auto& kvs = getTablet(request->row());
            auto& logger = getLogger(request->row());

            if (kvs.find(request->row()) == kvs.end()) {
                reply->set_success(false);
                log("ReplicateCPUT Request Failed");
                return Status(StatusCode::NOT_FOUND, "Row key not found");
            }
            if (kvs.at(request->row()).find(request->column()) == kvs.at(request->row()).end()) {
                reply->set_success(false);
                log("ReplicateCPUT Request Failed");
                return Status(StatusCode::NOT_FOUND, "Column key not found");
            }
            if (kvs.at(request->row()).at(request->column()).compare(request->current_value())) {
                reply->set_success(false);
                log("ReplicateCPUT Request Failed");
                return Status(StatusCode::FAILED_PRECONDITION, "Current value does not match specified value");
            }
            kvs.at(request->row())[request->column()] = request->new_value();
            reply->set_success(true);

            // log command
            logger.logCommand({"CPUT", request->row(), request->column(), request->current_value(), request->new_value()});

            // update counter
            updateRequestCount[tabletNum]++;

            // checkpoint tablet if required
            if (updateRequestCount[tabletNum] >= MAX_UPDATE_COUNT) {
                string checkpointFile = "../backend/checkpoints/worker" + to_string(workerId) + "_tablet" + to_string(tabletNum) + ".bin";
                string binaryData = serializeTablet(kvs);
                commitCheckpoint(checkpointFile, binaryData);
                // clear command logs
                logger.clearLog();
                updateRequestCount[tabletNum] = 0;
                log("Checkpoint committed for tablet " + to_string(tabletNum));
            }
            
            log("Successfully completed ReplicateCPUT request on row [" + request->row() + "] column [" + request->column() + "]!\n");
            return Status::OK;
        }

        Status ReplicateDelete(ServerContext* context, const DeleteRequest* request, Reply* reply) override {
            if (!admin_workerUp.load()) {
                log("Worker is shut down; rejecting request.");
                return Status(StatusCode::UNAVAILABLE, "Worker is shut down");
            }
            
            log("Received ReplicateDELETE request with row [" + request->row() + "] column [" + request->column() + "]");

            int tabletNum = getTabletNumberOnThisWorker(request->row());
            if (tabletNum == TABLET_NOT_FOUND) {
                log("ReplicateDELETE Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Key does not belong in this worker");
            }

            auto& kvs = getTablet(request->row());
            auto& logger = getLogger(request->row());

            if (kvs.find(request->row()) != kvs.end()) {
                if (kvs.at(request->row()).find(request->column()) != kvs.at(request->row()).end()) {
                    kvs.at(request->row()).erase(request->column());
                    if (kvs.at(request->row()).empty()) {
                        kvs.erase(request->row());
                    }
                }
            }
            reply->set_success(true);

            // log command
            logger.logCommand({"DELETE", request->row(), request->column(), "NULL", "NULL"});

            // update counter
            updateRequestCount[tabletNum]++;;

            // checkpoint tablet if required
            if (updateRequestCount[tabletNum] >= MAX_UPDATE_COUNT) {
                string checkpointFile = "../backend/checkpoints/worker" + to_string(workerId) + "_tablet" + to_string(tabletNum) + ".bin";
                string binaryData = serializeTablet(kvs);
                commitCheckpoint(checkpointFile, binaryData);
                // clear command logs
                logger.clearLog();
                updateRequestCount[tabletNum] = 0;
                log("Checkpoint committed for tablet " + to_string(tabletNum));
            }
            
            log("Successfully completed ReplicateDELETE request on row [" + request->row() + "] column [" + request->column() + "]!\n");
            return Status::OK;
        }

        Status CopyTablet(ServerContext* context, const CopyTabletRequest* request, ServerWriter<TabletChunk>* writer) override {
            if (!admin_workerUp.load()) {
                log("Worker is shut down; rejecting request.");
                return Status(StatusCode::UNAVAILABLE, "Worker is shut down");
            }
            
            log("Received CopyTablet request for tablet [" + to_string(request->tablet_id()) + "]");

            int tabletNum = getTabletNumberOnThisWorker(request->tablet_id());
            if (tabletNum == TABLET_NOT_FOUND) {
                log("CopyTablet Request Failed.");
                return Status(StatusCode::NOT_FOUND, "Key does not belong in this worker");
            }
            
            auto &kvs = getTablet(request->tablet_id());
            string binaryData = serializeTablet(kvs);

            const size_t CHUNK_SIZE = 1024 * 1024; // 1MB
            for (size_t i = 0; i < binaryData.size(); i += CHUNK_SIZE) {
                TabletChunk chunk;
                chunk.set_data(binaryData.substr(i, CHUNK_SIZE));
                if (!writer->Write(chunk)) {
                    log("Failed to write chunk to stream.");
                    return Status(StatusCode::INTERNAL, "Stream write failed");
                }
            }

            // response->set_tablet_string(binaryData);

            log("Successfully completed CopyTablet request for tablet [" + to_string(request->tablet_id()) + "]!\n");
            return Status::OK;
        }

        Status AdminConsole(ServerContext* context, const AdminRequest* request, AdminResponse* response) override {
            log("Received AdminConsole request with command [" + request->command() + "]");

            if (request->password() != ADMIN_PASS) {
                log("Received invalid password [" + request->password() + "]");
                response->set_success(false);
                return Status(StatusCode::INVALID_ARGUMENT, "Invalid admin password");
            }
            
            if (request->command() == "SHUTDOWN") {
                if (admin_workerUp.load()) {
                    log("Shutting down worker [" + to_string(workerId) + "]");
                    admin_workerUp.store(false);
                }
            } else if (request->command() == "RESTART") {
                if (!admin_workerUp.load()) {
                    log("Restarting worker [" + to_string(workerId) + "]");
                    // recover all tablets before coming up
                    recoverAllTablets();
                    admin_workerUp.store(true);  
                }   
            } else {
                log("Unknown command received from admin console, request failed.");
                response->set_success(false);
            }

            log("Successfully completed AdminConsole request.\n");
            response->set_success(true);
            return Status::OK;
        }
};


bool replayLogs(vector<CommandLog> logs, unordered_map<string, unordered_map<string, string>>& tablet, int tabletNum) {
    log("Starting log replay for tablet [" + to_string(tabletNum) + "]");
    for (const CommandLog& cl: logs) {
        if (cl.command == "PUT") {
            log("Replaying PUT request with row [" + cl.row + "] column [" + cl.column + "]");
            if (tablet.find(cl.row) == tablet.end()) {
                unordered_map<string, string> entry;
                tablet[cl.row] = entry;
            }
            tablet.at(cl.row)[cl.column] = cl.value1;
            log("Successfully completed PUT request on row [" + cl.row + "] column [" + cl.column + "]!");
        } else if (cl.command == "CPUT") {
            log("Replaying CPUT request with row [" + cl.row + "] column [" + cl.column + "]");
            if (tablet.find(cl.row) == tablet.end()) {
                log("Could not replay CPUT request. Row key not found.");
                return false;
            }
            if (tablet.at(cl.row).find(cl.column) == tablet.at(cl.row).end()) {
                log("Could not replay CPUT request. Column key not found.");
                return false;
            }
            if (tablet.at(cl.row).at(cl.column).compare(cl.value1)) {
                log("Could not replay CPUT request. Current value does not match specified value.");
                return false;
            }
            tablet.at(cl.row)[cl.column] = cl.value2;
            log("Successfully completed PUT request on row [" + cl.row + "] column [" + cl.column + "]!");
        } else if (cl.command == "DELETE") {
            log("Replaying DELETE request with row [" + cl.row + "] column [" + cl.column + "]");
            if (tablet.find(cl.row) != tablet.end()) {
                if (tablet.at(cl.row).find(cl.column) != tablet.at(cl.row).end()) {
                    tablet.at(cl.row).erase(cl.column);
                    if (tablet.at(cl.row).empty()) {
                        tablet.erase(cl.row);
                    }
                }
            }
            log("Successfully completed DELETE request on row [" + cl.row + "] column [" + cl.column + "]!");
        } else {
            log("Error occurred while replaying logs: Unrecognised command " + cl.command);
            return false;
        }
    }
    log("Completed replaying all logs!");
    return true;
}


void recoverFromLocal(int tabletNum, int tabletOnThisWorker) {
    string binaryData;
    string checkpointFile = "../backend/checkpoints/worker" + to_string(workerId) + "_tablet" + to_string(tabletOnThisWorker) + ".bin";
    auto& logger = getLogger(tabletNum);
    auto& tablet = getTablet(tabletNum);
    
    // Try to retrieve checkpoint
    retrieveCheckpoint(checkpointFile, binaryData);
    tablet.clear();
    if (binaryData.length() > 0) {
        log("Retrieving checkpoint from [" + checkpointFile + "]");
        bool res = deserializeTablet(tablet, binaryData);
        if (!res) {
            cerr << "Error occurred while recovering tablet " << tabletNum << "from local checkpoint" << endl;
            log("Unable to recover tablet [" + to_string(tabletNum) + "] from local checkpoint");
            exit(1);
        }
    }

    // Try to replay logs
    vector<CommandLog> logs = logger.getLogs();
    if (logs.size() > 0) {
        log("Retrieving logs from [" + logger.getFilename() + "]");
        bool success = replayLogs(logs, tablet, tabletNum);
        if (!success) {
            log("Unable to recover tablet [" + to_string(tabletNum) + "] from local, facing issues while replaying logs.");
            exit(EXIT_FAILURE);
        } else {
            log("Successfully recovered tablet [" + to_string(tabletNum) + "] from local!");
        }
    }
}


void recoverAllTablets() {
    string res = masterClientPtr->WorkerLookup(workerId);
    if (res.substr(0, 3) == "ERR") {
        cerr << "Error occurred on WorkerLookup from master server: " << res << endl;
        exit(1);
    }

    stringstream ss(res);
    string host;
    int tablet = workerId;

    while (getline(ss, host, ';')) {
        int tabletNum = getTabletNumberOnThisWorker(tablet);
        if (tabletNum == TABLET_NOT_FOUND) {
            log("Error: Tablet [" + to_string(tablet) + "] does not exist on this worker.");
            exit(1);
        } else {
            if (host == "ALLDEAD") {
                recoverFromLocal(tablet, tabletNum);
            } else {
                KeyValueStoreClient* workerClient = workerClientPtrs[host].get();
                string tabletString = workerClient->CopyTablet(tablet);
                auto &kvs = getTablet(tablet);
                kvs.clear();
                deserializeTablet(kvs, tabletString);
                log("Successfully recovered tablet [" + to_string(tablet) + "] from [" + host + "]");
            }
        }
        tablet = ((tablet + NUM_TABLETS - 2) % NUM_TABLETS) + 1;
    }
}


void* heartbeatSenderThread(void* arg) {
    // int workerId = *(int *) arg;
    while (true) {
        if (admin_workerUp.load()) {
            masterClientPtr->Heartbeat(workerId, time(nullptr));
            sleep(2); // heartbeat interval
        }
    }
    return nullptr;
}


void RunMasterClient() {
    string target_address(DEFAULT_HOST + string(":") + MASTER_PORT);
    masterClientPtr = new MasterNodeClient(
        grpc::CreateChannel(target_address, grpc::InsecureChannelCredentials())
    );
}


void StartHeartbeats() {
    pthread_t thread;
    pthread_create(&thread, nullptr, heartbeatSenderThread, nullptr);
    pthread_detach(thread);
}


void RunServer(string port) {
    string server_address(DEFAULT_HOST + string(":") + port);
    WorkerNodeKVS service;

    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which communication with client takes place
    builder.RegisterService(&service);

    // Assembling the server
    unique_ptr<Server> server(builder.BuildAndStart());
    log("Server listening on port: " + server_address);

    server->Wait();
}


int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "*** 1 arguments required\n");
        exit(1);
    }

    workerId = stoi(argv[1]);
    string port = to_string(10000 + workerId);

    loggers.push_back(make_unique<Logger>("../backend/command_logs/worker" + to_string(workerId) + "_tablet1.log"));
    loggers.push_back(make_unique<Logger>("../backend/command_logs/worker" + to_string(workerId) + "_tablet2.log"));
    loggers.push_back(make_unique<Logger>("../backend/command_logs/worker" + to_string(workerId) + "_tablet3.log"));

    for (int i = 1; i <= NUM_TABLETS; ++i) {
        if (i == workerId) continue;
    
        string address = DEFAULT_HOST + string(":") + to_string(10000 + i);
        auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        workerClientPtrs[address] = make_unique<KeyValueStoreClient>(channel);
    }

    RunMasterClient();
    recoverAllTablets();
    StartHeartbeats();
    RunServer(port);
    return 0;
}


